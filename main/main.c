/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file main.c
 * @brief 8BitGo 客户端应用编排
 *
 * 启动流程：
 *   启动页（LOGO + 进度条，同时准备存储 / 注册并预热模拟器核心）
 *   -> 主页（8BitGo 云库 / SD 卡本地 ROM 双数据源，封面懒加载、分页加载）
 *   -> 游玩页（模拟器 + 游戏简介）
 *   右上角「设置」-> Wi-Fi / 蓝牙 / 模拟器核心 / GitHub / About
 *
 * 线程约定（重要）：
 *   - 所有 LVGL 操作都在 gc_ui_lock/gc_ui_unlock 内完成；
 *   - 网络 / 下载 / SD 扫描一律放在后台任务里，绝不阻塞 LVGL 事件回调；
 *   - 页面数据（s_games / s_sd_roms）由后台任务写入（见 components/gc_library），
 *     写入时持 UI 锁，保证 UI 线程读到的列表始终一致（避免 realloc 造成的悬垂指针）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "bsp/esp32_p4_function_ev_board.h"
#include "bsp/display.h"
#include "bsp/touch.h"

#include "gc_net.h"
#include "gc_api.h"
#include "gc_store.h"
#include "gc_emu.h"
#include "gc_sys.h"
#include "gc_ui.h"
#include "gc_provision.h"
#include "gc_library.h"
#include "gc_ble.h"
#include "esp_codec_dev.h"

static const char *TAG = "8bitgo";

/* ==================== 配网流程同步 ==================== */
/* （配网向导已抽到 components/gc_provision） */

static esp_lcd_panel_handle_t s_panel;

/* 前向声明 */
static void on_settings_action(gc_ui_settings_action_t action, void *ctx);

/* ==================== 网络事件 ==================== */

static void on_net_event(gc_net_event_t evt, void *ctx)
{
    (void)ctx;
    switch (evt) {
    case GC_NET_EVT_CONNECTING:
        gc_ui_update_net_status("Wi-Fi 连接中...");
        break;
    case GC_NET_EVT_CONNECTED:
        gc_library_on_connected(NULL);
        break;
    case GC_NET_EVT_DISCONNECTED:
        gc_ui_update_net_status("Wi-Fi 已断开，重连中...");
        break;
    case GC_NET_EVT_FAILED:
        gc_ui_update_net_status("Wi-Fi 连接失败");
        break;
    default:
        break;
    }
}

/* ==================== 游戏库编排 ==================== */
/* 云库分页加载 / 封面懒加载 / SD 扫描 / 模拟器主循环 / 主页回调
 * 已抽到 components/gc_library（gc_library_init / gc_library_bind_ui /
 * gc_library_on_connected），本文件不再持有游戏列表状态。 */

/* ==================== 设置 ==================== */

static void on_open_settings(void *ctx)
{
    (void)ctx;
    gc_ui_show_settings(on_settings_action, NULL);
}

static void on_settings_action(gc_ui_settings_action_t action, void *ctx)
{
    (void)ctx;
    if (action == GC_UI_SET_WIFI) {
        gc_provision_start();
    }
}

/* ==================== 启动引导 ==================== */

static void boot_task(void *arg)
{
    (void)arg;

    gc_ui_boot_progress(10, "初始化存储 ...");
    esp_err_t st = gc_store_init();
    if (st != ESP_OK) {
        ESP_LOGW(TAG, "no usable storage (%s)", esp_err_to_name(st));
    }

    gc_ui_boot_progress(30, "识别设备芯片 ...");
    gc_sys_probe();                 /* 探测芯片型号/核数/主频/PSRAM -> 性能档位 */
    gc_sys_tune_for_emulation();    /* 锁最高主频、关省电抖动，模拟器优先 */

    gc_ui_boot_progress(44, "注册模拟器核心 ...");
    gc_emu_init();
    gc_emu_gb_register();

    gc_ui_boot_progress(58, "预热模拟器核心 ...");
    gc_emu_warmup();

    gc_library_init(); /* 分配 SD ROM 缓冲（PSRAM） */
    gc_ui_boot_progress(80, "扫描 SD 卡 ...");
    gc_library_bind_ui(); /* 扫描 SD + 绑定主页列表 */
    gc_ui_boot_progress(100, "准备就绪");
    vTaskDelay(pdMS_TO_TICKS(260)); /* 让用户看清 LOGO 与进度条 */

    gc_ui_home_show(GC_UI_SRC_CLOUD, false);

    /* 有凭据则后台自动回连并拉云库；没有则等用户去设置里配网 */
    if (gc_net_has_credentials()) {
        gc_provision_start();
    } else {
        ESP_LOGI(TAG, "no Wi-Fi credentials, waiting for user setup");
    }

    vTaskDelete(NULL);
}

/* ==================== 入口 ==================== */

void app_main(void)
{
    ESP_LOGI(TAG, "8BitGo client starting");

    /* 1. 屏幕 / 触控 */
    bsp_display_config_t disp_cfg = {.dummy = 0};
    esp_lcd_panel_io_handle_t io = NULL;
    if (bsp_display_new(&disp_cfg, &s_panel, &io) != ESP_OK) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    esp_lcd_touch_handle_t touch = NULL;
    bsp_touch_config_t touch_cfg = {.dummy = 0};
    if (bsp_touch_new(&touch_cfg, &touch) != ESP_OK) {
        ESP_LOGW(TAG, "touch init failed");
    }
    bsp_display_backlight_on();

    /* 2. UI（先显示启动页） */
    if (gc_ui_init(s_panel, io, touch, BSP_LCD_H_RES, BSP_LCD_V_RES) != ESP_OK) {
        ESP_LOGE(TAG, "ui init failed");
        return;
    }
    gc_ui_update_net_status("Wi-Fi 未连接");
    gc_ui_boot_show();

    /* 3. 网络：gc_net 单例管理，断线自动重连；状态由事件回调刷新状态栏 */
    gc_provision_init();
    gc_provision_set_ready_cb(gc_library_on_connected, NULL);

    gc_net_init();
    gc_net_register_event_cb(on_net_event, NULL);
    gc_net_set_auto_reconnect(true); /* 任何页面都不主动断开，断线无限重连 */

    /* 4. 主页设置回调 + 游戏库绑定（SD 扫描 / 列表绑定在 gc_library_bind_ui 内完成） */
    gc_ui_set_home_settings_cb(on_open_settings, NULL);

    /* 5. SD ROM 缓冲放 PSRAM（在 boot_task 里由 gc_library_init 分配） */

    /* 6. 引导任务：存储 / 核心预热 -> 主页 */
    xTaskCreatePinnedToCore(boot_task, "gc_boot", 8192, NULL, 5, NULL, 0);
}
