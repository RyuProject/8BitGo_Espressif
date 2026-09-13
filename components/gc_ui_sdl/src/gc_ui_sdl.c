/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_ui_sdl.c
 * @brief SDL3 界面层（替代 LVGL 版 gc_ui）
 *
 * M3/M4 范围：
 *   - 游玩页：模拟器画布整数倍最近邻缩放 + 虚拟手柄（按压态）+ 简介浮层 + 退出
 *   - 设置页：Wi-Fi / 蓝牙 / 核心 / 存储 / 诊断 / 旋转 / GitHub / About
 *   - Wi-Fi 配网：扫描列表 -> 软键盘输密码 -> 连接/结果页
 *   - 通用状态页（show_message / show_progress：标题 + 文本 + 进度条）
 *
 * 页面均为即时模式渲染；触摸坐标与呈现旋转严格互逆（见 map_touch）。
 */

#include "gc_ui.h"

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "SDL3/SDL.h"
#include "esp_bsp_sdl.h"
#include "gc_emu.h"
#include "gc_prefs.h"
#include "esp_pthread.h"
#include <pthread.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ui_gfx.h"

/* stb_image（stb_impl.c 内实现）：封面 JPEG/PNG 解码 */
extern unsigned char *stbi_load(char const *filename, int *x, int *y, int *comp,
                                int req_comp);
extern void stbi_image_free(void *retval_from_stbi_load);

/* 封面缓存：解码后预缩放到卡片封面尺寸的 RGB565 槽位（按 index 取模） */
#define COVER_SLOTS 40
#define COVER_MAX_W 320
#define COVER_MAX_H 80
typedef struct {
    bool     valid;
    int      index;
    uint32_t gen;
    uint16_t *pix;
    int      w, h;
} cover_slot_t;
static cover_slot_t s_covers[COVER_SLOTS];

static void covers_invalidate(void)
{
    for (int i = 0; i < COVER_SLOTS; i++) {
        if (s_covers[i].pix) {
            heap_caps_free(s_covers[i].pix);
            s_covers[i].pix = NULL;
        }
        s_covers[i].valid = false;
    }
}

static const char *TAG = "gc_ui_sdl";

#define GC_PREFS_KEY_ROTATION "lcd_rotation"

/* ==================== 页面 ==================== */

typedef enum {
    PAGE_BOOT,
    PAGE_HOME,
    PAGE_PLAY,
    PAGE_SETTINGS,
    PAGE_WIFI_SCAN,
    PAGE_WIFI_PASSWORD,
    PAGE_WIFI_STATUS,
    PAGE_TEXT,
} ui_page_id_t;

static ui_page_id_t s_page = PAGE_BOOT;

/* 通用信息页（Cores / Storage / Diag / About ...） */
static struct {
    char title[48];
    char lines[8][72];
    int  n;
} s_text_page;

/* 通用状态页（message / progress / wifi 状态） */
static struct {
    char title[48];
    char text[80];
    int  percent;      /* <0 无进度条 */
} s_status_page;
static ui_page_id_t s_status_back = PAGE_HOME;  /* 状态页返回键的去向 */

/* ==================== 基础状态 ==================== */

static SDL_Window  *s_win;
static SDL_Surface *s_surf;          /* 窗口表面 = 面板原生方向 */
static SDL_Surface *s_log_surf;      /* 逻辑方向渲染面（rot!=0 时使用） */
static void        *s_log_pixels;
static int s_w, s_h;                 /* 面板物理分辨率 */
static uint8_t s_rotation;

static SemaphoreHandle_t s_lock;
static char s_status[80];

/* 启动页 */
static struct {
    int  percent;
    char text[80];
} s_boot;

/* toast */
static char    s_toast[80];
static int64_t s_toast_until_ms;

/* 设置页动作回调（gc_ui_show_settings 注册；Wi-Fi 行触发 GC_UI_SET_WIFI） */
static gc_ui_settings_cb s_settings_action_cb;
static void *s_settings_action_ctx;

/* ==================== 主页状态 ==================== */

static struct {
    gc_ui_source_t      src;
    const gc_game_list_t *games;
    const gc_sd_rom_t  *roms;
    size_t              rom_n;
    gc_ui_index_cb      play_game_cb;
    gc_ui_index_cb      play_rom_cb;
    void               *bind_ctx;
    gc_ui_source_cb     source_cb;
    void               *source_ctx;
    gc_ui_loadmore_cb   load_cb;
    void               *load_ctx;
    gc_ui_home_settings_cb settings_cb;
    void               *settings_ctx;
    uint32_t            gen;
    int                 scroll;
    bool                loading;
} s_home;

/* ==================== 游玩页状态 ==================== */

static struct {
    int w, h;
    const uint16_t *fb;
    char title[48];
    char meta[32];
    char desc[160];
    int  scale;
    bool info_open;
} s_play;

static volatile uint32_t s_pad_state;
static volatile bool     s_exit_req;

/* ==================== Wi-Fi 配网状态 ==================== */

static struct {
    gc_net_scan_result_t scan;        /* 扫描结果副本 */
    int                  scroll;
    char                 ssid[36];
    char                 pass[49];
    bool                 ssid_editable;
    gc_ui_ap_selected_cb ap_cb;
    void                *ap_ctx;
    gc_ui_password_cb    pw_cb;
    void                *pw_ctx;
    gc_ui_rescan_cb      rescan_cb;
    void                *rescan_ctx;
    gc_ui_rescan_cb      manual_cb;
    void                *manual_ctx;
    gc_ui_cancel_cb      cancel_cb;
    void                *cancel_ctx;
} s_wifi;

/* ==================== 触摸（逻辑坐标） ==================== */

static bool t_down;
static bool t_press_edge, t_release_edge;
static int  t_lx, t_ly;
static int  press_x, press_y;
static bool press_moved;
static int  press_id;
#define UI_ID_SCROLL 10000

/* 临时调试：触摸事件计数（屏幕可见，免串口） */
static volatile uint32_t s_dbg_downs, s_dbg_ups, s_dbg_motions;
static char s_dbg_click[48] = "no card tap yet";   /* 最近一次卡片点击诊断 */

static int iabs(int v) { return v < 0 ? -v : v; }

static bool pt_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

static bool ui_clicked(int id, int x, int y, int w, int h)
{
    /* 注意不要提前 return：SDL 事件批处理会把「按下+抬起」放在同一帧
       （快速点按 <30ms），认领后必须继续执行同帧的释放判定，
       否则快 tap 的点击永远丢失。
       点击判定以「按压点」为准（抬手前手指几乎都会滑几像素，
       若要求抬起位置仍在矩形内会造成随机失败）。 */
    if (t_press_edge && t_down && press_id == 0 && pt_in(t_lx, t_ly, x, y, w, h)) {
        press_id = id;
        press_x = t_lx;
        press_y = t_ly;
        press_moved = false;
    }
    if (t_down && press_id == id) {
        if (iabs(t_lx - press_x) + iabs(t_ly - press_y) > 12) {
            press_moved = true;
        }
    }
    if (t_release_edge && press_id == id) {
        press_id = 0;
        return !press_moved;
    }
    return false;
}

static bool ui_item_clicked(int id, int x, int y, int w, int h)
{
    /* 同 ui_clicked：以按压点为准，抬起位置不要求精确落回 */
    if (t_release_edge && press_id == UI_ID_SCROLL && !press_moved &&
        pt_in(press_x, press_y, x, y, w, h)) {
        press_id = 0;
        return true;
    }
    return false;
}

static bool ui_button(int id, int x, int y, int w, int h,
                      const char *label, bool primary)
{
    bool pressed = t_down && press_id == id;
    Uint32 bg = primary ? ui_col_brand : ui_col_surface_hi;
    if (pressed) {
        bg = ui_col_brand_press;
    }
    ui_fill_round(x, y, w, h, 8, bg);
    int tx = x + (w - ui_text_w(label, 2)) / 2;
    int ty = y + (h - 8 * 2) / 2;
    ui_text(tx, ty, label, primary ? ui_col_text : ui_col_text_sub, 2);
    return ui_clicked(id, x, y, w, h);
}

