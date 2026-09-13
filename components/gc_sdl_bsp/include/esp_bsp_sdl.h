/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file esp_bsp_sdl.h
 * @brief SDL3 esp-idf video driver 所需的板级抽象（shim）
 *
 * 与 georgik/esp-idf-component-SDL_bsp 的接口保持同名同签名，
 * 但由本工程自己的 panel / touch 句柄实现（main 启动时先建好 LCD，
 * 再通过 gc_sdl_bsp_attach() 移交给 SDL）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 本板带触摸（SDL_espidftouch.c 由该宏启用） */
#define ESP_BSP_SDL_TOUCH_SUPPORT 1

typedef struct {
    int width;               /*!< 显示宽（像素） */
    int height;              /*!< 显示高（像素） */
    int pixel_format;        /*!< SDL 像素格式 */
    size_t max_transfer_sz;  /*!< 单次传输上限 */
    bool has_touch;          /*!< 是否有触摸 */
} esp_bsp_sdl_display_config_t;

typedef struct {
    bool pressed;            /*!< 是否按下 */
    int x;                   /*!< 面板物理 X */
    int y;                   /*!< 面板物理 Y */
} esp_bsp_sdl_touch_info_t;

/* ---- 以下由 video driver 调用 ---- */

esp_err_t esp_bsp_sdl_init(esp_bsp_sdl_display_config_t *config,
                           esp_lcd_panel_handle_t *panel_handle,
                           esp_lcd_panel_io_handle_t *panel_io_handle);
esp_err_t esp_bsp_sdl_deinit(void);
esp_err_t esp_bsp_sdl_backlight_on(void);
esp_err_t esp_bsp_sdl_backlight_off(void);
esp_err_t esp_bsp_sdl_display_on_off(bool enable);
esp_err_t esp_bsp_sdl_touch_init(void);
esp_err_t esp_bsp_sdl_touch_read(esp_bsp_sdl_touch_info_t *touch_info);
const char *esp_bsp_sdl_get_board_name(void);

/* ---- 应用侧：把已初始化的 LCD / 触摸句柄移交给 SDL ---- */

esp_err_t gc_sdl_bsp_attach(esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch);

/** 设置显示模式（须在 SDL_Init 前调用；pixel_format 为 SDL_PixelFormat 枚举值） */
void gc_sdl_bsp_set_display(int w, int h, int pixel_format);

/** 启用外部触摸采样：禁用 SDL 驱动内置轮询，由应用自建任务调用
 *  esp_bsp_sdl_touch_read() 并自行推送事件（须在 SDL_Init 前调用） */
void gc_sdl_bsp_use_external_touch(bool en);

#ifdef __cplusplus
}
#endif
