/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_emu_gb.cpp
 * @brief Game Boy / Game Boy Color 机种实现（基于 libretro/gambatte-libretro）
 *
 * Retro-Go 的 GB 核心就是 gambatte-libretro 的 ESP32 移植分支，libretro 接口完全一致，
 * 因此这里用上游核心充当「前端」接入 gc_emu 框架：注册 libretro 回调、驱动 retro_run，
 * 把核心输出的 RGB565 帧拷贝到上层帧缓冲。
 *
 * 说明：当前 client 主循环只调 render_frame（不调 audio_fill），所以音频被收集进环形
 * 缓冲但不播放，保持与 NES 一致的静音行为；audio_fill 接口已实现，将来接上音频输出
 * 链路即可发声。
 */

#include "gc_emu.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "libretro.h"

/* ---- libretro core 入口（C 链接，由本组件链接进来的 gambatte 提供） ---- */
extern "C" {
    void retro_set_environment(retro_environment_t cb);
    void retro_set_video_refresh(retro_video_refresh_t cb);
    void retro_set_audio_sample(retro_audio_sample_t cb);
    void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
    void retro_set_input_poll(retro_input_poll_t cb);
    void retro_set_input_state(retro_input_state_t cb);
    void retro_init(void);
    void retro_deinit(void);
    unsigned retro_api_version(void);
    void retro_get_system_info(struct retro_system_info *info);
    void retro_get_system_av_info(struct retro_system_av_info *info);
    bool retro_load_game(const struct retro_game_info *game);
    bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info);
    void retro_unload_game(void);
    void retro_run(void);
    void retro_reset(void);
    size_t retro_serialize_size(void);
    bool retro_serialize(void *data, size_t size);
    bool retro_unserialize(const void *data, size_t size);
}

static const char *TAG = "gc_emu_gb";

/* 单一模拟器实例的共享状态（client 的 emu_task 同一时刻只跑一个核心） */
static uint32_t s_pad;                 /*!< gc_pad_button_t 按位 */
static uint16_t *s_frame;              /*!< 当前帧输出目标（render_frame 时设置） */
static retro_pixel_format s_pixfmt = RETRO_PIXEL_FORMAT_RGB565;

/* 音频环形缓冲：16K 采样 = 32KB。
   必须放 PSRAM —— ESP32-P4 的 sram_low 只有 ~175KB 且与 IRAM 共用，
   32KB 静态缓冲会直接把整个工程挤到链接失败（linker 报 discards section）。 */
#define GB_AUDIO_RB  16384
static int16_t *s_audio_rb;
static size_t   s_audio_w, s_audio_r;

static bool audio_rb_ensure(void)
{
    if (s_audio_rb) {
        return true;
    }
    s_audio_rb = (int16_t *)heap_caps_malloc((size_t)GB_AUDIO_RB * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM);
    if (!s_audio_rb) {
        ESP_LOGW(TAG, "audio ring buffer alloc (PSRAM) failed, audio muted");
        return false;
    }
    s_audio_w = 0;
    s_audio_r = 0;
    return true;
}

/* ---------------- libretro 回调 ---------------- */

static bool env_cb(unsigned cmd, void *data)
{
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT && data) {
        s_pixfmt = *(const retro_pixel_format *)data;
        return true;
    }
    /* 其余环境请求（核心选项 / VFS 接口等）暂不处理 */
    return false;
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (!data || !s_frame) {
        return;
    }
    if (s_pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
        const uint8_t *src = (const uint8_t *)data;
        for (unsigned y = 0; y < height; y++) {
            memcpy(s_frame + (size_t)y * width,
                   src + (size_t)y * pitch,
                   (size_t)width * 2);
        }
    } else if (s_pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
        /* 兜底：XRGB8888 -> RGB565 */
        const uint32_t *src = (const uint32_t *)data;
        for (unsigned y = 0; y < height; y++) {
            const uint32_t *row = src + (size_t)y * (pitch / 4);
            uint16_t *dst = s_frame + (size_t)y * width;
            for (unsigned x = 0; x < width; x++) {
                uint32_t p = row[x];
                uint16_t r5 = (uint16_t)((p >> 16) & 0x1F);
                uint16_t g6 = (uint16_t)((p >>  8) & 0x3F);
                uint16_t b5 = (uint16_t)((p >>  0) & 0x1F);
                dst[x] = (uint16_t)(r5 | (g6 << 5) | (b5 << 11));
            }
        }
    }
    /* 其它格式：直接忽略（保持静默） */
}

