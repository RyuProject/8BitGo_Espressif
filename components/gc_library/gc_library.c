/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_library.c
 * @brief 游戏库编排（从 main.c 迁移）
 *
 * 包含：云库分页加载、封面懒加载、SD 扫描、模拟器运行主循环。
 * 所有网络/下载/扫描都在后台任务里，绝不阻塞 LVGL 事件回调。
 */

#include "gc_library.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_codec_dev.h"

#include "gc_api.h"
#include "gc_store.h"
#include "gc_emu.h"
#include "gc_ui.h"
#include "gc_net.h"
#include "gc_sys.h"

static const char *TAG = "gc_library";

#define GC_PAGE_SIZE 24

/* ==================== 页面数据 ==================== */

static gc_game_list_t s_games;                                  /*!< 云库（分页累加） */
static gc_sd_rom_t   *s_sd_roms;                                /*!< SD 卡 ROM（PSRAM） */
static size_t         s_sd_count;
static int            s_pages_loaded;
static int            s_total_pages;
static volatile bool  s_fetch_running;
static volatile bool  s_sd_scanning;
static volatile bool  s_cover_running;

/* 前向声明（fetch_task 会用 cover_start，定义在下方） */
static void cover_start(void);

/* ==================== 网络状态栏 ==================== */

static void refresh_net_status(void)
{
    char ip[32];
    char text[80];

    if (gc_net_is_connected() && gc_net_get_ip(ip, sizeof(ip)) == ESP_OK) {
        snprintf(text, sizeof(text), "Wi-Fi %s", ip);
    } else if (gc_net_is_connected()) {
        snprintf(text, sizeof(text), "Wi-Fi 已连接");
    } else {
        snprintf(text, sizeof(text), "Wi-Fi 未连接");
    }
    gc_ui_update_net_status(text);
}

/* ==================== 云库分页加载 ==================== */

static void fetch_task(void *arg)
{
    int page = (int)(uintptr_t)arg;
    gc_game_list_t tmp = {0};
    int total_pages = 0;

    esp_err_t err = gc_api_fetch_game_page(&tmp, page, GC_PAGE_SIZE, &total_pages);
    if (err == ESP_OK) {
        s_pages_loaded = page;
        s_total_pages = total_pages > 0 ? total_pages : page;

        if (tmp.count > 0) {
            /* 性能：新列表的分配 + 拷贝放在锁外完成，UI 锁内只做「换指针」，
             * 避免 realloc / 大 memcpy 长时间持锁导致界面卡顿。
             * 仅 fetch 任务写 s_games（s_fetch_running 保证串行），读旧指针安全；
             * 旧数组从列表摘除后再释放。 */
            gc_game_item_t *old_items = s_games.items;
            size_t old_count = s_games.count;
            size_t base = (page == 1) ? 0 : old_count; /* 第 1 页替换整表 */
            size_t total = base + tmp.count;

            gc_game_item_t *merged = heap_caps_malloc(total * sizeof(gc_game_item_t),
                                                      MALLOC_CAP_SPIRAM);
            if (!merged) {
                merged = malloc(total * sizeof(gc_game_item_t));
            }
            if (merged) {
                if (base && old_items) {
                    memcpy(merged, old_items, base * sizeof(gc_game_item_t));
                }
                memcpy(merged + base, tmp.items, tmp.count * sizeof(gc_game_item_t));
            }

            bool swapped = false;
            if (merged && gc_ui_lock(2000)) {
                s_games.items = merged;
                s_games.count = total;
                s_games.capacity = total;
                s_games.source = tmp.source;
                gc_ui_unlock();
                free(old_items); /* 已从列表摘除，可安全释放 */
                swapped = true;
            } else if (merged) {
                free(merged); /* 没拿到 UI 锁，丢弃本次合并 */
            }

            if (!swapped) {
                ESP_LOGW(TAG, "merge page %d failed (mem or lock)", page);
                gc_ui_home_set_loading(false);
                gc_api_free_game_list(&tmp);
                s_fetch_running = false;
                vTaskDelete(NULL);
                return;
            }
        } else {
            ESP_LOGI(TAG, "cloud page %d: 0 games (all filtered / empty)", page);
        }

        gc_ui_home_set_loading(false);
        if (page == 1) {
            gc_ui_home_reload(); /* 列表被替换：整页重建（generation 变化，封面重来） */
        } else {
            gc_ui_home_rerender(); /* 只重绘内容，不打断用户当前页面 */
        }
        cover_start();
        gc_api_free_game_list(&tmp);
    } else {
        ESP_LOGW(TAG, "fetch page %d failed (%s)", page, esp_err_to_name(err));
        gc_ui_home_set_loading(false);
        if (page != 1) {
            s_total_pages = s_pages_loaded; /* 到底了，别再请求 */
        }
    }

    s_fetch_running = false;
    vTaskDelete(NULL);
}

