/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_font.h
 * @brief 字体组件：Alibaba PuHuiTi 16px / 4bpp（含 ASCII + 中日韩）
 *
 * 字体源文件放在项目根的 font/ 目录（font_puhui_16_4.c），由本组件纳入构建。
 * UI 文案默认使用英文；该字体主要作为中文兜底，避免出现「豆腐块」。
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 由 font/font_puhui_16_4.c 导出 */
extern const lv_font_t font_puhui_16_4;

/** 取默认字体（当前即 PuHui 16） */
const lv_font_t *gc_font_default(void);

#ifdef __cplusplus
}
#endif