/* 去重：gambatte 音频只走一条路径（blipper -> audio_batch_cb，
   或 cc_resampler -> audio_out_buffer_write），用标志避免双填 */
static int s_audio_path = 0; /* 0=unknown, 1=batch, 2=out */

static void audio_rb_push(const int16_t *src, size_t n)
{
    if (!audio_rb_ensure()) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        s_audio_rb[s_audio_w] = src[i];
        s_audio_w = (s_audio_w + 1) % GB_AUDIO_RB;
    }
}

/* libretro 的 batch 回调原型是 size_t (*)(const int16_t *, size_t)，
   返回“实际消费的帧数”，必须按原型实现，否则编译期就会报函数指针类型不匹配。 */
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    if (s_audio_path == 2) {
        return frames; /* 核心走 audio_out_buffer_write 路径，跳过 */
    }
    s_audio_path = 1;
    audio_rb_push(data, frames * 2);
    return frames;
}

/* 平台音频出口（core 在启用 cc_resampler 时调用）；默认走 audio_batch_cb。
   必须提供定义（core 只声明不定义），这里填环形缓冲供主循环取走。 */
extern "C" void audio_out_buffer_write(int16_t *samples, size_t num_samples)
{
    if (s_audio_path == 1) {
        return;
    }
    s_audio_path = 2;
    audio_rb_push(samples, num_samples);
}

static void input_poll_cb(void) { /* 状态由 set_pad 维护 */ }

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) {
        return 0;
    }
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_A:      return (s_pad & GC_PAD_A)      ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_B:      return (s_pad & GC_PAD_B)      ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return (s_pad & GC_PAD_SELECT) ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_START:  return (s_pad & GC_PAD_START)  ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_UP:     return (s_pad & GC_PAD_UP)     ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (s_pad & GC_PAD_DOWN)   ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (s_pad & GC_PAD_LEFT)   ? 1 : 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (s_pad & GC_PAD_RIGHT)  ? 1 : 0;
    default:                            return 0;
    }
}

/* ---------------- 机种私有上下文 ---------------- */
typedef struct {
    uint8_t *rom;
    size_t   rom_size;
    int      w;
    int      h;
} gb_t;

/* ---------------- gc_emu_ops_t 回调 ---------------- */

