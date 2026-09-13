/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file ui_gfx.h
 * @brief SDL UI 绘制原语：逻辑坐标画布 / 裁剪 / 8x8 字体文本
 *
 * 画布面向「逻辑方向」：rot=0 时直接绑窗口表面；rot!=0 时绑 PSRAM 逻辑面，
 * 呈现时整体旋转搬运到窗口表面（见 gc_ui_sdl.c 的 rotate_blit）。
 */
#pragma once

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    SDL_Surface *s;   /* 目标表面（RGB565） */
    int w, h;         /* 逻辑宽高 */
    /* 裁剪区（画布坐标） */
    int clip_x, clip_y, clip_w, clip_h;
} ui_canvas_t;

extern ui_canvas_t ui_g;

static inline void ui_set_canvas(SDL_Surface *s)
{
    ui_g.s = s;
    ui_g.w = s->w;
    ui_g.h = s->h;
    ui_g.clip_x = 0;
    ui_g.clip_y = 0;
    ui_g.clip_w = ui_g.w;
    ui_g.clip_h = ui_g.h;
}

static inline void ui_set_clip(int x, int y, int w, int h)
{
    ui_g.clip_x = x;
    ui_g.clip_y = y;
    ui_g.clip_w = w;
    ui_g.clip_h = h;
}

void ui_fill(int x, int y, int w, int h, Uint32 c);
void ui_rect(int x, int y, int w, int h, int t, Uint32 c);
/* 近似圆角（四角内收）填充 */
void ui_fill_round(int x, int y, int w, int h, int r, Uint32 c);
/* RGB565 位块拷贝（带裁剪），用于封面图 */
void ui_blit(int x, int y, int w, int h, const uint16_t *pix);
int  ui_text(int x, int y, const char *s, Uint32 c, int scale);
int  ui_text_w(const char *s, int scale);
/* 带省略号截断，返回实际结束 x */
int  ui_text_ellipsis(int x, int y, const char *s, int max_w, Uint32 c, int scale);
void ui_text_centered(int cy, const char *s, Uint32 c, int scale);

/* 主题色（表面创建后调用 ui_theme_map 映射） */
extern Uint32 ui_col_bg, ui_col_surface, ui_col_surface_hi, ui_col_header;
extern Uint32 ui_col_text, ui_col_text_sub, ui_col_mute, ui_col_brand, ui_col_brand_press;
void ui_theme_map(SDL_Surface *s);
