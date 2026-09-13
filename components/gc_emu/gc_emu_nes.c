/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_emu_nes.c
 * @brief NES / FC 机种实现骨架（iNES 头解析已就绪，CPU/PPU 核心待接入）
 *
 * 接入真实核心（推荐 nofrendo）时改动点：
 *   1. nes_t 中增加核心自己的上下文字段；
 *   2. open()      里调用核心的初始化（如 nofrendo 的 nes_init / osd 安装）；
 *   3. load_rom()  里把 rom 数据交给核心（nofrendo: nes_load() 或 rom_load()）；
 *   4. render_frame() 里改为调用核心跑一帧，再把 ppu 输出转成 RGB565；
 *   5. audio_fill() 里取 APU 采样。
 *
 * 目前 render_frame 输出「滚动彩条 + 帧计数」，用于在没有核心的情况下
 * 验证 下载 → 加载 → 渲染 → 显示 这条链路是否通畅。
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "gc_emu.h"

static const char *TAG = "gc_emu_nes";

#define NES_W 256
#define NES_H 240

typedef struct __attribute__((packed)) {
    uint8_t magic[4];    /* 'N','E','S',0x1A */
    uint8_t prg_banks;   /* 16KB 单位 */
    uint8_t chr_banks;   /* 8KB 单位 */
    uint8_t flags6;
    uint8_t flags7;
    uint8_t flags8;
    uint8_t flags9;
    uint8_t flags10;
    uint8_t padding[5];
} ines_header_t;

typedef struct {
    int      screen_w;
    int      screen_h;
    uint8_t *rom;        /* 整个 ROM 镜像 */
    size_t   rom_size;
    uint8_t  mapper;
    uint8_t  prg_banks;
    uint8_t  chr_banks;
    bool     has_battery;
    bool     mirror_vertical;
    uint32_t pad_state;
    uint32_t frame;
    /* TODO(8bitgo): 核心上下文放这里 */
} nes_t;

/* ---------------- ops 实现 ---------------- */

static esp_err_t nes_open(gc_emu_t **emu, const gc_emu_info_t *info)
{
    ESP_RETURN_ON_FALSE(emu, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    nes_t *nes = heap_caps_calloc(1, sizeof(nes_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(nes, ESP_ERR_NO_MEM, TAG, "no mem");

    nes->screen_w = (info && info->screen_w) ? info->screen_w : NES_W;
    nes->screen_h = (info && info->screen_h) ? info->screen_h : NES_H;

    /* TODO(8bitgo): nofrendo 初始化 */
    *emu = (gc_emu_t *)nes;
    return ESP_OK;
}

static esp_err_t nes_load_rom(gc_emu_t *emu, const char *rom_path)
{
    ESP_RETURN_ON_FALSE(emu && rom_path, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    nes_t *nes = (nes_t *)emu;

    FILE *f = fopen(rom_path, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "open %s failed", rom_path);

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < (long)sizeof(ines_header_t)) {
        fclose(f);
        ESP_LOGE(TAG, "%s too small (%ld)", rom_path, size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(buf, 1, size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        return ESP_FAIL;
    }
    fclose(f);

    ines_header_t *hdr = (ines_header_t *)buf;
    if (memcmp(hdr->magic, "NES\x1a", 4) != 0) {
        ESP_LOGE(TAG, "not an iNES file: %02x %02x %02x %02x",
                 hdr->magic[0], hdr->magic[1], hdr->magic[2], hdr->magic[3]);
        free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    nes->rom = buf;
    nes->rom_size = size;
    nes->prg_banks = hdr->prg_banks;
    nes->chr_banks = hdr->chr_banks;
    nes->mapper = ((hdr->flags7 & 0xF0) << 0) | ((hdr->flags6 & 0xF0) >> 4);
    nes->has_battery = (hdr->flags6 & 0x02) != 0;
    nes->mirror_vertical = (hdr->flags6 & 0x01) != 0;
    nes->frame = 0;

    ESP_LOGI(TAG, "ROM %s: %zu B, PRG=%u CHR=%u mapper=%u bat=%d mirror=%s",
             rom_path, nes->rom_size, nes->prg_banks, nes->chr_banks, nes->mapper,
             nes->has_battery, nes->mirror_vertical ? "V" : "H");

    /* TODO(8bitgo): nofrendo 加载 ROM */
    return ESP_OK;
}

static esp_err_t nes_reset(gc_emu_t *emu)
{
    nes_t *nes = (nes_t *)emu;
    if (!nes) {
        return ESP_ERR_INVALID_ARG;
    }
    nes->frame = 0;
    /* TODO(8bitgo): 核心 reset */
    return ESP_OK;
}

static void nes_set_pad(gc_emu_t *emu, uint32_t state)
{
    nes_t *nes = (nes_t *)emu;
    if (nes) {
        nes->pad_state = state;
        /* TODO(8bitgo): 把状态同步给核心 */
    }
}

static esp_err_t nes_render_frame(gc_emu_t *emu, uint16_t *fb, size_t fb_px)
{
    ESP_RETURN_ON_FALSE(emu && fb, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    nes_t *nes = (nes_t *)emu;
    ESP_RETURN_ON_FALSE(fb_px >= (size_t)(nes->screen_w * nes->screen_h),
                        ESP_ERR_INVALID_SIZE, TAG, "fb too small");

    /* ---- 骨架占位：滚动彩条（接入核心后删除） ---- */
    static const uint16_t bars[] = {
        0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0xFFFF, 0x0000,
    };
    const int nbars = sizeof(bars) / sizeof(bars[0]);
    int offset = nes->frame % (nes->screen_w);

    for (int y = 0; y < nes->screen_h; y++) {
        uint16_t *row = fb + (size_t)y * nes->screen_w;
        for (int x = 0; x < nes->screen_w; x++) {
            row[x] = bars[((x + offset) * nbars / nes->screen_w) % nbars];
        }
    }

    /* 左上角用按键状态画一个 8 像素的指示条，验证输入链路 */
    int n = 0;
    for (uint32_t m = 1; m < (1u << 8); m <<= 1) {
        if (nes->pad_state & m) {
            for (int y = 0; y < 8; y++) {
                uint16_t *p = fb + (size_t)y * nes->screen_w + n * 10;
                for (int k = 0; k < 8; k++) {
                    p[k] = 0xFFFF;
                }
            }
        }
        n++;
    }

    nes->frame++;
    return ESP_OK;
}

static esp_err_t nes_audio_fill(gc_emu_t *emu, int16_t *buf, size_t samples)
{
    /* TODO(8bitgo): APU 输出；当前静音 */
    (void)emu;
    if (buf && samples) {
        memset(buf, 0, samples * sizeof(int16_t));
    }
    return ESP_OK;
}

static void nes_close(gc_emu_t *emu)
{
    nes_t *nes = (nes_t *)emu;
    if (!nes) {
        return;
    }
    /* TODO(8bitgo): 核心 deinit */
    free(nes->rom);
    free(nes);
}

const gc_emu_ops_t gc_emu_ops_nes = {
    .platform     = GC_PLATFORM_NES,
    .open         = nes_open,
    .load_rom     = nes_load_rom,
    .reset        = nes_reset,
    .set_pad      = nes_set_pad,
    .render_frame = nes_render_frame,
    .audio_fill   = nes_audio_fill,
    .close        = nes_close,
};
