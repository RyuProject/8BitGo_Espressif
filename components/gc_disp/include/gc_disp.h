/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_disp.h
 * @brief 屏幕尺寸 / 分辨率自适应
 *
 * 背景：UI 的像素值（间距、控件高度、字号）原本是按一块 800x1280 的屏幕手工调的，
 * 换一块分辨率不同的屏就会错位或过小/过大。
 *
 * 本模块把「设计基准」和「实际屏幕」解耦：
 *   - 以**短边 800px** 为设计基准（GC_DISP_BASE_SHORT）；
 *   - 依据实际面板短边算出统一的缩放系数（旋转不变，转屏不改变控件大小）；
 *   - 提供 gc_disp_s() 把设计基准像素换算成当前屏幕像素。
 *
 * 用法：
 *   1) 初始化：gc_disp_init(phys_w, phys_h);
 *   2) 旋转：  gc_disp_set_rotation(0..3);
 *   3) 布局：  lv_obj_set_size(obj, gc_disp_s(196), gc_disp_s(352));
 *
 * 约定：设计值与实际值都是「逻辑像素」（含旋转后的方向），旋转只改变逻辑宽高，
 * 不改变缩放系数，因此转屏前后控件尺寸一致。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 旋转角度（顺时针），与 LVGL 的 lv_display_rotation_t 取值一致 */
typedef enum {
    GC_DISP_ROT_0 = 0,
    GC_DISP_ROT_90 = 1,
    GC_DISP_ROT_180 = 2,
    GC_DISP_ROT_270 = 3,
} gc_disp_rot_t;

/** 设计基准短边（px）：UI 尺寸以这块屏幕为准调过 */
#define GC_DISP_BASE_SHORT 800

/** 缩放系数下限 / 上限（×1000），避免极端分辨率下控件不可用或过大 */
#define GC_DISP_SCALE_MIN_PERMILLE 350
#define GC_DISP_SCALE_MAX_PERMILLE 2000

/** 屏幕信息快照 */
typedef struct {
    int phys_w;           /*!< 面板原生宽（未旋转） */
    int phys_h;           /*!< 面板原生高（未旋转） */
    int log_w;            /*!< 当前逻辑宽（含旋转） */
    int log_h;            /*!< 当前逻辑高（含旋转） */
    int rot;              /*!< 0..3 */
    int short_side;       /*!< min(phys_w, phys_h) */
    int long_side;        /*!< max(phys_w, phys_h) */
    int scale_permille;   /*!< 缩放系数 ×1000（1000 = 1.0，即设计基准） */
} gc_disp_info_t;

/** 用面板原生分辨率初始化（可重复调用；未初始化时按 800x1280 / 系数 1.0 降级） */
esp_err_t gc_disp_init(int phys_w, int phys_h);

/** 设置旋转（0..3，越界自动取模）并刷新逻辑分辨率 */
esp_err_t gc_disp_set_rotation(int rot);

/** 取信息快照（永远非 NULL；未初始化时给出默认值） */
const gc_disp_info_t *gc_disp_info(void);

int  gc_disp_w(void);          /*!< 当前逻辑宽 */
int  gc_disp_h(void);          /*!< 当前逻辑高 */
int  gc_disp_rot(void);        /*!< 当前旋转 0..3 */
bool gc_disp_is_landscape(void);
int  gc_disp_scale_permille(void);
float gc_disp_scale(void);

/**
 * @brief 设计基准像素 -> 当前屏幕像素（四舍五入；0 仍为 0；结果至少 1）
 *        负值（偏移）按符号缩放。
 */
int gc_disp_s(int design_px);

/** 同 gc_disp_s()，但结果不小于 min_px（触摸目标等有最小尺寸要求时用） */
int gc_disp_s_min(int design_px, int min_px);

#ifdef __cplusplus
}
#endif