static void fetch_start(int page)
{
    if (s_fetch_running) {
        return;
    }
    s_fetch_running = true;
    if (xTaskCreatePinnedToCore(fetch_task, "gc_fetch", 8192, (void *)(uintptr_t)page, 4,
                                NULL, 0) != pdPASS) {
        s_fetch_running = false;
        ESP_LOGE(TAG, "create fetch task failed");
    }
}

/* ==================== 封面懒加载（并发下载 worker 池） ==================== */

/* 并发路数：串行(1)最稳。C6 SDIO Wi-Fi 链路下，3 路并发 TLS 握手会挤爆
   内存/连接，偶发 mbedtls FATAL_ALERT(-0x2700) 与 ALLOC_FAILED(-0x008D)。
   封面下载本就走 CDN，串行足够，可靠性优先。 */
#define GC_COVER_WORKERS 1

/* 一批封面任务共享的分发状态（s_cover_running 保证同一时刻只有一批） */
static volatile uint32_t s_cover_gen;   /*!< 本批任务对应的列表 generation */
static volatile size_t   s_cover_next;  /*!< 下一个待处理索引（原子自增分配） */
static SemaphoreHandle_t s_cover_done;  /*!< 计数信号量：每个 worker 收工 give 一次 */

/** 下载工人：从共享游标原子领取索引并下载，成功即通知 UI 渲染 */
static void cover_worker(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t gen = s_cover_gen;
        if (gen != gc_ui_home_generation()) {
            break; /* 列表换了一轮，退出后由新一轮接管 */
        }

        /* 原子领取下一个索引：快的工人自动多干，天然负载均衡 */
        size_t i = (size_t)__atomic_fetch_add(&s_cover_next, 1, __ATOMIC_RELAXED);

        /* 短锁内拷贝 id/url 并确认下标仍有效，避免与 fetch 的换表竞争 */
        char id[GC_GAME_ID_LEN]  = {0};
        char url[GC_GAME_URL_LEN] = {0};
        if (gc_ui_lock(300)) {
            if (i >= s_games.count) {
                gc_ui_unlock();
                break; /* 本批已发完；换列表（gen 变化）由新一轮 cover_task 接管 */
            }
            strlcpy(id, s_games.items[i].id, sizeof(id));
            strlcpy(url, s_games.items[i].cover_url, sizeof(url));
            gc_ui_unlock();
        } else {
            break; /* 拿不到锁就别硬撑，交给下一轮 */
        }
        if (!url[0] || !gc_api_cover_supported(url)) {
            continue; /* WebP/AVIF 暂时无法解码，交给占位图 */
        }

        char path[GC_STORE_PATH_MAX];
        if (gc_store_cover_path(id, url, path, sizeof(path)) != ESP_OK) {
            continue;
        }
        if (!gc_store_exists(path)) {
            if (gc_api_download_file(url, path, NULL, NULL) != ESP_OK) {
                continue;
            }
        }
        gc_ui_home_cover_ready(gen, i, path);
    }

    xSemaphoreGive(s_cover_done);
    vTaskDelete(NULL);
}

/** 协调器：起 N 个工人并行下载，全部收工后按 generation 变化决定是否重启 */
static void cover_task(void *arg)
{
    (void)arg;
    uint32_t gen = gc_ui_home_generation();
    s_cover_gen = gen;
    s_cover_next = 0;

    s_cover_done = xSemaphoreCreateCounting(GC_COVER_WORKERS, 0);
    if (!s_cover_done) {
        s_cover_running = false;
        vTaskDelete(NULL);
        return;
    }

    int spawned = 0;
    for (int w = 0; w < GC_COVER_WORKERS; w++) {
        /* 解码 JPEG 封面时要走 LVGL tjpgd 解码器（decoder_info 等），
           栈开销较大，给足 16KB 避免 Stack protection fault。 */
        if (xTaskCreatePinnedToCore(cover_worker, "gc_cover", 16384, NULL, 3, NULL, 0) == pdPASS) {
            spawned++;
        }
    }
    if (spawned == 0) {
        vSemaphoreDelete(s_cover_done);
        s_cover_done = NULL;
        s_cover_running = false;
        ESP_LOGE(TAG, "create cover workers failed");
        vTaskDelete(NULL);
        return;
    }

    for (int w = 0; w < spawned; w++) {
        xSemaphoreTake(s_cover_done, portMAX_DELAY);
    }
    vSemaphoreDelete(s_cover_done);
    s_cover_done = NULL;

    bool restart = (gen != gc_ui_home_generation());
    s_cover_running = false;
    if (restart) {
        cover_start(); /* 新列表：重新起一批 */
    }
    vTaskDelete(NULL);
}