/* 设置页等使用的整行条目 */
static bool ui_row(int id, int x, int y, int w, int h,
                   const char *key, const char *val)
{
    ui_fill_round(x, y, w, h, 8, ui_col_surface);
    ui_text_ellipsis(x + 14, y + (h - 16) / 2, key, w / 2, ui_col_text, 2);
    if (val && val[0]) {
        int vw = ui_text_w(val, 1);
        ui_text_ellipsis(x + w - vw - 40, y + (h - 8) / 2, val,
                         w - 220 > 0 ? w - 220 : w / 3, ui_col_mute, 1);
        ui_text(x + w - 30, y + (h - 8) / 2, ">", ui_col_mute, 1);
    }
    return ui_clicked(id, x, y, w, h);
}

static void show_toast(const char *text)
{
    strlcpy(s_toast, text, sizeof(s_toast));
    s_toast_until_ms = esp_log_timestamp() + 2000;
}

static void draw_toast(void)
{
    if (!s_toast[0] || esp_log_timestamp() > s_toast_until_ms) {
        return;
    }
    const int scale = 2;
    int tw = ui_text_w(s_toast, scale) + 32;
    int th = 8 * scale + 24;
    int x = (ui_g.w - tw) / 2;
    int y = ui_g.h - th - 60;
    ui_fill_round(x, y, tw, th, 10, ui_col_surface_hi);
    ui_text(x + 16, y + 12, s_toast, ui_col_text, scale);
}

/* 通用页眉：标题 + 返回按钮；返回点击返回 true */
static bool page_header(int id_base, const char *title)
{
    const int W = ui_g.w;
    ui_fill(0, 0, W, 56, ui_col_header);
    if (ui_button(id_base, 8, 8, 96, 40, "Back", false)) {
        return true;
    }
    int tx = 120;
    ui_text_ellipsis(tx, (56 - 16) / 2, title, W - tx - 130, ui_col_text, 2);
    return false;
}

/* 简易按宽度折行（词边界优先），最多 out_n 行 */
static void wrap_text(const char *s, int max_chars, char out[][72], int out_n, int *out_cnt)
{
    *out_cnt = 0;
    int line = 0, col = 0;
    out[0][0] = '\0';
    while (*s && line < out_n) {
        if (*s == ' ' && col == 0) {
            s++;
            continue;
        }
        if (col >= max_chars - 1) {
            out[line][col] = '\0';
            line++;
            if (line >= out_n) {
                break;
            }
            col = 0;
            out[line][0] = '\0';
            if (*s == ' ') {
                s++;
            }
            continue;
        }
        out[line][col++] = *s++;
    }
    if (line < out_n) {
        out[line][col] = '\0';
        *out_cnt = line + 1;
    } else {
        *out_cnt = line;
    }
}

/* ==================== 启动页 ==================== */

static void page_boot(void)
{
    const int W = ui_g.w, H = ui_g.h;

    ui_fill(0, 0, W, H, ui_col_bg);

    const int bw = 240;
    const int bx = (W - bw) / 2;
    const int by = H / 3 - bw;
    ui_fill_round(bx, by, bw, bw, 48, ui_col_brand);
    ui_text((W - 8 * 16) / 2, by + (bw - 8 * 16) / 2, "8", ui_col_text, 16);

    ui_text_centered(H / 3 + 60, "8BitGo", ui_col_text, 6);
    ui_text_centered(H / 3 + 150, "RETRO GAME STATION", ui_col_mute, 2);

    const int pw = 400;
    const int py = H * 2 / 3;
    const int pxx = (W - pw) / 2;
    ui_fill_round(pxx, py, pw, 18, 8, ui_col_surface_hi);
    int pct = s_boot.percent < 0 ? 33 : s_boot.percent;
    if (pct > 100) {
        pct = 100;
    }
    if (pct > 0) {
        ui_fill_round(pxx + 3, py + 3, (pw - 6) * pct / 100, 12, 6, ui_col_brand);
    }

    char line[96];
    if (s_boot.percent >= 0) {
        snprintf(line, sizeof(line), "%d%% %s", s_boot.percent,
                 s_boot.text[0] ? s_boot.text : "");
    } else {
        snprintf(line, sizeof(line), "%s", s_boot.text);
    }
    ui_text_centered(py + 64, line, ui_col_mute, 2);

    ui_text_centered(H - 80, "v1.0 SDL3 UI", ui_col_mute, 1);
}

/* ==================== 主页 ==================== */

static int home_content_height(int view_h, int cols, int item_h)
{
    size_t total = (s_home.src == GC_UI_SRC_CLOUD)
                       ? (s_home.games ? s_home.games->count : 0)
                       : s_home.rom_n;
    if (s_home.src == GC_UI_SRC_CLOUD && total == 0) {
        return 120;
    }
    int rows = (s_home.src == GC_UI_SRC_CLOUD) ? ((int)total + cols - 1) / cols
                                               : (int)total;
    int h = rows * (item_h + 8) + 8;
    return h > view_h ? h : view_h;
}

