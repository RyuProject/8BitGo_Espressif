/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_sdl_bsp.c
 * @brief SDL3 esp-idf video driver 的板级 shim：复用 main 已建好的 panel/touch
 */

#include "esp_bsp_sdl.h"

#include <string.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static int s_disp_w, s_disp_h, s_pixel_format;
static bool s_ext_touch;   /* true: 应用自建触摸任务采样，禁用 SDL 驱动内轮询 */

esp_err_t gc_sdl_bsp_attach(esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch)
{
    s_panel = panel;
    s_touch = touch;
    return ESP_OK;
}

void gc_sdl_bsp_set_display(int w, int h, int pixel_format)
{
    s_disp_w = w;
    s_disp_h = h;
    s_pixel_format = pixel_format;
}

void gc_sdl_bsp_use_external_touch(bool en)
{
    s_ext_touch = en;
}

esp_err_t esp_bsp_sdl_init(esp_bsp_sdl_display_config_t *config,
                           esp_lcd_panel_handle_t *panel_handle,
                           esp_lcd_panel_io_handle_t *panel_io_handle)
{
    if (!s_panel || !panel_handle || !config) {
        return ESP_ERR_INVALID_STATE;
    }
    /* driver 不会填 config，由本 shim 提供显示模式 */
    config->width = s_disp_w;
    config->height = s_disp_h;
    config->pixel_format = s_pixel_format;
    config->max_transfer_sz = 0;
    config->has_touch = (s_touch != NULL) && !s_ext_touch;

    *panel_handle = s_panel;
    if (panel_io_handle) {
        *panel_io_handle = NULL; /* DPI 面板不需要 DBI IO 做呈现 */
    }
    return ESP_OK;
}

esp_err_t esp_bsp_sdl_deinit(void)
{
    return ESP_OK;
}

esp_err_t esp_bsp_sdl_backlight_on(void)
{
    return ESP_OK; /* 背光由 main 的 BSP 初始化负责 */
}

esp_err_t esp_bsp_sdl_backlight_off(void)
{
    return ESP_OK;
}

esp_err_t esp_bsp_sdl_display_on_off(bool enable)
{
    return s_panel ? esp_lcd_panel_disp_on_off(s_panel, enable) : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_bsp_sdl_touch_init(void)
{
    /* 外部采样模式下告知 SDL 驱动"无触摸"，避免其每帧轮询与
       应用的独立触摸任务双重采样 */
    return (s_touch && !s_ext_touch) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_bsp_sdl_touch_read(esp_bsp_sdl_touch_info_t *touch_info)
{
    if (!s_touch || !touch_info) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* 与原 LVGL 版一致：限频由上层做，这里单点读取。
       坐标为面板物理坐标（esp_lcd_touch 已按 mirror 标志归一化）。 */
    esp_lcd_touch_point_data_t pt = {0};
    uint8_t cnt = 0;
    if (esp_lcd_touch_read_data(s_touch) == ESP_OK) {
        esp_lcd_touch_get_data(s_touch, &pt, &cnt, 1);
    }
    touch_info->pressed = (cnt > 0);
    touch_info->x = pt.x;
    touch_info->y = pt.y;
    return ESP_OK;
}

const char *esp_bsp_sdl_get_board_name(void)
{
    return "8bitgo-esp32p4";
}