static void cover_start(void)
{
    if (s_cover_running || s_games.count == 0) {
        return;
    }
    if (!gc_store_using_sd()) {
        ESP_LOGI(TAG, "no SD card: cover cache disabled, keep placeholders");
        return;
    }
    s_cover_running = true;
    if (xTaskCreatePinnedToCore(cover_task, "gc_cover", 8192, NULL, 3, NULL, 0) != pdPASS) {
        s_cover_running = false;
        ESP_LOGE(TAG, "create cover task failed");
    }
}

/* ==================== SD 卡扫描 ==================== */

static void sd_scan_task(void *arg)
{
    (void)arg;
    size_t n = 0;
    if (gc_store_scan_sd(s_sd_roms, GC_STORE_MAX_SD_ROMS, &n) != ESP_OK) {
        n = 0;
    }
    if (gc_ui_lock(2000)) {
        s_sd_count = n;
        gc_ui_unlock();
    }
    gc_ui_home_bind(&s_games, s_sd_roms, s_sd_count, NULL, NULL, NULL);
    gc_ui_home_rerender();
    gc_ui_home_set_loading(false);

    s_sd_scanning = false;
    vTaskDelete(NULL);
}

static void sd_scan_start(void)
{
    if (s_sd_scanning) {
        return;
    }
    s_sd_scanning = true;
    if (xTaskCreatePinnedToCore(sd_scan_task, "gc_sdscan", 6144, NULL, 4, NULL, 0) != pdPASS) {
        s_sd_scanning = false;
    }
}

/* ==================== 模拟器 ==================== */

static int s_dl_last_pct = -1;

static void download_progress(size_t got, size_t total, void *ctx)
{
    (void)ctx;
    int pct = total ? (int)(got * 100 / total) : -1;
    if (pct >= 0 && pct == s_dl_last_pct) {
        return;
    }
    s_dl_last_pct = pct;

    char text[128];
    if (total) {
        snprintf(text, sizeof(text), "Downloading  %u / %u KB",
                 (unsigned)(got / 1024), (unsigned)(total / 1024));
    } else {
        snprintf(text, sizeof(text), "Downloading  %u KB", (unsigned)(got / 1024));
    }
    gc_ui_show_progress(pct, text);
}

/**
 * @brief 运行一个 ROM（阻塞直到用户退出）
 *
 * fb 直接交给 UI 作为画布数据源，零额外拷贝。
 */