static void page_home(void)
{
    const int W = ui_g.w, H = ui_g.h;
    const int hdr_h = 56;
    const int tab_h = 56;

    /* 整屏清底：底部统计/调试文字长度会变，不清底会留残影 */
    ui_fill(0, 0, W, H, ui_col_bg);
    ui_fill(0, 0, W, hdr_h, ui_col_header);
    ui_text(16, (hdr_h - 24) / 2, "8BitGo", ui_col_text, 3);
    int st_w = ui_text_w(s_status, 1);
    int st_x = (W - 104 - 24 - st_w > 200) ? W - 104 - 24 - st_w : 200;
    ui_text_ellipsis(st_x, (hdr_h - 8) / 2, s_status, W - 104 - 24 - st_x,
                     ui_col_mute, 1);
    if (ui_button(1, W - 104, 8, 96, 40, "Settings", false)) {
        s_page = PAGE_SETTINGS;
        return;
    }

    const int tab_y = hdr_h + 8;
    if (ui_button(2, 8, tab_y, W / 2 - 16, tab_h - 16, "Cloud",
                  s_home.src == GC_UI_SRC_CLOUD)) {
        s_home.src = GC_UI_SRC_CLOUD;
        s_home.scroll = 0;
        if (s_home.source_cb) {
            s_home.source_cb(GC_UI_SRC_CLOUD, s_home.source_ctx);
        }
    }
    if (ui_button(3, W / 2 + 8, tab_y, W / 2 - 16, tab_h - 16, "SD Card",
                  s_home.src == GC_UI_SRC_SD)) {
        s_home.src = GC_UI_SRC_SD;
        s_home.scroll = 0;
        if (s_home.source_cb) {
            s_home.source_cb(GC_UI_SRC_SD, s_home.source_ctx);
        }
    }

    const int ly = hdr_h + tab_h + 4;
    const int lh = H - ly - 28;
    const int lw = W - 16;
    ui_fill(0, ly, W, lh, ui_col_bg);

    const bool cloud = (s_home.src == GC_UI_SRC_CLOUD);
    const int cols = lw >= 1000 ? 2 : 1;
    const int cw = (lw - (cols + 1) * 8) / cols;
    const int ch = 150;
    const int row_h_sd = 56;

    size_t total = cloud ? (s_home.games ? s_home.games->count : 0) : s_home.rom_n;
    int content_h = home_content_height(lh, cols, cloud ? ch : row_h_sd);
    int max_scroll = content_h > lh ? content_h - lh : 0;

    static int drag_base_scroll;
    static int drag_base_y;
    if (t_press_edge && t_down && press_id == 0 && pt_in(t_lx, t_ly, 0, ly, W, lh)) {
        press_id = UI_ID_SCROLL;
        press_x = t_lx;
        press_y = t_ly;
        press_moved = false;
        drag_base_scroll = s_home.scroll;
        drag_base_y = t_ly;
    }
    if (t_down && press_id == UI_ID_SCROLL) {
        if (iabs(t_lx - press_x) + iabs(t_ly - press_y) > 10) {
            press_moved = true;
        }
        int want = drag_base_scroll + (drag_base_y - t_ly);
        if (want < 0) {
            want = 0;
        }
        if (want > max_scroll) {
            want = max_scroll;
        }
        s_home.scroll = want;
    }
    /* 注意：抬起时不能在这里清 press_id——下面条目的点击判定需要它，
       未命中任何条目时由 page_home 末尾统一清理 */

    /* 滚动到底触发加载更多（云库）；以 total 变化防重复触发 */
    static bool load_fired;
    static size_t fired_total;
    if (cloud && !s_home.loading && s_home.load_cb && !load_fired &&
        s_home.scroll >= max_scroll - 8) {
        load_fired = true;
        fired_total = total;
        s_home.load_cb(s_home.load_ctx);
    }
    if (load_fired && total != fired_total) {
        load_fired = false;
    }

    ui_set_clip(0, ly, W, lh);

    if (cloud && total == 0) {
        const char *msg = "No games yet. Connect Wi-Fi in Settings.";
        ui_text_centered(ly + lh / 2, msg, ui_col_mute, 2);
    } else if (cloud) {
        for (size_t i = 0; i < total; i++) {
            const gc_game_item_t *g = &s_home.games->items[i];
            int col = (int)i % cols;
            int row = (int)i / cols;
            int x = 8 + col * (cw + 8);
            int y = ly - s_home.scroll + 8 + row * (ch + 8);

            ui_fill_round(x, y, cw, 80, 8, ui_col_surface_hi);
            cover_slot_t *cs = &s_covers[i % COVER_SLOTS];
            bool has_img = cs->valid && cs->index == (int)i &&
                           cs->gen == s_home.gen && cs->pix;
            if (has_img) {
                ui_blit(x + (cw - cs->w) / 2, y + (80 - cs->h) / 2, cs->w, cs->h,
                        cs->pix);
            } else {
                char initial[2] = { g->title[0] ? g->title[0] : '?', '\0' };
                ui_text(x + 12, y + 22, initial, ui_col_mute, 4);
                ui_text_ellipsis(x + 12, y + 60,
                                 gc_api_platform_name(gc_api_platform_from_str(g->platform)),
                                 cw - 24, ui_col_mute, 1);
            }

            ui_text_ellipsis(x, y + 88, g->title, cw, ui_col_text, 2);
            char meta[64];
            if (g->year > 0) {
                snprintf(meta, sizeof(meta), "%d %s", g->year, g->genre);
            } else {
                snprintf(meta, sizeof(meta), "%s", g->genre);
            }
            ui_text_ellipsis(x, y + 108, meta, cw - 60, ui_col_mute, 1);
            bool playable = gc_emu_get_ops(gc_api_platform_from_str(g->platform)) != NULL;
            ui_text(x + cw - 52, y + 108, playable ? "OK" : "N/A",
                    playable ? ui_col_brand : ui_col_mute, 1);

            if (ui_item_clicked(100 + (int)i, x, y, cw, ch - 8)) {
                ESP_LOGI(TAG, "card %u clicked playable=%d", (unsigned)i, playable);
                snprintf(s_dbg_click, sizeof(s_dbg_click), "card %u pl=%d cb=%d",
                         (unsigned)i, playable, s_home.play_game_cb != NULL);
                if (playable) {
                    if (s_home.play_game_cb) {
                        s_dbg_click[0] = '\0'; /* cb 已触发，页面应切走 */
                        s_home.play_game_cb(i, s_home.bind_ctx);
                    }
                } else {
                    show_toast("Core not connected");
                }
            }
        }
    } else {
        for (size_t i = 0; i < total; i++) {
            const gc_sd_rom_t *r = &s_home.roms[i];
            int x = 8;
            int y = ly - s_home.scroll + 8 + (int)i * (row_h_sd + 8);
            ui_fill_round(x, y, cw, row_h_sd, 8, ui_col_surface);
            ui_text_ellipsis(x + 12, y + (row_h_sd - 16) / 2, r->title, cw - 160,
                             ui_col_text, 2);
            char sub[48];
            snprintf(sub, sizeof(sub), "%s %u KB",
                     gc_api_platform_name(r->platform),
                     (unsigned)(r->size / 1024));
            ui_text_ellipsis(x + cw - 150, y + (row_h_sd - 8) / 2, sub, 140,
                             ui_col_mute, 1);

            if (ui_item_clicked(200 + (int)i, x, y, cw, row_h_sd)) {
                if (s_home.play_rom_cb) {
                    s_home.play_rom_cb(i, s_home.bind_ctx);
                }
            }
        }
    }

    ui_set_clip(0, 0, W, H);

    /* 条目判定结束：抬起但未命中任何条目时清理滚动按压态 */
    if (t_release_edge && press_id == UI_ID_SCROLL) {
        press_id = 0;
    }

    char foot[80];
    if (cloud) {
        snprintf(foot, sizeof(foot), "%u games%s",
                 (unsigned)total, s_home.loading ? " - loading..." : "");
    } else {
        snprintf(foot, sizeof(foot), "%u ROMs on SD", (unsigned)total);
    }
    ui_text_centered(H - 20, foot, ui_col_mute, 1);

    /* 临时调试行：触摸事件计数 + 当前逻辑坐标 + 最近卡片点击诊断 */
    char dbg[80];
    snprintf(dbg, sizeof(dbg), "D%u U%u M%u t(%d,%d)%s %s", (unsigned)s_dbg_downs,
             (unsigned)s_dbg_ups, (unsigned)s_dbg_motions, t_lx, t_ly,
             t_down ? "DN" : "", s_dbg_click);
    ui_text_ellipsis(4, H - 12, dbg, W - 8, ui_col_brand, 1);

    draw_toast();
}

/* ==================== 游玩页 ==================== */

typedef struct {
    int x, y, w, h;
    const char *label;
    uint32_t bit;
} pad_btn_t;