static esp_err_t gb_open(gc_emu_t **emu, const gc_emu_info_t *info)
{
    ESP_RETURN_ON_FALSE(emu, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    gb_t *g = (gb_t *)heap_caps_calloc(1, sizeof(gb_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(g, ESP_ERR_NO_MEM, TAG, "no mem for ctx");

    g->w = (info && info->screen_w) ? info->screen_w : 160;
    g->h = (info && info->screen_h) ? info->screen_h : 144;

    /* 安装前端回调，再初始化核心 */
    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    *emu = (gc_emu_t *)g;
    return ESP_OK;
}

static esp_err_t gb_load_rom(gc_emu_t *emu, const char *rom_path)
{
    ESP_RETURN_ON_FALSE(emu && rom_path, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    gb_t *g = (gb_t *)emu;

    FILE *f = fopen(rom_path, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "open %s failed", rom_path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        return ESP_FAIL;
    }
    fclose(f);

    g->rom = buf;
    g->rom_size = (size_t)size;

    struct retro_game_info gi;
    gi.path = rom_path;
    gi.data = g->rom;
    gi.size = g->rom_size;
    gi.meta = NULL;

    if (!retro_load_game(&gi)) {
        ESP_LOGE(TAG, "retro_load_game failed for %s", rom_path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "loaded ROM %s (%zu B)", rom_path, g->rom_size);
    return ESP_OK;
}

static esp_err_t gb_reset(gc_emu_t *emu)
{
    (void)emu;
    retro_reset();
    return ESP_OK;
}

static void gb_set_pad(gc_emu_t *emu, uint32_t state)
{
    (void)emu;
    s_pad = state;
}

static esp_err_t gb_render_frame(gc_emu_t *emu, uint16_t *fb, size_t fb_px)
{
    ESP_RETURN_ON_FALSE(emu && fb, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    gb_t *g = (gb_t *)emu;
    ESP_RETURN_ON_FALSE(fb_px >= (size_t)(g->w * g->h), ESP_ERR_INVALID_SIZE, TAG, "fb too small");

    s_frame = fb;
    retro_run();
    s_frame = NULL;
    return ESP_OK;
}

static esp_err_t gb_audio_fill(gc_emu_t *emu, int16_t *buf, size_t samples)
{
    (void)emu;
    if (!buf || !samples) {
        return ESP_OK;
    }
    if (!s_audio_rb) {
        memset(buf, 0, samples * sizeof(int16_t));
        return ESP_OK;
    }
    for (size_t i = 0; i < samples; i++) {
        if (s_audio_r != s_audio_w) {
            buf[i] = s_audio_rb[s_audio_r];
            s_audio_r = (s_audio_r + 1) % GB_AUDIO_RB;
        } else {
            buf[i] = 0;
        }
    }
    return ESP_OK;
}

static void gb_close(gc_emu_t *emu)
{
    gb_t *g = (gb_t *)emu;
    if (!g) {
        return;
    }
    retro_unload_game();
    retro_deinit();
    if (g->rom) {
        free(g->rom);
    }
    free(g);
}

/* ---------------- ops 表 + 注册 ----------------
   注意：GB 与 GBC 必须用**两个独立的 ops 结构体**。
   gc_emu 注册表以 `ops->platform` 作为身份标识，如果两个机种指向同一个结构体
   并在注册时修改它的 platform，第二次注册会把自己当成「同 platform 重复注册」
   从而覆盖掉 GB 条目 —— 表现为 GB 游戏全部「未支持」。
   回调实现可以共用，只有 platform 字段必须各自固定。 */
static const gc_emu_ops_t s_ops_gb = {
    .platform     = GC_PLATFORM_GB,
    .open         = gb_open,
    .load_rom     = gb_load_rom,
    .reset        = gb_reset,
    .set_pad      = gb_set_pad,
    .render_frame = gb_render_frame,
    .audio_fill   = gb_audio_fill,
    .close        = gb_close,
};

static const gc_emu_ops_t s_ops_gbc = {
    .platform     = GC_PLATFORM_GBC,
    .open         = gb_open,
    .load_rom     = gb_load_rom,
    .reset        = gb_reset,
    .set_pad      = gb_set_pad,
    .render_frame = gb_render_frame,
    .audio_fill   = gb_audio_fill,
    .close        = gb_close,
};

int gc_gb_audio_get_sample_rate(void)
{
    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    int sr = (int)(av.timing.sample_rate + 0.5f);
    return sr > 0 ? sr : 44100;
}

esp_err_t gc_emu_gb_register(void)
{
    /* GB 与 GBC 共用同一核心（gambatte 按 ROM 头自动识别 GBC），
       分别注册两个 platform（两个独立 ops），名字统一为 gambatte，
       设置页里 GB/GBC 会显示成同一个核心的两条机种。 */
    gc_emu_register_ops_named("gambatte", &s_ops_gb);
    gc_emu_register_ops_named("gambatte", &s_ops_gbc);
    return ESP_OK;
}