static void run_rom(const gc_game_item_t *game, const char *title, const char *platform,
                    const char *meta, const char *desc, const char *rom_path)
{
    gc_platform_t p = gc_api_platform_from_str(platform);
    const gc_emu_ops_t *ops = gc_emu_get_ops(p);
    if (!ops) {
        gc_ui_show_message("Not supported", "No emulator core for this platform");
        vTaskDelay(pdMS_TO_TICKS(2600));
        gc_ui_home_refresh();
        return;
    }

    int w = 0, h = 0;
    if (gc_emu_screen_size(p, &w, &h) != ESP_OK) {
        gc_ui_show_message("Not supported", "Cannot determine screen size");
        vTaskDelay(pdMS_TO_TICKS(2200));
        gc_ui_home_refresh();
        return;
    }

    gc_emu_info_t info = {.screen_w = w, .screen_h = h, .fps = 60};
    gc_emu_t *emu = NULL;
    if (ops->open(&emu, &info) != ESP_OK || !emu) {
        gc_ui_show_message("Start failed", "Cannot create emulator (out of memory?)");
        vTaskDelay(pdMS_TO_TICKS(2200));
        gc_ui_home_refresh();
        return;
    }

    if (ops->load_rom(emu, rom_path) != ESP_OK) {
        gc_ui_show_message("Load failed", "Cannot open ROM (bad format or corrupted)");
        /* 云端下载的文件可能损坏（如重定向页），删缓存让下次重新下载；
           SD 卡用户自放的 ROM 保留 */
        if (game) {
            remove(rom_path);
        }
        vTaskDelay(pdMS_TO_TICKS(2600));
        ops->close(emu);
        gc_ui_home_refresh();
        return;
    }

    /* GB/GBC 音频输出初始化（ES8311 + I2S，立体声） */
    esp_codec_dev_handle_t spk = NULL;
    uint32_t audio_chunk = 0;
    if (p == GC_PLATFORM_GB || p == GC_PLATFORM_GBC) {
        int sr = gc_gb_audio_get_sample_rate();
        i2s_std_gpio_config_t gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK, .dout = BSP_I2S_DOUT, .din = BSP_I2S_DSIN,
        };
        i2s_std_config_t i2s_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sr),
            .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = gpio_cfg,
        };
        if (bsp_audio_init(&i2s_cfg) == ESP_OK) {
            spk = bsp_audio_codec_speaker_init();
            if (spk) {
                esp_codec_dev_sample_info_t fs = {
                    .sample_rate = sr, .channel = 2, .bits_per_sample = 16,
                };
                if (esp_codec_dev_open(spk, &fs) != ESP_OK) {
                    spk = NULL;
                } else {
                    esp_codec_dev_set_out_vol(spk, 70);
                }
            }
        }
        audio_chunk = (uint32_t)sr * 2 / 60;
        if (audio_chunk > 2048) {
            audio_chunk = 2048;
        }
    }

    size_t px = (size_t)w * h;
    uint16_t *fb = heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!fb) {
        gc_ui_show_message("Start failed", "Framebuffer alloc failed (PSRAM low)");
        vTaskDelay(pdMS_TO_TICKS(2200));
        if (spk) {
            esp_codec_dev_close(spk);
        }
        ops->close(emu);
        gc_ui_home_refresh();
        return;
    }
    memset(fb, 0, px * sizeof(uint16_t));

    gc_ui_play_enter(w, h, fb, title, platform, meta, desc);
    gc_ui_clear_exit_request();
    ESP_LOGI(TAG, "play: %s (%s)", title ? title : "?", rom_path);

    const TickType_t frame_ticks = pdMS_TO_TICKS(16);
    TickType_t last = xTaskGetTickCount();
    uint32_t frames = 0;

    /* 模拟器运行期间取「最高主频」PM 锁：关闭 DFS 降频，锁死最高主频，
       保证全程满速；退出循环后释放，恢复空闲降频省电。 */
    gc_sys_perf_lock_acquire();

    /* 整个游戏循环不触碰 Wi-Fi，连接由 gc_net 守护任务维持 */
    while (!gc_ui_exit_requested()) {
        ops->set_pad(emu, gc_ui_get_pad());
        if (ops->render_frame(emu, fb, px) == ESP_OK) {
            gc_ui_play_blit();
        }
        if (spk) {
            static int16_t abuf[2048];
            ops->audio_fill(emu, abuf, audio_chunk);
            esp_codec_dev_write(spk, abuf, (size_t)audio_chunk * 2);
        }
        if (++frames % 120 == 0) {
            refresh_net_status();
        }
        vTaskDelayUntil(&last, frame_ticks);
    }

    gc_sys_perf_lock_release();

    ESP_LOGI(TAG, "stop: %s", title ? title : "?");
    if (spk) {
        esp_codec_dev_close(spk);
    }
    gc_ui_play_exit();
    heap_caps_free(fb);
    ops->close(emu);
    (void)game;
}