static void page_play(void)
{
    const int W = ui_g.w, H = ui_g.h;
    const int hdr_h = 48;
    const int pad_h = 190;

    ui_fill(0, 0, W, H, ui_col_bg);
    ui_fill(0, 0, W, hdr_h, ui_col_header);

    if (ui_button(30, 8, 4, 96, 40, "Exit", false)) {
        s_exit_req = true;
    }
    ui_text_ellipsis(120, (hdr_h - 16) / 2, s_play.title, W - 120 - 230,
                     ui_col_text, 2);
    if (ui_button(31, W - 104, 4, 96, 40, "Info", false)) {
        s_play.info_open = !s_play.info_open;
    }

    /* 画布：整数倍最近邻缩放（16ms 帧预算内 CPU 足够） */
    const int stage_y = hdr_h;
    const int stage_h = H - hdr_h - pad_h;
    int scale = (W - 24) / (s_play.w > 0 ? s_play.w : 1);
    int sy = (stage_h - 16) / (s_play.h > 0 ? s_play.h : 1);
    if (sy < scale) {
        scale = sy;
    }
    if (scale < 1) {
        scale = 1;
    }
    if (scale > 6) {
        scale = 6;
    }
    s_play.scale = scale;
    const int cw = s_play.w * scale, chh = s_play.h * scale;
    const int cx = (W - cw) / 2;
    const int cy = stage_y + (stage_h - chh) / 2;

    if (s_play.fb) {
        for (int row = 0; row < s_play.h; row++) {
            const Uint16 *srow = s_play.fb + (size_t)row * s_play.w;
            for (int dy = 0; dy < scale; dy++) {
                Uint16 *drow = (Uint16 *)ui_g.s->pixels +
                               (cy + row * scale + dy) * (ui_g.s->pitch >> 1);
                for (int col = 0; col < s_play.w; col++) {
                    Uint16 p = srow[col];
                    int dx0 = cx + col * scale;
                    for (int dx = 0; dx < scale; dx++) {
                        drow[dx0 + dx] = p;
                    }
                }
            }
        }
        ui_rect(cx - 1, cy - 1, cw + 2, chh + 2, 1, ui_col_surface_hi);
    }

    /* ---- 虚拟手柄（含运行期 W 的自动局部数组） ---- */
    const pad_btn_t pads[] = {
        {  24,  20, 64, 64, "^", GC_PAD_UP },
        {  24, 100, 64, 64, "v", GC_PAD_DOWN },
        { 110,  60, 64, 64, "<", GC_PAD_LEFT },
        { 190,  60, 64, 64, ">", GC_PAD_RIGHT },
        {  60,  62, 60, 60, "SEL", GC_PAD_SELECT },
        { 130,  62, 60, 60, "STA", GC_PAD_START },
        { W - 260,  10, 72, 72, "B", GC_PAD_B },
        { W - 150,  60, 72, 72, "A", GC_PAD_A },
    };
    const int n_pads = (int)(sizeof(pads) / sizeof(pads[0]));
    const int pad_y = H - pad_h + 6;

    uint32_t mask = 0;
    for (int i = 0; i < n_pads; i++) {
        const pad_btn_t *b = &pads[i];
        int x = (i < 6) ? b->x : b->x;
        int y = pad_y + b->y;
        bool active = t_down && pt_in(t_lx, t_ly, x, y, b->w, b->h);
        if (active) {
            mask |= b->bit;
        }
        ui_fill_round(x, y, b->w, b->h, b->w / 2 - 2,
                      active ? ui_col_brand_press : ui_col_surface_hi);
        int fscale = strlen(b->label) > 2 ? 1 : 2;
        ui_text(x + (b->w - ui_text_w(b->label, fscale)) / 2,
                y + (b->h - 8 * fscale) / 2, b->label,
                active ? ui_col_text : ui_col_text_sub, fscale);
    }
    s_pad_state = mask;

    /* ---- 简介浮层 ---- */
    if (s_play.info_open) {
        const int iw = W - 60, ih = H - 200;
        const int ix = 30, iy = 80;
        ui_fill_round(ix, iy, iw, ih, 12, ui_col_surface);
        ui_rect(ix, iy, iw, ih, 2, ui_col_surface_hi);
        ui_text(ix + 20, iy + 16, s_play.title, ui_col_text, 3);
        if (s_play.meta[0]) {
            ui_text(ix + 20, iy + 48, s_play.meta, ui_col_mute, 1);
        }
        char lines[6][72];
        int n = 0;
        wrap_text(s_play.desc[0] ? s_play.desc : "No description.",
                  (iw - 40) / 8, lines, 6, &n);
        for (int i = 0; i < n; i++) {
            ui_text(ix + 20, iy + 76 + i * 16, lines[i], ui_col_text_sub, 1);
        }
        if (ui_button(32, ix + iw / 2 - 80, iy + ih - 56, 160, 44, "Close", true)) {
            s_play.info_open = false;
        }
    }
}

/* ==================== 设置页 ==================== */

static void text_page_show(const char *title)
{
    strlcpy(s_text_page.title, title, sizeof(s_text_page.title));
    s_text_page.n = 0;
    s_page = PAGE_TEXT;
}

static void text_page_line(const char *fmt, ...)
{
    if (s_text_page.n >= 8) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_text_page.lines[s_text_page.n], sizeof(s_text_page.lines[0]), fmt, ap);
    va_end(ap);
    s_text_page.n++;
}

static void page_settings(void)
{
    const int W = ui_g.w, H = ui_g.h;
    if (page_header(40, "Settings")) {
        s_page = PAGE_HOME;
        return;
    }

    int y = 68;
    const int x = 8, w = W - 16, rh = 52;

    if (ui_row(41, x, y, w, rh, "Wi-Fi", s_status)) {
        /* 触发 gc_ui_show_settings 注册的动作回调（main: gc_provision_start） */
        if (s_settings_action_cb) {
            s_settings_action_cb(GC_UI_SET_WIFI, s_settings_action_ctx);
        } else if (s_home.settings_cb) {
            s_home.settings_cb(s_home.settings_ctx);
        }
    }
    y += rh + 8;
    char rot[16];
    snprintf(rot, sizeof(rot), "%u deg", (unsigned)s_rotation * 90);
    if (ui_row(42, x, y, w, rh, "Rotation", rot)) {
        gc_ui_set_rotation((s_rotation + 1) % 4);
    }
    y += rh + 8;
    if (ui_row(43, x, y, w, rh, "Bluetooth", "via ESP32-C6")) {
        text_page_show("Bluetooth");
        text_page_line("8BitGo uses the onboard ESP32-C6");
        text_page_line("(NumBLE) as a BLE gamepad.");
        text_page_line("Pair from your phone / PC.");
    }
    y += rh + 8;
    if (ui_row(44, x, y, w, rh, "Emulator Cores", NULL)) {
        text_page_show("Emulator Cores");
        const gc_emu_core_info_t *cores = NULL;
        size_t n = gc_emu_list(&cores);
        for (size_t i = 0; i < n && i < 6; i++) {
            text_page_line("%-4s %-10s %s", cores[i].platform_name,
                           cores[i].name, cores[i].enabled ? "on" : "off");
        }
        if (n == 0) {
            text_page_line("no cores registered");
        }
    }
    y += rh + 8;
    if (ui_row(45, x, y, w, rh, "Storage", gc_store_using_sd() ? "SD card" : "SPIFFS")) {
        text_page_show("Storage");
        text_page_line("root: %s", gc_store_root());
        text_page_line("sd: %s", gc_store_using_sd() ? "mounted" : "absent");
    }
    y += rh + 8;
    if (ui_row(49, x, y, w, rh, "Clear cache", NULL)) {
        int n = gc_store_clear_cache();
        covers_invalidate();
        text_page_show("Clear cache");
        if (n >= 0) {
            text_page_line("removed %d files", n);
        } else {
            text_page_line("storage unavailable");
        }
    }
    y += rh + 8;
    if (ui_row(46, x, y, w, rh, "Diagnostics", NULL)) {
        text_page_show("Diagnostics");
        text_page_line("uptime: %u s", (unsigned)(esp_log_timestamp() / 1000));
        text_page_line("display: %dx%d rot %u", s_w, s_h, (unsigned)s_rotation * 90);
        text_page_line("net: %s", s_status);
    }
    y += rh + 8;
    if (ui_row(47, x, y, w, rh, "GitHub", "Digit-Dog/8bitgo")) {
        text_page_show("GitHub");
        text_page_line("github.com/Digit-Dog/8bitgo_esp32p4");
        text_page_line("Open the URL on a PC / phone.");
    }
    y += rh + 8;
    if (ui_row(48, x, y, w, rh, "About", "v1.0 SDL3")) {
        text_page_show("About");
        text_page_line("8BitGo Retro Game Station");
        text_page_line("SDL3 UI on ESP32-P4");
    }
}

static void page_text(void)
{
    const int W = ui_g.w, H = ui_g.h;
    if (page_header(50, s_text_page.title)) {
        s_page = PAGE_SETTINGS;
        return;
    }
    ui_fill(0, 56, W, H - 56, ui_col_bg);
    for (int i = 0; i < s_text_page.n; i++) {
        ui_text(20, 80 + i * 18, s_text_page.lines[i], ui_col_text_sub, 1);
    }
}

/* ==================== Wi-Fi 配网 ==================== */

