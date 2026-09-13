/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_net.h
 * @brief 网络层：ESP32-P4 通过 ESP32-C6 (esp_wifi_remote + esp-hosted) 连 Wi-Fi
 *
 * 设计要点：
 *   - 对上层只暴露标准 STA 语义，隐藏 P4 无内置 Wi-Fi 的事实；
 *   - 凭据存 NVS，重启后自动重连；
 *   - 断线后由内部守护任务自动无限重连，"页面切换"不参与 Wi-Fi 生命周期。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_NET_SSID_MAX_LEN 33
#define GC_NET_PASS_MAX_LEN 65
#define GC_NET_SCAN_MAX     24

typedef struct {
    char    ssid[GC_NET_SSID_MAX_LEN];
    int8_t  rssi;
    uint8_t authmode;   /*!< wifi_auth_mode_t */
    bool    encrypted;
} gc_net_ap_info_t;

typedef struct {
    gc_net_ap_info_t aps[GC_NET_SCAN_MAX];
    uint16_t         count;
} gc_net_scan_result_t;

typedef struct {
    const char *ssid;          /*!< AP 名称 */
    const char *password;      /*!< AP 密码，开放网络传 NULL */
    TickType_t  timeout_ticks; /*!< 等待拿到 IP 的超时（tick） */
    int         max_retry;     /*!< 首次连接阶段的重试次数 */
} gc_net_sta_config_t;

typedef enum {
    GC_NET_EVT_IDLE = 0,
    GC_NET_EVT_CONNECTING,
    GC_NET_EVT_CONNECTED,
    GC_NET_EVT_DISCONNECTED,
    GC_NET_EVT_FAILED,
} gc_net_event_t;

/** 网络状态变化回调（在 Wi-Fi 事件上下文中调用，勿做阻塞操作） */
typedef void (*gc_net_event_cb_t)(gc_net_event_t evt, void *ctx);

/* ---------------- 生命周期 ---------------- */

/**
 * @brief 初始化 NVS / netif / event loop / Wi-Fi（remote）。可重复调用。
 */
esp_err_t gc_net_init(void);

/**
 * @brief 以 STA 模式连接 AP，阻塞直到拿到 IPv4 或超时
 */
esp_err_t gc_net_sta_connect(const gc_net_sta_config_t *cfg);

/**
 * @brief 用 NVS 中保存的凭据连接；无凭据返回 ESP_ERR_NOT_FOUND
 */
esp_err_t gc_net_auto_connect(TickType_t timeout_ticks);

/**
 * @brief 主动断开（仅在用户明确"忘记网络"时调用）
 */
esp_err_t gc_net_sta_disconnect(void);

bool gc_net_is_connected(void);

esp_err_t gc_net_wait_connected(TickType_t timeout_ticks);

esp_err_t gc_net_get_ip(char *buf, size_t len);

/* ---------------- 扫描 ---------------- */

/**
 * @brief 阻塞扫描周边 AP，结果按信号强度降序
 */
esp_err_t gc_net_scan(gc_net_scan_result_t *out);

/* ---------------- 凭据持久化 ---------------- */

esp_err_t gc_net_save_credentials(const char *ssid, const char *password);
esp_err_t gc_net_load_credentials(char *ssid, size_t ssid_len,
                                  char *password, size_t pass_len);
bool      gc_net_has_credentials(void);
esp_err_t gc_net_clear_credentials(void);

/* ---------------- 保持连接 ---------------- */

/**
 * @brief 开启/关闭后台自动重连（默认开启）
 *
 * 开启后：断线 -> 守护任务按 6s 间隔无限重试，直到连上或调用本函数关闭。
 */
void gc_net_set_auto_reconnect(bool enable);
bool gc_net_get_auto_reconnect(void);

/**
 * @brief 注册状态变化回调，用于 UI 状态栏
 */
esp_err_t gc_net_register_event_cb(gc_net_event_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