/** 云库游戏：先确保 ROM 已缓存（可能要下载），再进游玩页 */
static void play_game_task(void *arg)
{
    gc_game_item_t *game = (gc_game_item_t *)arg;

    ESP_LOGI(TAG, "play task running: %s (%s)", game->title, game->platform);
    s_dl_last_pct = -1;
    gc_ui_show_progress(0, "Preparing ROM ...");
    char path[GC_STORE_PATH_MAX];
    esp_err_t err = gc_store_ensure_rom(game, path, sizeof(path), download_progress, NULL);
    ESP_LOGI(TAG, "ensure_rom -> %s (%s)", esp_err_to_name(err), path);
    if (err != ESP_OK) {
        gc_ui_show_message("Download failed",
                           "ROM is unavailable. Check network or the game entry.");
        vTaskDelay(pdMS_TO_TICKS(2600));
        gc_ui_home_refresh();
        free(game);
        vTaskDelete(NULL);
        return;
    }

    char meta[64] = {0};
    if (game->year > 0 && game->genre[0]) {
        snprintf(meta, sizeof(meta), "%d · %s", game->year, game->genre);
    } else if (game->developer[0]) {
        strlcpy(meta, game->developer, sizeof(meta));
    }

    run_rom(game, game->title, game->platform, meta, game->description, path);

    free(game);
    vTaskDelete(NULL);
}

/** SD 卡 ROM：直接进游玩页 */
static void play_rom_task(void *arg)
{
    size_t idx = (size_t)(uintptr_t)arg;
    if (idx >= s_sd_count) {
        vTaskDelete(NULL);
        return;
    }
    gc_sd_rom_t rom = s_sd_roms[idx];
    char meta[64];
    snprintf(meta, sizeof(meta), "SD 卡 · %u KB", (unsigned)(rom.size / 1024));
    run_rom(NULL, rom.title, gc_api_platform_name(rom.platform), meta, NULL, rom.path);
    vTaskDelete(NULL);
}

/* ==================== 主页回调 ==================== */

static void on_play_game(size_t index, void *ctx)
{
    (void)ctx;
    if (index >= s_games.count) {
        return;
    }
    gc_game_item_t *copy = malloc(sizeof(gc_game_item_t));
    if (!copy) {
        gc_ui_show_message("Error", "Out of memory");
        return;
    }
    *copy = s_games.items[index];
    if (xTaskCreatePinnedToCore(play_game_task, "gc_play", 8192, copy, 5, NULL, 0) != pdPASS) {
        free(copy);
        gc_ui_show_message("Error", "Cannot create emulator task");
        return;
    }
    ESP_LOGI(TAG, "play task spawned: %s (idx %u)", copy->title, (unsigned)index);
}

static void on_play_rom(size_t index, void *ctx)
{
    (void)ctx;
    if (xTaskCreatePinnedToCore(play_rom_task, "gc_play", 8192, (void *)(uintptr_t)index, 5,
                                NULL, 0) != pdPASS) {
        gc_ui_show_message("Error", "Cannot create emulator task");
    }
}

static void on_load_more(void *ctx)
{
    (void)ctx;
    if (s_pages_loaded >= s_total_pages || !gc_net_is_connected()) {
        gc_ui_home_set_loading(false);
        return;
    }
    fetch_start(s_pages_loaded + 1);
}

static void on_source_changed(gc_ui_source_t src, void *ctx)
{
    (void)ctx;
    if (src == GC_UI_SRC_SD) {
        sd_scan_start();
        return;
    }
    if (s_games.count == 0 && gc_net_is_connected()) {
        fetch_start(1);
    } else {
        cover_start();
    }
}

/* ==================== 对外 API ==================== */

esp_err_t gc_library_init(void)
{
    s_sd_roms = heap_caps_calloc(GC_STORE_MAX_SD_ROMS, sizeof(gc_sd_rom_t), MALLOC_CAP_SPIRAM);
    if (!s_sd_roms) {
        s_sd_roms = calloc(GC_STORE_MAX_SD_ROMS, sizeof(gc_sd_rom_t));
    }
    if (!s_sd_roms) {
        ESP_LOGE(TAG, "sd rom buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void gc_library_bind_ui(void)
{
    gc_ui_set_source_cb(on_source_changed, NULL);
    gc_ui_set_loadmore_cb(on_load_more, NULL);
    /* 开机同步扫描 SD 卡，填充列表并绑定到主页 */
    size_t n = 0;
    if (gc_store_scan_sd(s_sd_roms, GC_STORE_MAX_SD_ROMS, &n) == ESP_OK) {
        s_sd_count = n;
    }
    gc_ui_home_bind(&s_games, s_sd_roms, s_sd_count, on_play_game, on_play_rom, NULL);
    gc_ui_home_rerender();
    gc_ui_home_set_loading(false);
}

void gc_library_on_connected(void *ctx)
{
    (void)ctx;
    refresh_net_status();
    gc_store_init();
    s_pages_loaded = 0;
    s_total_pages = 1;
    fetch_start(1);
}
