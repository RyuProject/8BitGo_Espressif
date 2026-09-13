/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gc_font.h"

const lv_font_t *gc_font_default(void)
{
#ifdef GC_FONT_STUB
    /* 大幅面字库暂未纳入构建，回退到 LVGL 内置默认字体兜底。
       注意：LVGL 9 的 API 是 lv_font_get_default()（v8 才叫 lv_font_default）。
       内置字体只有 ASCII/拉丁，界面里的中文会显示为空白，仅用于字体瘦身期间。 */
    return lv_font_get_default();
#else
    return &font_puhui_16_4;
#endif
}
