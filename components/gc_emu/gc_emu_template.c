/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_emu_template.c
 * @brief 「嵌入你自己的模拟器」极简模板
 *
 * 这是一份可编译运行的骨架，演示如何把一个真实模拟器核心接入 gc_emu 框架。
 * 默认用占位桩（滚动彩条）证明链路通畅；把桩换成你的核心即可真实游玩。
 *
 * 接入步骤：
 *   1. 把本文件的 my_core_* 换成你真实核心的接口（或直接 #include 你的核心头）。
 *   2. 在 open / load_rom / reset / set_pad / render_frame / audio_fill 里调用你的核心。
 *   3. 在 app_main 初始化阶段调用一次 gc_emu_template_register() 完成注册，
 *      上层（gc_emu_get_ops / main.c 主循环）完全不用改。
 *
 * 注意：本模板演示用 GC_PLATFORM_GB（160x144）作为「新机种」示例，
 *       真实接入时把 .platform 改成你的机种（GC_PLATFORM_GB/GBC/SNES/MD/...）。
 */

#include "gc_emu.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

static const char *TAG = "gc_emu_tmpl";

/* =========================================================================
 * 1) 你的模拟器核心接口
 *    —— 替换成真实核心（如 nofrendo / infoNES / 自研）。
 *    下面 my_core_* 仅是占位桩，保证本文件能编译、能跑通「下载→加载→渲染」链路。
 * ========================================================================= */
typedef struct my_core_t my_core_t;

static my_core_t *my_core_create(void)                                { return (my_core_t *)1; } /* TODO: 创建核心上下文 */
static void       my_core_destroy(my_core_t *c)                      { (void)c; }                /* TODO: 销毁 */
static void       my_core_load_rom(my_core_t *c, const uint8_t *d, size_t n) {
    (void)c; (void)d; (void)n;                                                            /* TODO: 把 ROM 交给核心 */
}
static void       my_core_reset(my_core_t *c)                        { (void)c; }                /* TODO: 复位核心 */
static void       my_core_set_pad(my_core_t *c, uint32_t s)          { (void)c; (void)s; }      /* TODO: 同步手柄状态 */
/* 跑一帧，把画面写入 fb（RGB565，长度 = w*h） */
static void       my_core_run_frame(my_core_t *c, uint16_t *fb, int w, int h) {
    (void)c;
    static const uint16_t bars[] = {0xF800, 0x07E0, 0x001F, 0xFFE0}; /* R G B Y */
    static int off = 0;
    off = (off + 1) % w;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            fb[y * w + x] = bars[((x + off) * 4 / w) % 4];
        }
    }
}
/* 取一段音频采样（int16）填入 buf；无声就 memset 0 */
static void       my_core_fill_audio(my_core_t *c, int16_t *buf, size_t samples) {
    (void)c;
    if (buf && samples) {
        memset(buf, 0, samples * sizeof(int16_t));
    }
}

/* =========================================================================
 * 2) 机种私有上下文（gc_emu_t 不透明句柄背后的真实结构）
 * ========================================================================= */
typedef struct {
    my_core_t *core;      /*!< 你的核心上下文 */
    uint8_t   *rom;       /*!< 整个 ROM 镜像（PSRAM），生命周期跟随实例 */
    size_t     rom_size;
    int        screen_w;
    int        screen_h;
    uint32_t   pad_state;
} template_t;

/* =========================================================================
 * 3) gc_emu_ops_t 回调实现
 * ========================================================================= */

static esp_err_t tmpl_open(gc_emu_t **emu, const gc_emu_info_t *info)
{
    ESP_RETURN_ON_FALSE(emu, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    template_t *t = heap_caps_calloc(1, sizeof(template_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(t, ESP_ERR_NO_MEM, TAG, "no mem");

    t->screen_w = (info && info->screen_w) ? info->screen_w : 160;
    t->screen_h = (info && info->screen_h) ? info->screen_h : 144;
    t->core = my_core_create();                 /* TODO: 真实核心初始化（分配上下文/显存） */

    *emu = (gc_emu_t *)t;
    return ESP_OK;
}

static esp_err_t tmpl_load_rom(gc_emu_t *emu, const char *rom_path)
{
    ESP_RETURN_ON_FALSE(emu && rom_path, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    template_t *t = (template_t *)emu;

    /* ---- 把 ROM 文件整块读进 PSRAM（大 ROM 建议按需映射，这里演示最简单做法） ---- */
    FILE *f = fopen(rom_path, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "open %s failed", rom_path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return ESP_FAIL;
    }
    uint8_t *buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
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

    t->rom = buf;
    t->rom_size = (size_t)size;

    /* ---- 把 ROM 字节喂给你的核心（真实核心可能需要 PRG/CHR 拆分，见 TODO） ---- */
    my_core_load_rom(t->core, t->rom, t->rom_size);

    ESP_LOGI(TAG, "loaded rom %s (%zu B) -> core", rom_path, t->rom_size);
    return ESP_OK;
}

static esp_err_t tmpl_reset(gc_emu_t *emu)
{
    template_t *t = (template_t *)emu;
    if (!t) {
        return ESP_ERR_INVALID_ARG;
    }
    my_core_reset(t->core);                     /* TODO: 真实核心 reset */
    return ESP_OK;
}

static void tmpl_set_pad(gc_emu_t *emu, uint32_t state)
{
    template_t *t = (template_t *)emu;
    if (!t) {
        return;
    }
    t->pad_state = state;
    my_core_set_pad(t->core, state);            /* TODO: 同步给真实核心 */
}

static esp_err_t tmpl_render_frame(gc_emu_t *emu, uint16_t *fb, size_t fb_px)
{
    ESP_RETURN_ON_FALSE(emu && fb, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    template_t *t = (template_t *)emu;
    ESP_RETURN_ON_FALSE(fb_px >= (size_t)(t->screen_w * t->screen_h),
                        ESP_ERR_INVALID_SIZE, TAG, "fb too small");

    /* ---- 调你的核心跑一帧，核心负责把画面写进 RGB565 缓冲 ---- */
    my_core_run_frame(t->core, fb, t->screen_w, t->screen_h);
    return ESP_OK;
}

static esp_err_t tmpl_audio_fill(gc_emu_t *emu, int16_t *buf, size_t samples)
{
    template_t *t = (template_t *)emu;
    if (!t) {
        return ESP_ERR_INVALID_ARG;
    }
    my_core_fill_audio(t->core, buf, samples);
    return ESP_OK;
}

static void tmpl_close(gc_emu_t *emu)
{
    template_t *t = (template_t *)emu;
    if (!t) {
        return;
    }
    my_core_destroy(t->core);                   /* TODO: 真实核心 deinit */
    free(t->rom);
    free(t);
}

/* =========================================================================
 * 4) ops 表 + 注册入口
 * ========================================================================= */

const gc_emu_ops_t gc_emu_ops_template = {
    .platform     = GC_PLATFORM_GB,   /* TODO: 改成你的机种（GB/GBC/SNES/MD/...） */
    .open         = tmpl_open,
    .load_rom     = tmpl_load_rom,
    .reset        = tmpl_reset,
    .set_pad      = tmpl_set_pad,
    .render_frame = tmpl_render_frame,
    .audio_fill   = tmpl_audio_fill,
    .close        = tmpl_close,
};

/**
 * @brief 注册本模板机种。在 app_main 初始化阶段调用一次即可。
 */
esp_err_t gc_emu_template_register(void)
{
    return gc_emu_register_ops(&gc_emu_ops_template);
}