static void page_wifi_scan(void)
{
    const int W = ui_g.w, H = ui_g.h;
    if (page_header(60, "Wi-Fi")) {
        s_page = PAGE_HOME;
        if (s_wifi.cancel_cb) {
            s_wifi.cancel_cb(s_wifi.cancel_ctx);
        }
        return;
    }
    const int ly = 64;
    const int lh = H - ly - 104;
    ui_fill(0, ly, W, lh, ui_col_bg);

    const int total = s_wifi.scan.count;
    const int row_h = 52;
    int content_h = total * (row_h + 6) + 6;
    int max_scroll = content_h > lh ? content_h - lh : 0;
    if (s_wifi.scroll > max_scroll) {
        s_wifi.scroll = max_scroll;
    }
    if (s_wifi.scroll < 0) {
        s_wifi.scroll = 0;
    }

    static int drag_base_scroll, drag_base_y;
    if (t_press_edge && t_down && press_id == 0 && pt_in(t_lx, t_ly, 0, ly, W, lh)) {
        press_id = UI_ID_SCROLL;
        press_x = t_lx;
        press_y = t_ly;
        press_moved = false;
        drag_base_scroll = s_wifi.scroll;
        drag_base_y = t_ly;
    }
    if (t_down && press_id == UI_ID_SCROLL) {
        if (iabs(t_lx - press_x) + iabs(t_ly - press_y) > 10) {
            press_moved = true;
        }
        int want = drag_base_scroll + (drag_base_y - t_ly);
        s_wifi.scroll = want < 0 ? 0 : (want > max_scroll ? max_scroll : want);
    }
    /* 抬起清理放在条目判定之后（同 page_home） */

    ui_set_clip(0, ly, W, lh);
    for (int i = 0; i < total; i++) {
        const gc_net_ap_info_t *ap = &s_wifi.scan.aps[i];
        int x = 8, y = ly - s_wifi.scroll + 6 + i * (row_h + 6);
        ui_fill_round(x, y, W - 16, row_h, 8, ui_col_surface);
        ui_text_ellipsis(x + 14, y + (row_h - 16) / 2, ap->ssid, W / 2, ui_col_text, 2);
        char sub[24];
        snprintf(sub, sizeof(sub), "%d dBm %s", ap->rssi,
                 ap->encrypted ? "*" : "open");
        ui_text(x + W - 180, y + (row_h - 8) / 2, sub, ui_col_mute, 1);

        if (ui_item_clicked(300 + i, x, y, W - 16, row_h)) {
            if (ap->encrypted) {
                strlcpy(s_wifi.ssid, ap->ssid, sizeof(s_wifi.ssid));
                s_wifi.pass[0] = '\0';
                s_page = PAGE_WIFI_PASSWORD;
            } else if (s_wifi.pw_cb) {
                s_wifi.pw_cb(ap->ssid, NULL, s_wifi.pw_ctx);
            }
        }
    }
    ui_set_clip(0, 0, W, H);

    if (t_release_edge && press_id == UI_ID_SCROLL) {
        press_id = 0;
    }

    if (ui_button(399, (W - 220) / 2, H - 96, 220, 44, "Rescan", false)) {
        if (s_wifi.rescan_cb) {
            s_wifi.rescan_cb(s_wifi.rescan_ctx);
        }
    }
    ui_text_centered(H - 28, total ? "tap an AP to connect" : "no APs found",
                     ui_col_mute, 1);
}

/* 软键盘：5 行；返回 true 表示布局键被点按，键值写入 *out */
static bool keyboard_key(int id_base, int W, int kb_y, char *out)
{
    static const char *rows[] = {
        "1234567890",
        "qwertyuiop",
        "asdfghjkl-",
        "zxcvbnm_.",
    };
    const int kh = 40, gap = 5;
    int kw = (W - 16 - 9 * gap) / 10;
    if (kw > 56) {
        kw = 56;
    }
    int kb_w = 10 * kw + 9 * gap;
    int kx0 = (W - kb_w) / 2;

    int id = id_base;
    for (int r = 0; r < 4; r++) {
        const char *row = rows[r];
        int n = (int)strlen(row);
        int extra = (r >= 2) ? (kw + gap) / 2 : 0; /* 后两行视觉居中 */
        for (int c = 0; c < n; c++) {
            int kx = kx0 + extra + c * (kw + gap);
            int ky = kb_y + r * (kh + gap);
            char label[2] = { row[c], '\0' };
            if (ui_button(id, kx, ky, kw, kh, label, false)) {
                *out = row[c];
                return true;
            }
            id++;
        }
    }
    /* 第五行：Space / Del / OK / Cancel（累进布局，保证恰好铺满 kb_w） */
    int ky = kb_y + 4 * (kh + gap);
    int cx = kx0;
    if (ui_button(id, cx, ky, 3 * kw + 2 * gap, kh, "Space", false)) {
        *out = ' ';
        return true;
    }
    id++;
    cx += 3 * kw + 2 * gap + gap;
    if (ui_button(id, cx, ky, 2 * kw + gap, kh, "Del", false)) {
        *out = '\b';
        return true;
    }
    id++;
    cx += 2 * kw + gap + gap;
    if (ui_button(id, cx, ky, 2 * kw + gap, kh, "OK", true)) {
        *out = '\n';
        return true;
    }
    id++;
    cx += 2 * kw + gap + gap;
    if (ui_button(id, cx, ky, kx0 + kb_w - cx, kh, "Cancel", false)) {
        *out = 1; /* 取消哨兵值 */
        return true;
    }
    return false;
}

static void page_wifi_password(void)
{
    const int W = ui_g.w, H = ui_g.h;
    if (page_header(70, "Wi-Fi Password")) {
        if (s_wifi.pw_cb) {
            s_wifi.pw_cb(s_wifi.ssid, NULL, s_wifi.pw_ctx); /* NULL = 取消 */
        }
        s_page = PAGE_WIFI_SCAN;
        return;
    }

    /* SSID + 密码框 */
    ui_fill(0, 56, W, 110, ui_col_surface);
    ui_text(16, 64, "SSID:", ui_col_mute, 1);
    ui_text(70, 62, s_wifi.ssid, ui_col_text, 2);
    ui_text(16, 96, "PASS:", ui_col_mute, 1);
    char masked[52];
    size_t plen = strlen(s_wifi.pass);
    for (size_t i = 0; i < plen && i < 48; i++) {
        masked[i] = '*';
    }
    masked[plen] = '\0';
    ui_text_ellipsis(70, 94, masked, W - 100, ui_col_text, 2);

    char key = 0;
    if (keyboard_key(80, W, 180, &key)) {
        if (key == '\b') {
            size_t l = strlen(s_wifi.pass);
            if (l > 0) {
                s_wifi.pass[l - 1] = '\0';
            }
        } else if (key == '\n') {
            if (plen > 0 && s_wifi.pw_cb) {
                s_wifi.pw_cb(s_wifi.ssid, s_wifi.pass, s_wifi.pw_ctx);
            } else {
                show_toast("Enter a password");
            }
        } else if (key == 1) {
            if (s_wifi.pw_cb) {
                s_wifi.pw_cb(s_wifi.ssid, NULL, s_wifi.pw_ctx);
            }
            s_page = PAGE_WIFI_SCAN;
        } else if (plen < sizeof(s_wifi.pass) - 1) {
            size_t l = strlen(s_wifi.pass);
            s_wifi.pass[l] = key;
            s_wifi.pass[l + 1] = '\0';
        }
    }
}

static void page_wifi_status(void)
{
    const int W = ui_g.w, H = ui_g.h;
    if (page_header(90, s_status_page.title)) {
        s_page = (s_status_back == PAGE_WIFI_STATUS) ? PAGE_HOME : s_status_back;
        return;
    }
    ui_fill(0, 56, W, H - 56, ui_col_bg);
    ui_text_centered(H / 3, s_status_page.text, ui_col_text, 2);
    static int spin;
    spin = (spin + 1) % 4;
    const char dots[4][4] = { "|", "/", "-", "\\" };
    ui_text_centered(H / 3 + 40, dots[spin], ui_col_brand, 4);
}

/* ==================== 通用状态页 ==================== */

static void page_status(void)
{
    const int W = ui_g.w, H = ui_g.h;
    if (page_header(95, s_status_page.title)) {
        s_page = PAGE_HOME;
        return;
    }
    ui_fill(0, 56, W, H - 56, ui_col_bg);
    ui_text_centered(H / 3, s_status_page.text, ui_col_text, 2);
    if (s_status_page.percent >= 0) {
        const int pw = 400;
        const int py = H / 2;
        const int pxx = (W - pw) / 2;
        ui_fill_round(pxx, py, pw, 16, 8, ui_col_surface_hi);
        int pct = s_status_page.percent > 100 ? 100 : s_status_page.percent;
        if (pct > 0) {
            ui_fill_round(pxx + 3, py + 3, (pw - 6) * pct / 100, 10, 5, ui_col_brand);
        }
        char pct_s[8];
        snprintf(pct_s, sizeof(pct_s), "%d%%", pct);
        ui_text_centered(py + 40, pct_s, ui_col_mute, 2);
    }
}

