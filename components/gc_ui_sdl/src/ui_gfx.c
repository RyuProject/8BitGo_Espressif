/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file ui_gfx.c
 * @brief 绘制原语实现（见 ui_gfx.h）
 */
#include "ui_gfx.h"
#include "font8x8.h"
#include <string.h>

ui_canvas_t ui_g;

Uint32 ui_col_bg, ui_col_surface, ui_col_surface_hi, ui_col_header;
Uint32 ui_col_text, ui_col_text_sub, ui_col_mute, ui_col_brand, ui_col_brand_press;

void ui_theme_map(SDL_Surface *s)
{
    ui_col_bg          = SDL_MapSurfaceRGB(s, 14, 16, 22);
    ui_col_surface     = SDL_MapSurfaceRGB(s, 28, 32, 42);
    ui_col_surface_hi  = SDL_MapSurfaceRGB(s, 44, 50, 64);
    ui_col_header      = SDL_MapSurfaceRGB(s, 20, 23, 30);
    ui_col_text        = SDL_MapSurfaceRGB(s, 235, 238, 242);
    ui_col_text_sub    = SDL_MapSurfaceRGB(s, 180, 186, 196);
    ui_col_mute        = SDL_MapSurfaceRGB(s, 122, 130, 142);
    ui_col_brand       = SDL_MapSurfaceRGB(s, 0x2D, 0x7D, 0xFF);
    ui_col_brand_press = SDL_MapSurfaceRGB(s, 0x1B, 0x5C, 0xCC);
}

static inline void px(int x, int y, Uint32 c)
{
    const ui_canvas_t *g = &ui_g;
    if (x < g->clip_x || x >= g->clip_x + g->clip_w ||
        y < g->clip_y || y >= g->clip_y + g->clip_h) {
        return;
    }
    ((Uint16 *)g->s->pixels)[y * (g->s->pitch >> 1) + x] = (Uint16)c;
}

void ui_fill(int x, int y, int w, int h, Uint32 c)
{
    const ui_canvas_t *g = &ui_g;
    /* 与裁剪区求交 */
    int x1 = x + w, y1 = y + h;
    if (x < g->clip_x) {
        x = g->clip_x;
    }
    if (y < g->clip_y) {
        y = g->clip_y;
    }
    if (x1 > g->clip_x + g->clip_w) {
        x1 = g->clip_x + g->clip_w;
    }
    if (y1 > g->clip_y + g->clip_h) {
        y1 = g->clip_y + g->clip_h;
    }
    if (x1 <= x || y1 <= y) {
        return;
    }
    const int pitch16 = g->s->pitch >> 1;
    Uint16 *base = (Uint16 *)g->s->pixels;
    for (int yy = y; yy < y1; yy++) {
        Uint16 *row = base + yy * pitch16;
        for (int xx = x; xx < x1; xx++) {
            row[xx] = (Uint16)c;
        }
    }
}

void ui_rect(int x, int y, int w, int h, int t, Uint32 c)
{
    ui_fill(x, y, w, t, c);
    ui_fill(x, y + h - t, w, t, c);
    ui_fill(x, y, t, h, c);
    ui_fill(x + w - t, y, t, h, c);
}

void ui_fill_round(int x, int y, int w, int h, int r, Uint32 c)
{
    if (r <= 0) {
        ui_fill(x, y, w, h, c);
        return;
    }
    if (r * 2 > w) {
        r = w / 2;
    }
    if (r * 2 > h) {
        r = h / 2;
    }
    ui_fill(x + r, y, w - 2 * r, h, c);
    ui_fill(x, y + r, r, h - 2 * r, c);
    ui_fill(x + w - r, y + r, r, h - 2 * r, c);
    /* 四个角：逐步内收 */
    for (int i = 0; i < r; i++) {
        int inset = r - i;
        ui_fill(x + inset, y + i, 1, 1, c);                       /* 左上 */
        ui_fill(x + w - 1 - inset, y + i, 1, 1, c);               /* 右上 */
        ui_fill(x + inset, y + h - 1 - i, 1, 1, c);               /* 左下 */
        ui_fill(x + w - 1 - inset, y + h - 1 - i, 1, 1, c);       /* 右下 */
    }
}

void ui_blit(int x, int y, int w, int h, const uint16_t *pix)
{
    const ui_canvas_t *g = &ui_g;
    const int pitch16 = g->s->pitch >> 1;
    Uint16 *base = (Uint16 *)g->s->pixels;
    for (int row = 0; row < h; row++) {
        const int dy = y + row;
        if (dy < g->clip_y || dy >= g->clip_y + g->clip_h) {
            continue;
        }
        Uint16 *drow = base + dy * pitch16;
        const Uint16 *srow = pix + (size_t)row * w;
        for (int col = 0; col < w; col++) {
            const int dx = x + col;
            if (dx < g->clip_x || dx >= g->clip_x + g->clip_w) {
                continue;
            }
            drow[dx] = srow[col];
        }
    }
}

static int draw_char(int x, int y, char ch, Uint32 c, int scale)
{
    unsigned char u = (unsigned char)ch;
    if (u < 0x20 || u > 0x7E) {
        u = '?'; /* M2 仍为英文字库，非 ASCII 占位 */
    }
    const unsigned char *g = font8x8_basic[u];
    for (int row = 0; row < 8; row++) {
        const unsigned char bits = g[row];
        if (!bits) {
            continue;
        }
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1u << col))) {
                continue;
            }
            const int px0 = x + col * scale;
            const int py0 = y + row * scale;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    px(px0 + dx, py0 + dy, c);
                }
            }
        }
    }
    return 8 * scale;
}

int ui_text_w(const char *s, int scale)
{
    return (int)strlen(s) * 8 * scale;
}

int ui_text(int x, int y, const char *s, Uint32 c, int scale)
{
    for (const char *p = s; *p; p++) {
        x += draw_char(x, y, *p, c, scale);
    }
    return x;
}

int ui_text_ellipsis(int x, int y, const char *s, int max_w, Uint32 c, int scale)
{
    const int cw = 8 * scale;
    if (ui_text_w(s, scale) <= max_w) {
        return ui_text(x, y, s, c, scale);
    }
    int n = 0;
    while (s[n] && (n + 1) * cw + 3 * cw <= max_w) {
        n++;
    }
    for (int i = 0; i < n; i++) {
        x += draw_char(x, y, s[i], c, scale);
    }
    return ui_text(x, y, "...", c, scale);
}

void ui_text_centered(int cy, const char *s, Uint32 c, int scale)
{
    ui_text((ui_g.w - ui_text_w(s, scale)) / 2, cy - 4 * scale, s, c, scale);
}
