/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_ble.h
 * @brief 蓝牙（NimBLE）组件
 *
 * 8BitGo 本体（ESP32-P4）没有射频，蓝牙经板载 ESP32-C6 提供。C6 已开启
 * NumBLE（esp-hosted + NimBLE），P4 这侧运行 NimBLE 主机协议栈，把本机
 * 暴露为一个 BLE 设备：
 *   - Battery Service（BAS）：上报电池电压折算的电量百分比；
 *   - 自定义「8BitGo Gamepad」服务：以 notify 方式把虚拟手柄的按键 /
 *     摇杆状态推送给已配对的手机 / PC（可当作无线手柄使用）。
 *
 * 全部 NimBLE 代码都用 CONFIG_BT_NIMBLE_ENABLED 包裹：未启用时本组件只提供
 * 空实现，不会引入任何 NimBLE 符号，构建不受影响。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化并启动蓝牙（NimBLE 主机协议栈）。未启用 BT 时直接返回 ESP_OK。 */
esp_err_t gc_ble_init(void);

/** 停用蓝牙（释放协议栈）。 */
void gc_ble_deinit(void);

/** 蓝牙协议栈是否已初始化 */
bool gc_ble_is_enabled(void);

/** 是否正在广播（等待配对） */
bool gc_ble_is_advertising(void);

/** 填充一段状态短文本（供设置页展示），如 "已启用" / "未启用" / "未编译" */
void gc_ble_status_text(char *buf, size_t len);

/**
 * @brief 上报手柄状态（按键位图 + 两轴）
 *
 * @param buttons 16 位按键位图（位 0..15 对应 A/B/X/Y/L/R/...，由调用方约定）
 * @param lx,ly   左摇杆，范围 -127..127
 * @return 未启用 BT 或无人订阅时返回 ESP_ERR_INVALID_STATE
 */
esp_err_t gc_ble_gamepad_report(uint16_t buttons, int8_t lx, int8_t ly);

/** 上报电池电量百分比（0..100），用于 BAS notify */
esp_err_t gc_ble_battery_set(uint8_t percent);

#ifdef __cplusplus
}
#endif