/* ==================== 旋转呈现 ==================== */

static void rotate_blit(void)
{
    const int lw = s_log_surf->w, lh = s_log_surf->h;
    const int pw = s_surf->w, ph = s_surf->h;
    const Uint16 *src = (const Uint16 *)s_log_surf->pixels;
    const int src_pitch = s_log_surf->pitch >> 1;
    Uint16 *dst = (Uint16 *)s_surf->pixels;
    const int dst_pitch = s_surf->pitch >> 1;

    /* 注意内存布局：逻辑面按行主序存储，像素 (列 x_l, 行 y_l) 在
       src[y_l * pitch + x_l]。90/270 时宽高互换，若把列索引当行索引用
       会得到转置图像且读越界（表现为画面错乱/缺元素）。 */

    if (s_rotation == 1) {          /* 90°: logical(x_l,y_l) -> panel(px=y_l, py=ph-1-x_l) */
        for (int py = 0; py < ph; py++) {
            const int x_l = ph - 1 - py;
            Uint16 *drow = dst + (size_t)py * dst_pitch;
            for (int px = 0; px < pw; px++) {
                drow[px] = src[(size_t)px * src_pitch + x_l];
            }
        }
    } else if (s_rotation == 2) {   /* 180°: -> panel(pw-1-x_l, ph-1-y_l) */
        for (int y_l = 0; y_l < lh; y_l++) {
            const Uint16 *srow = src + (size_t)y_l * src_pitch;
            Uint16 *drow = dst + (size_t)(ph - 1 - y_l) * dst_pitch;
            for (int x_l = 0; x_l < lw; x_l++) {
                drow[pw - 1 - x_l] = srow[x_l];
            }
        }
    } else if (s_rotation == 3) {   /* 270°: logical(x_l,y_l) -> panel(px=pw-1-y_l, py=x_l) */
        for (int py = 0; py < ph; py++) {
            const int x_l = py;
            Uint16 *drow = dst + (size_t)py * dst_pitch;
            for (int y_l = 0; y_l < lh; y_l++) {
                drow[pw - 1 - y_l] = src[(size_t)y_l * src_pitch + x_l];
            }
        }
    }
}

static void map_touch(int px, int py, int *lx, int *ly)
{
    switch (s_rotation) {
    case 1:  *lx = s_h - 1 - py; *ly = px;            break;
    case 2:  *lx = s_w - 1 - px; *ly = s_h - 1 - py;  break;
    case 3:  *lx = py;           *ly = s_w - 1 - px;  break;
    default: *lx = px;           *ly = py;            break;
    }
}

static void ensure_log_surf(void)
{
    const int lw = (s_rotation & 1) ? s_h : s_w;
    const int lh = (s_rotation & 1) ? s_w : s_h;

    if (s_log_surf && (s_log_surf->w != lw || s_log_surf->h != lh)) {
        SDL_DestroySurface(s_log_surf);
        s_log_surf = NULL;
    }
    if (!s_log_surf) {
        if (s_log_pixels) {
            heap_caps_free(s_log_pixels);
            s_log_pixels = NULL;
        }
        s_log_pixels = heap_caps_malloc((size_t)lw * lh * 2, MALLOC_CAP_SPIRAM);
        if (!s_log_pixels) {
            ESP_LOGE(TAG, "log surface alloc failed (%dx%d)", lw, lh);
            return;
        }
        s_log_surf = SDL_CreateSurfaceFrom(lw, lh, SDL_PIXELFORMAT_RGB565,
                                           s_log_pixels, lw * 2);
        ESP_LOGI(TAG, "log surface %dx%d", lw, lh);
    }
}

/* ==================== UI 线程 ==================== */

static void process_events(void)
{
    SDL_Event e;
    t_press_edge = false;
    t_release_edge = false;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            t_down = true;
            t_press_edge = true;
            map_touch((int)e.button.x, (int)e.button.y, &t_lx, &t_ly);
            s_dbg_downs++;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (t_down) {
                map_touch((int)e.motion.x, (int)e.motion.y, &t_lx, &t_ly);
                s_dbg_motions++;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            t_down = false;
            t_release_edge = true;
            map_touch((int)e.button.x, (int)e.button.y, &t_lx, &t_ly);
            s_dbg_ups++;
            break;
        default:
            break;
        }
    }
}

/**
 * 独立触摸采样任务（60Hz）：渲染帧周期 60~90ms，快 tap 的按下过程会
 * 完整落在两次帧采样之间而丢失。这里与帧率解耦，状态变化即时推入
 * SDL 事件队列（SDL_PushEvent 线程安全），坐标为面板物理像素。
 */
static void *touch_thread(void *arg)
{
    (void)arg;
    /* 必须是 pthread：SDL_PushEvent 内部走 pthread 语义，
       裸 FreeRTOS 任务调用会触发 pthread_self 断言 -> 重启 */
    bool prev = false;
    int px = -1, py = -1;
    for (;;) {
        esp_bsp_sdl_touch_info_t ti;
        if (esp_bsp_sdl_touch_read(&ti) == ESP_OK) {
            if (ti.pressed != prev) {
                SDL_Event e = {0};
                e.button.type = ti.pressed ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                           : SDL_EVENT_MOUSE_BUTTON_UP;
                e.button.x = (float)ti.x;
                e.button.y = (float)ti.y;
                e.button.clicks = 1;
                SDL_PushEvent(&e);
                prev = ti.pressed;
                px = ti.x;
                py = ti.y;
            } else if (ti.pressed && (ti.x != px || ti.y != py)) {
                SDL_Event e = {0};
                e.motion.type = SDL_EVENT_MOUSE_MOTION;
                e.motion.x = (float)ti.x;
                e.motion.y = (float)ti.y;
                SDL_PushEvent(&e);
                px = ti.x;
                py = ti.y;
            }
        }
        /* gsl3680 已知特性（见旧 LVGL 版注释）：读取过频会返回空数据。
           20ms 是旧版验证过的节奏，勿再调快。 */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return NULL;
}

static void *ui_thread(void *arg)
{
    (void)arg;

    /* 必须跑在 pthread 里：SDL 内部会调用 pthread_self()，
       IDF 的 pthread 层对非 pthread 任务直接断言。 */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        ESP_LOGE(TAG, "SDL_Init failed: %s", SDL_GetError());
        return NULL;
    }
    ESP_LOGI(TAG, "SDL %d.%d.%d video ready", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
             SDL_MICRO_VERSION);

    /* SDL 就绪后再启动独立触摸采样（60Hz，与帧率解耦；pthread 语义） */
    pthread_t tid;
    esp_pthread_cfg_t tcfg = esp_pthread_get_default_config();
    tcfg.stack_size = 6144;
    tcfg.prio = 6;
    tcfg.pin_to_core = 0;
    tcfg.inherit_cfg = false;
    if (esp_pthread_set_cfg(&tcfg) == ESP_OK &&
        pthread_create(&tid, NULL, touch_thread, NULL) == 0) {
        /* started */
    } else {
        ESP_LOGE(TAG, "create touch thread failed");
    }

    s_win = SDL_CreateWindow("8BitGo", s_w, s_h, 0);
    if (!s_win) {
        ESP_LOGE(TAG, "SDL_CreateWindow failed: %s", SDL_GetError());
        return NULL;
    }

    s_surf = SDL_GetWindowSurface(s_win);
    if (!s_surf || !s_surf->pixels) {
        ESP_LOGE(TAG, "SDL_GetWindowSurface failed: %s", SDL_GetError());
        return NULL;
    }
    ESP_LOGI(TAG, "window surface %dx%d fmt=%s", s_surf->w, s_surf->h,
             SDL_GetPixelFormatName(s_surf->format));

    ui_theme_map(s_surf);

    for (;;) {
        process_events();

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_rotation == 0) {
                ui_set_canvas(s_surf);
            } else {
                ensure_log_surf();
                if (s_log_surf) {
                    ui_set_canvas(s_log_surf);
                } else {
                    ui_set_canvas(s_surf);
                }
            }

            switch (s_page) {
            case PAGE_BOOT:          page_boot(); break;
            case PAGE_HOME:          page_home(); break;
            case PAGE_PLAY:          page_play(); break;
            case PAGE_SETTINGS:      page_settings(); break;
            case PAGE_WIFI_SCAN:     page_wifi_scan(); break;
            case PAGE_WIFI_PASSWORD: page_wifi_password(); break;
            case PAGE_WIFI_STATUS:   page_wifi_status(); break;
            case PAGE_TEXT:          page_text(); break;
            default:                 page_boot(); break;
            }
            xSemaphoreGive(s_lock);
        }

        if (s_rotation != 0 && s_log_surf) {
            rotate_blit();
        }
        SDL_UpdateWindowSurface(s_win);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return NULL;
}

