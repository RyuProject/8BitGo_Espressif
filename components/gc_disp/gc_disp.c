/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gc_disp.h"

#include "esp_log.h"

static const char *TAG = "gc_disp";

/* 默认按设计基准降级：即使忘了 init，布局也不会崩（系数 1.0） */
static gc_disp_info_t s_info = {
    .phys_w = 800,
    .phys_h = 1280,
    .log_w = 800,
    .log_h = 1280,
    .rot = 0,
    .short_side = 800,
    .long_side = 1280,
    .scale_permille = 1000,
};

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

esp_err_t gc_disp_init(int phys_w, int phys_h)
{
    if (phys_w <= 0 || phys_h <= 0) {
        ESP_LOGE(TAG, "invalid resolution %dx%d", phys_w, phys_h);
        return ESP_ERR_INVALID_ARG;
    }

    s_info.phys_w = phys_w;
    s_info.phys_h = phys_h;
    s_info.short_side = (phys_w < phys_h) ? phys_w : phys_h;
    s_info.long_side = (phys_w > phys_h) ? phys_w : phys_h;

    /* 以短边为基准做等比缩放：旋转不改系数，转屏后控件大小一致 */
    int permille = (int)(((int64_t)s_info.short_side * 1000) / GC_DISP_BASE_SHORT);
    s_info.scale_permille = clamp_int(permille, GC_DISP_SCALE_MIN_PERMILLE,
                                      GC_DISP_SCALE_MAX_PERMILLE);

    gc_disp_set_rotation(s_info.rot);
    ESP_LOGI(TAG, "display %dx%d (short %d) -> scale %d.%03d, logical %dx%d rot=%d",
             phys_w, phys_h, s_info.short_side,
             s_info.scale_permille / 1000, s_info.scale_permille % 1000,
             s_info.log_w, s_info.log_h, s_info.rot);
    return ESP_OK;
}

esp_err_t gc_disp_set_rotation(int rot)
{
    rot = ((rot % 4) + 4) % 4;
    s_info.rot = rot;

    /* 0/180 逻辑=物理；90/270 交换宽高 */
    if (rot % 2 == 0) {
        s_info.log_w = s_info.phys_w;
        s_info.log_h = s_info.phys_h;
    } else {
        s_info.log_w = s_info.phys_h;
        s_info.log_h = s_info.phys_w;
    }
    return ESP_OK;
}

const gc_disp_info_t *gc_disp_info(void)
{
    return &s_info;
}

int gc_disp_w(void)
{
    return s_info.log_w;
}

int gc_disp_h(void)
{
    return s_info.log_h;
}

int gc_disp_rot(void)
{
    return s_info.rot;
}

bool gc_disp_is_landscape(void)
{
    return s_info.log_w > s_info.log_h;
}

int gc_disp_scale_permille(void)
{
    return s_info.scale_permille;
}

float gc_disp_scale(void)
{
    return (float)s_info.scale_permille / 1000.0f;
}

int gc_disp_s(int design_px)
{
    if (design_px == 0) {
        return 0;
    }
    int sign = (design_px < 0) ? -1 : 1;
    int mag = (design_px < 0) ? -design_px : design_px;

    /* 四舍五入，避免小尺寸被截断成 0 */
    int v = (int)(((int64_t)mag * s_info.scale_permille + 500) / 1000);
    if (v < 1) {
        v = 1;
    }
    return sign * v;
}

int gc_disp_s_min(int design_px, int min_px)
{
    int v = gc_disp_s(design_px);
    return (v < min_px) ? min_px : v;
}
