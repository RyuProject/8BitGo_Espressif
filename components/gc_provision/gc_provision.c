/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_provision.c
 * @brief Wi-Fi 配网向导（从 main.c 迁移）
 *
 * 仅负责「连上 Wi-Fi」这一步：自动回连 or 扫描/选择/输密码/连接。
 * 连上之后做什么（拉云库、初始化存储）由上层通过 ready_cb 决定，本组件不关心。
 */

#include "gc_provision.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "gc_net.h"
#include "gc_ui.h"

static const char *TAG = "gc_provision";

/* ==================== 配网流程同步 ==================== */

static SemaphoreHandle_t s_ap_sem;
static SemaphoreHandle_t s_pwd_sem;
static SemaphoreHandle_t s_rescan_sem;
static size_t            s_sel_index;
static char              s_input_pass[GC_NET_PASS_MAX_LEN];
static char              s_conn_ssid[GC_NET_SSID_MAX_LEN];
static bool              s_pwd_cancelled;
static bool              s_manual_mode;
static volatile bool     s_setup_running;
static volatile bool     s_setup_abort;

static gc_net_scan_result_t s_scan;

static void (*s_ready_cb)(void *ctx);
static void *s_ready_ctx;

/* ==================== 生命周期 ==================== */

esp_err_t gc_provision_init(void)
{
    if (s_ap_sem) {
        return ESP_OK; /* 已初始化 */
    }
    s_ap_sem = xSemaphoreCreateBinary();
    s_pwd_sem = xSemaphoreCreateBinary();
    s_rescan_sem = xSemaphoreCreateBinary();
    if (!s_ap_sem || !s_pwd_sem || !s_rescan_sem) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void gc_provision_set_ready_cb(void (*cb)(void *ctx), void *ctx)
{
    s_ready_cb = cb;
    s_ready_ctx = ctx;
}

static void fire_ready(void)
{
    if (s_ready_cb) {
        s_ready_cb(s_ready_ctx);
    }
}

void gc_provision_cancel(void)
{
    s_setup_abort = true;
    if (s_ap_sem) {
        xSemaphoreGive(s_ap_sem);
    }
    if (s_pwd_sem) {
        xSemaphoreGive(s_pwd_sem);
    }
    if (s_rescan_sem) {
        xSemaphoreGive(s_rescan_sem);
    }
}

bool gc_provision_is_running(void)
{
    return s_setup_running;
}

/* ==================== 配网页交互回调 ==================== */

static void on_ap_selected(size_t index, void *ctx)
{
    (void)ctx;
    s_sel_index = index;
    xSemaphoreGive(s_ap_sem);
}

static void on_password(const char *ssid, const char *password, void *ctx)
{
    (void)ctx;
    if (!password) {
        s_pwd_cancelled = true;
    } else {
        s_pwd_cancelled = false;
        if (ssid) {
            strlcpy(s_conn_ssid, ssid, sizeof(s_conn_ssid));
        }
        strlcpy(s_input_pass, password, sizeof(s_input_pass));
    }
    xSemaphoreGive(s_pwd_sem);
}

static void on_rescan(void *ctx)
{
    (void)ctx;
    xSemaphoreGive(s_rescan_sem);
    xSemaphoreGive(s_ap_sem); /* 唤醒等待中的主流程，让它 Rescan */
}

static void on_manual(void *ctx)
{
    (void)ctx;
    s_manual_mode = true;
    xSemaphoreGive(s_ap_sem);
}

/** 用户在配网页点了返回：结束配网任务，避免后台任务悬挂 */
static void on_wifi_cancel(void *ctx)
{
    (void)ctx;
    s_setup_abort = true;
    if (s_ap_sem) {
        xSemaphoreGive(s_ap_sem);
    }
    if (s_pwd_sem) {
        xSemaphoreGive(s_pwd_sem);
    }
    if (s_rescan_sem) {
        xSemaphoreGive(s_rescan_sem);
    }
}

/* ==================== 配网主流程 ==================== */

static void net_setup_task(void *arg)
{
    (void)arg;
    s_setup_running = true;
    s_setup_abort = false;

    /* gc_net 已在 app_main 初始化并开启自动重连；这里只负责配网 UI 流程 */

    /* 1) 优先用已保存凭据自动回连 */
    if (gc_net_has_credentials()) {
        gc_ui_update_net_status("Wi-Fi 连接中...");
        if (gc_net_auto_connect(pdMS_TO_TICKS(CONFIG_GC_WIFI_CONNECT_TIMEOUT_MS)) == ESP_OK) {
            ESP_LOGI(TAG, "auto connected with saved credentials");
            fire_ready();
            s_setup_running = false;
            vTaskDelete(NULL);
            return;
        }
        /* 已保存过凭据但这次没连上（偶发 assoc 失败很常见）：
           不要打断用户去弹配网页——gc_net 的守护任务会继续无限重连，
           连上后 on_net_event(CONNECTED) 会自动把云库拉下来。
           想主动改网络时，从「设置 - Wi-Fi」进来即可。 */
        ESP_LOGW(TAG, "auto connect failed, keep retrying in background");
        gc_ui_update_net_status("Wi-Fi 重连中...");
        s_setup_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* 2) 进入配网页：扫描 -> 选 AP -> 输密码 -> 连接 */
    gc_ui_set_wifi_rescan_cb(on_rescan, NULL);
    gc_ui_set_wifi_manual_cb(on_manual, NULL);
    gc_ui_set_wifi_cancel_cb(on_wifi_cancel, NULL);

    while (!s_setup_abort) {
        s_manual_mode = false;

        gc_ui_show_wifi_scanning();
        if (gc_net_scan(&s_scan) != ESP_OK) {
            gc_ui_show_wifi_result(false, "扫描失败，请重试");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        /* 刚断开连接时 C6 侧还没稳定，第一次扫描常常是 0 个 AP；
           等一会儿再扫一次，避免用户看到空列表以为设备坏了。 */
        if (s_scan.count == 0) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            gc_net_scan(&s_scan);
        }
        gc_ui_show_wifi_scan(&s_scan, on_ap_selected, NULL);

        /* 等待用户选择 / Rescan / Manual */
        xSemaphoreTake(s_rescan_sem, 0);
        xSemaphoreTake(s_ap_sem, portMAX_DELAY);
        if (s_setup_abort) {
            break;
        }
        if (xSemaphoreTake(s_rescan_sem, 0) == pdTRUE) {
            continue;
        }

        char pass[GC_NET_PASS_MAX_LEN];

        if (s_manual_mode) {
            gc_ui_show_wifi_password("", true, on_password, NULL);
        } else {
            if (s_sel_index >= s_scan.count) {
                continue;
            }
            const gc_net_ap_info_t *ap = &s_scan.aps[s_sel_index];
            strlcpy(s_conn_ssid, ap->ssid, sizeof(s_conn_ssid));
            if (ap->encrypted) {
                gc_ui_show_wifi_password(s_conn_ssid, false, on_password, NULL);
            }
        }

        bool need_pass = s_manual_mode ||
                         (s_sel_index < s_scan.count && s_scan.aps[s_sel_index].encrypted);
        if (need_pass) {
            xSemaphoreTake(s_pwd_sem, portMAX_DELAY);
            if (s_setup_abort) {
                break;
            }
            if (s_pwd_cancelled) {
                continue;
            }
            strlcpy(pass, s_input_pass, sizeof(pass));
        } else {
            pass[0] = '\0';
        }

        gc_ui_show_wifi_connecting(s_conn_ssid);

        gc_net_sta_config_t cfg = {
            .ssid = s_conn_ssid,
            .password = pass[0] ? pass : NULL,
            .timeout_ticks = pdMS_TO_TICKS(CONFIG_GC_WIFI_CONNECT_TIMEOUT_MS),
            .max_retry = 3,
        };

        if (gc_net_sta_connect(&cfg) == ESP_OK) {
            char ip[32];
            gc_net_get_ip(ip, sizeof(ip));
            gc_ui_show_wifi_result(true, ip);
            ESP_LOGI(TAG, "connected to %s, ip=%s", s_conn_ssid, ip);
            vTaskDelay(pdMS_TO_TICKS(700));
            fire_ready();
            gc_ui_home_refresh();
            s_setup_running = false;
            vTaskDelete(NULL);
            return;
        }

        gc_ui_show_wifi_result(false, "密码错误或信号太弱，请重试");
        vTaskDelay(pdMS_TO_TICKS(2200));
    }

    /* 用户主动返回配网页：什么都不做（他已经回到设置页），只收尾任务 */
    s_setup_running = false;
    vTaskDelete(NULL);
}

void gc_provision_start(void)
{
    if (s_setup_running) {
        ESP_LOGW(TAG, "wifi setup already running");
        return;
    }
    xTaskCreatePinnedToCore(net_setup_task, "gc_net_setup", 16384, NULL, 5, NULL, 0);
}