/* ==================== 公共 API ==================== */

bool gc_ui_lock(uint32_t timeout_ms)
{
    return s_lock && xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void gc_ui_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

esp_err_t gc_ui_init(esp_lcd_panel_handle_t panel,
                     esp_lcd_panel_io_handle_t io,
                     esp_lcd_touch_handle_t touch,
                     int hres, int vres)
{
    (void)io;
    s_w = hres;
    s_h = vres;

    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create lock failed");

    strlcpy(s_status, "Wi-Fi offline", sizeof(s_status));
    s_rotation = gc_prefs_get_u8(GC_PREFS_KEY_ROTATION, 0) % 4;

    ESP_RETURN_ON_ERROR(gc_sdl_bsp_attach(panel, touch), TAG, "attach failed");
    gc_sdl_bsp_set_display(hres, vres, SDL_PIXELFORMAT_RGB565);
    gc_sdl_bsp_use_external_touch(true); /* 触摸由独立任务高频采样 */

    /* 钉到 CPU 1：渲染线程不与后台任务（多在 CPU 0）抢核 */
    esp_pthread_cfg_t pcfg = esp_pthread_get_default_config();
    pcfg.stack_size = 16384;
    pcfg.prio = 5;
    pcfg.pin_to_core = 1;
    pcfg.inherit_cfg = false;
    ESP_RETURN_ON_ERROR(esp_pthread_set_cfg(&pcfg), TAG, "set pthread cfg failed");
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, ui_thread, NULL);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_ERR_NO_MEM, TAG, "create ui thread failed");

    ESP_LOGI(TAG, "gc_ui_sdl ready (%dx%d) rotation=%u", hres, vres,
             (unsigned)s_rotation * 90);
    return ESP_OK;
}

/* ---- 启动页 ---- */

esp_err_t gc_ui_boot_show(void)
{
    if (!gc_ui_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    s_page = PAGE_BOOT;
    s_boot.percent = 0;
    strlcpy(s_boot.text, "Starting...", sizeof(s_boot.text));
    gc_ui_unlock();
    return ESP_OK;
}

esp_err_t gc_ui_boot_progress(int percent, const char *text)
{
    if (!gc_ui_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (percent >= 0) {
        s_boot.percent = percent > 100 ? 100 : percent;
    }
    if (text) {
        strlcpy(s_boot.text, text, sizeof(s_boot.text));
    }
    gc_ui_unlock();
    return ESP_OK;
}

/* ---- 状态栏 ---- */

esp_err_t gc_ui_update_net_status(const char *text)
{
    if (!gc_ui_lock(500)) {
        return ESP_ERR_TIMEOUT;
    }
    if (text) {
        strlcpy(s_status, text, sizeof(s_status));
    }
    gc_ui_unlock();
    return ESP_OK;
}

/* ---- 导航 ---- */

void gc_ui_nav_back(void)
{
    if (!gc_ui_lock(500)) {
        return;
    }
    if (s_page != PAGE_HOME && s_page != PAGE_PLAY && s_page != PAGE_BOOT) {
        s_page = PAGE_HOME;
    }
    gc_ui_unlock();
}

bool gc_ui_nav_can_back(void)
{
    return s_page != PAGE_HOME;
}

void gc_ui_nav_home(void)
{
    if (gc_ui_lock(500)) {
        s_page = PAGE_HOME;
        gc_ui_unlock();
    }
}

/* ---- 状态 / 提示页 ---- */

esp_err_t gc_ui_show_message(const char *title, const char *text)
{
    if (!gc_ui_lock(500)) {
        return ESP_ERR_TIMEOUT;
    }
    strlcpy(s_status_page.title, title ? title : "Message",
            sizeof(s_status_page.title));
    strlcpy(s_status_page.text, text ? text : "", sizeof(s_status_page.text));
    s_status_page.percent = -1;
    if (s_page != PAGE_PLAY && s_page != PAGE_WIFI_STATUS) {
        s_status_back = s_page;
        s_page = PAGE_WIFI_STATUS;
    }
    gc_ui_unlock();
    return ESP_OK;
}

esp_err_t gc_ui_show_progress(int percent, const char *text)
{
    if (!gc_ui_lock(500)) {
        return ESP_ERR_TIMEOUT;
    }
    strlcpy(s_status_page.title, "Please wait", sizeof(s_status_page.title));
    strlcpy(s_status_page.text, text ? text : "", sizeof(s_status_page.text));
    s_status_page.percent = percent;
    /* ROM 下载等流程需要可见反馈：从主页进入时切到进度页；
       游玩页绝不劫持（游戏运行中）。 */
    if (s_page != PAGE_PLAY && s_page != PAGE_WIFI_STATUS) {
        if (s_page != PAGE_WIFI_STATUS) {
            s_status_back = s_page;
        }
        s_page = PAGE_WIFI_STATUS;
    }
    gc_ui_unlock();
    return ESP_OK;
}

/* ---- 主页 ---- */

esp_err_t gc_ui_home_bind(const gc_game_list_t *games,
                          const gc_sd_rom_t *sd_roms, size_t sd_count,
                          gc_ui_index_cb on_play_game, gc_ui_index_cb on_play_rom,
                          void *ctx)
{
    if (!gc_ui_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    s_home.games = games;
    s_home.roms = sd_roms;
    s_home.rom_n = sd_count;
    s_home.play_game_cb = on_play_game;
    s_home.play_rom_cb = on_play_rom;
    s_home.bind_ctx = ctx;
    gc_ui_unlock();
    return ESP_OK;
}

esp_err_t gc_ui_home_show(gc_ui_source_t src, bool animate)
{
    (void)animate;
    if (!gc_ui_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    s_home.src = src;
    s_home.scroll = 0;
    s_page = PAGE_HOME;
    gc_ui_unlock();
    return ESP_OK;
}

esp_err_t gc_ui_home_refresh(void)
{
    return gc_ui_home_show(s_home.src, false);
}

esp_err_t gc_ui_home_rerender(void) { return ESP_OK; }

esp_err_t gc_ui_home_reload(void)
{
    if (gc_ui_lock(500)) {
        s_home.gen++;
        s_home.scroll = 0;
        covers_invalidate();
        gc_ui_unlock();
    }
    return ESP_OK;
}

void gc_ui_set_source_cb(gc_ui_source_cb cb, void *ctx)
{
    s_home.source_cb = cb;
    s_home.source_ctx = ctx;
}

void gc_ui_set_loadmore_cb(gc_ui_loadmore_cb cb, void *ctx)
{
    s_home.load_cb = cb;
    s_home.load_ctx = ctx;
}

void gc_ui_set_home_settings_cb(gc_ui_home_settings_cb cb, void *ctx)
{
    s_home.settings_cb = cb;
    s_home.settings_ctx = ctx;
}

uint32_t gc_ui_home_generation(void) { return s_home.gen; }

esp_err_t gc_ui_home_cover_ready(uint32_t gen, size_t index, const char *path)
{
    /* 由 gc_library 的下载 worker 调用：解码 + 预缩放到封面槽位 */
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    int x = 0, y = 0, n = 0;
    unsigned char *img = stbi_load(path, &x, &y, &n, 3);
    if (!img || x <= 0 || y <= 0) {
        return ESP_FAIL; /* 解码失败保留占位图 */
    }
    float sc = 1.0f;
    if ((float)COVER_MAX_W / x < sc) {
        sc = (float)COVER_MAX_W / x;
    }
    if ((float)COVER_MAX_H / y < sc) {
        sc = (float)COVER_MAX_H / y;
    }
    int tw = (int)(x * sc), th = (int)(y * sc);
    if (tw < 1) {
        tw = 1;
    }
    if (th < 1) {
        th = 1;
    }
    uint16_t *pix = heap_caps_malloc((size_t)tw * th * 2, MALLOC_CAP_SPIRAM);
    if (!pix) {
        stbi_image_free(img);
        return ESP_ERR_NO_MEM;
    }
    for (int ty = 0; ty < th; ty++) {
        const int syy = (int)((float)ty * y / th);
        const unsigned char *srow = img + (size_t)syy * x * 3;
        for (int tx = 0; tx < tw; tx++) {
            const int sxx = (int)((float)tx * x / tw);
            const unsigned char *p = srow + (size_t)sxx * 3;
            pix[(size_t)ty * tw + tx] =
                (Uint16)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
        }
    }
    stbi_image_free(img);

    if (!gc_ui_lock(1000)) {
        heap_caps_free(pix);
        return ESP_ERR_TIMEOUT;
    }
    cover_slot_t *cs = &s_covers[index % COVER_SLOTS];
    if (cs->pix) {
        heap_caps_free(cs->pix);
    }
    cs->pix = pix;
    cs->w = tw;
    cs->h = th;
    cs->index = (int)index;
    cs->gen = gen;
    cs->valid = true;
    gc_ui_unlock();
    return ESP_OK;
}

esp_err_t gc_ui_home_set_loading(bool loading)
{
    s_home.loading = loading;
    return ESP_OK;
}

/* ---- 游玩页 ---- */

esp_err_t gc_ui_play_enter(int w, int h, const uint16_t *fb,
                           const char *title, const char *platform,
                           const char *meta, const char *desc)
{
    if (!fb || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gc_ui_lock(2000)) {
        return ESP_ERR_TIMEOUT;
    }
    s_play.w = w;
    s_play.h = h;
    s_play.fb = fb;
    strlcpy(s_play.title, title ? title : "Game", sizeof(s_play.title));
    snprintf(s_play.meta, sizeof(s_play.meta), "%s%s%s",
             platform ? platform : "",
             meta && meta[0] ? " - " : "",
             meta ? meta : "");
    strlcpy(s_play.desc, desc ? desc : "", sizeof(s_play.desc));
    s_play.info_open = false;
    s_pad_state = 0;
    s_exit_req = false;
    s_page = PAGE_PLAY;
    gc_ui_unlock();
    ESP_LOGI(TAG, "play page: %s (%s) %dx%d", title ? title : "?",
             platform ? platform : "?", w, h);
    return ESP_OK;
}

esp_err_t gc_ui_play_blit(void) { return ESP_OK; } /* 即时模式每帧重画 */

esp_err_t gc_ui_play_exit(void)
{
    if (gc_ui_lock(1000)) {
        s_play.fb = NULL;
        s_pad_state = 0;
        s_exit_req = false;
        s_page = PAGE_HOME;
        gc_ui_unlock();
    }
    return ESP_OK;
}

uint32_t gc_ui_get_pad(void) { return s_pad_state; }
bool gc_ui_exit_requested(void) { return s_exit_req; }
void gc_ui_clear_exit_request(void) { s_exit_req = false; }

/* ---- Wi-Fi 配网 ---- */

void gc_ui_set_wifi_rescan_cb(gc_ui_rescan_cb cb, void *ctx)
{
    s_wifi.rescan_cb = cb;
    s_wifi.rescan_ctx = ctx;
}

void gc_ui_set_wifi_manual_cb(gc_ui_rescan_cb cb, void *ctx)
{
    s_wifi.manual_cb = cb;
    s_wifi.manual_ctx = ctx;
}

void gc_ui_set_wifi_cancel_cb(gc_ui_cancel_cb cb, void *ctx)
{
    s_wifi.cancel_cb = cb;
    s_wifi.cancel_ctx = ctx;
}

esp_err_t gc_ui_show_wifi_scan(const gc_net_scan_result_t *result,
                               gc_ui_ap_selected_cb cb, void *ctx)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gc_ui_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    s_wifi.scan = *result;
    s_wifi.scroll = 0;
    s_wifi.ap_cb = cb;
    s_wifi.ap_ctx = ctx;
    s_page = PAGE_WIFI_SCAN;
    gc_ui_unlock();
    return ESP_OK;
}

esp_err_t gc_ui_show_wifi_scanning(void)
{
    if (!gc_ui_lock(500)) {
        return ESP_ERR_TIMEOUT;
    }
    strlcpy(s_status_page.title, "Wi-Fi", sizeof(s_status_page.title));
    strlcpy(s_status_page.text, "Scanning Wi-Fi...", sizeof(s_status_page.text));
    s_status_page.percent = -1;
    s_page = PAGE_WIFI_STATUS;
    gc_ui_unlock();
    return ESP_OK;
}

esp_err_t gc_ui_show_wifi_password(const char *ssid, bool ssid_editable,
                                   gc_ui_password_cb cb, void *ctx)
{
    if (!gc_ui_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    strlcpy(s_wifi.ssid, ssid ? ssid : "", sizeof(s_wifi.ssid));
    s_wifi.pass[0] = '\0';
    s_wifi.ssid_editable = ssid_editable; /* M4：SSID 固定显示，暂不支持改 */
    s_wifi.pw_cb = cb;
    s_wifi.pw_ctx = ctx;
    s_page = PAGE_WIFI_PASSWORD;
    gc_ui_unlock();
    return ESP_OK;
}

esp_err_t gc_ui_show_wifi_connecting(const char *ssid)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Connecting %s...", ssid ? ssid : "");
    return gc_ui_show_message("Wi-Fi", buf);
}

esp_err_t gc_ui_show_wifi_result(bool ok, const char *msg)
{
    char buf[72];
    snprintf(buf, sizeof(buf), "%s%s", ok ? "Connected. " : "Failed: ",
             msg ? msg : "");
    esp_err_t err = gc_ui_show_message("Wi-Fi", buf);
    return err;
}

/* ---- 设置 / 子页 ---- */

esp_err_t gc_ui_show_settings(gc_ui_settings_cb cb, void *ctx)
{
    /* 记录动作回调（Wi-Fi 行点击时触发 GC_UI_SET_WIFI）并打开设置页 */
    s_settings_action_cb = cb;
    s_settings_action_ctx = ctx;
    if (gc_ui_lock(500)) {
        s_page = PAGE_SETTINGS;
        gc_ui_unlock();
    }
    return ESP_OK;
}

esp_err_t gc_ui_show_cores(void) { text_page_show("Emulator Cores"); return ESP_OK; }
esp_err_t gc_ui_show_bluetooth(void) { text_page_show("Bluetooth"); return ESP_OK; }
esp_err_t gc_ui_show_github(void) { text_page_show("GitHub"); return ESP_OK; }
esp_err_t gc_ui_show_about(void) { text_page_show("About"); return ESP_OK; }

/* ---- 旋转 ---- */

esp_err_t gc_ui_set_rotation(uint8_t rot)
{
    rot %= 4;
    if (!gc_ui_lock(500)) {
        return ESP_ERR_TIMEOUT;
    }
    s_rotation = rot;
    press_id = 0;
    gc_prefs_set_u8(GC_PREFS_KEY_ROTATION, rot);
    gc_ui_unlock();
    ESP_LOGI(TAG, "rotation set to %u", (unsigned)rot * 90);
    return ESP_OK;
}

uint8_t gc_ui_get_rotation(void) { return s_rotation; }

/* ---- SD / 诊断（设置页内直接展示，保留 API 兼容） ---- */

esp_err_t gc_ui_show_storage(void) { text_page_show("Storage"); return ESP_OK; }
esp_err_t gc_ui_show_diagnostics(void) { text_page_show("Diagnostics"); return ESP_OK; }
