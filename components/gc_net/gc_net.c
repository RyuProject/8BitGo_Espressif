/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gc_net.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "gc_net";

#if !defined(CONFIG_ESP_HOSTED_HOST) && !defined(CONFIG_ESP_HOST_WIFI_ENABLED)
#warning "ESP32-P4 没有内置 Wi-Fi：需要启用 esp-hosted（CONFIG_ESP_HOSTED_HOST）" \
         "或 esp_wifi_remote 的其它后端，否则 esp_wifi_* 无法真正联网。"
#endif

#define GC_NET_CONNECTED_BIT  BIT0
#define GC_NET_FAIL_BIT       BIT1
#define GC_NET_RECONNECT_BIT  BIT2

#define NVS_NAMESPACE "gc_net"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

#define GC_RECONNECT_INTERVAL_TICKS pdMS_TO_TICKS(6000)

static EventGroupHandle_t s_evt_group;
static esp_netif_t       *s_sta_netif;
static bool               s_inited;
static bool               s_started;
static bool               s_auto_reconnect = true;
static bool               s_ever_connected;
static int                s_first_retry;
static TaskHandle_t       s_reconnect_task;
static gc_net_event_cb_t  s_evt_cb;
static void              *s_evt_ctx;

/* ================= 事件 ================= */

static void notify_evt(gc_net_event_t evt)
{
    if (s_evt_cb) {
        s_evt_cb(evt, s_evt_ctx);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            /* 不在这里自动 connect：由 gc_net_sta_connect / 重连守护显式发起，
               否则 STA 会拿驱动里残留的配置反复重连，干扰扫描。 */
            ESP_LOGI(TAG, "STA start");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "disconnected, reason=%d", dis->reason);
            xEventGroupClearBits(s_evt_group, GC_NET_CONNECTED_BIT);
            notify_evt(GC_NET_EVT_DISCONNECTED);

            if (!s_ever_connected) {
                /* 首次连接阶段：有限次重试，让 gc_net_sta_connect 能返回失败 */
                if (s_first_retry < 3) {
                    s_first_retry++;
                    esp_wifi_connect();
                } else {
                    xEventGroupSetBits(s_evt_group, GC_NET_FAIL_BIT);
                    notify_evt(GC_NET_EVT_FAILED);
                }
            } else {
                /* 已经连上过：交给后台守护任务无限重连，保证"任何页面都保持连接" */
                xEventGroupSetBits(s_evt_group, GC_NET_RECONNECT_BIT);
            }
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_first_retry = 0;
        s_ever_connected = true;
        xEventGroupSetBits(s_evt_group, GC_NET_CONNECTED_BIT);
        notify_evt(GC_NET_EVT_CONNECTED);
    }
}

/* ================= 后台重连守护 ================= */

static void reconnect_task(void *arg)
{
    (void)arg;
    char ssid[GC_NET_SSID_MAX_LEN];
    char pass[GC_NET_PASS_MAX_LEN];

    while (1) {
        /* 等待断线事件 */
        xEventGroupWaitBits(s_evt_group, GC_NET_RECONNECT_BIT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (!s_auto_reconnect) {
            continue;
        }
        if (gc_net_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
            ESP_LOGW(TAG, "no saved credentials, skip reconnect");
            continue;
        }

        ESP_LOGI(TAG, "auto reconnecting to %s ...", ssid);
        notify_evt(GC_NET_EVT_CONNECTING);

        while (s_auto_reconnect && !gc_net_is_connected()) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "connect ret %s", esp_err_to_name(err));
            }
            EventBits_t bits = xEventGroupWaitBits(s_evt_group, GC_NET_CONNECTED_BIT,
                                                   pdFALSE, pdTRUE, GC_RECONNECT_INTERVAL_TICKS);
            if (bits & GC_NET_CONNECTED_BIT) {
                ESP_LOGI(TAG, "reconnected");
                break;
            }
        }
    }
}

/* ================= 初始化 ================= */

esp_err_t gc_net_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif, ESP_FAIL, TAG, "create default wifi sta failed");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init failed");

    s_evt_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_evt_group, ESP_ERR_NO_MEM, TAG, "create event group failed");

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");

    /* 没有保存过的凭据时，清掉驱动里可能残留的 STA 配置，
       避免 start() 后自动去连一个无效的旧 AP（会持续刷 disconnected 并干扰扫描）。 */
    if (!gc_net_has_credentials()) {
        ESP_LOGI(TAG, "no saved credentials, clear stale STA config");
        wifi_config_t empty = {0};
        esp_wifi_set_config(WIFI_IF_STA, &empty);
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    s_started = true;

    BaseType_t ok = xTaskCreatePinnedToCore(reconnect_task, "gc_net_rec", 4096, NULL, 4,
                                            &s_reconnect_task, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create reconnect task failed");

    s_inited = true;
    return ESP_OK;
}

/* ================= 连接 ================= */

esp_err_t gc_net_sta_connect(const gc_net_sta_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->ssid && cfg->ssid[0], ESP_ERR_INVALID_ARG, TAG, "invalid config");
    ESP_RETURN_ON_ERROR(gc_net_init(), TAG, "net init failed");

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, cfg->ssid, sizeof(wifi_cfg.sta.ssid));
    if (cfg->password && cfg->password[0] != '\0') {
        strlcpy((char *)wifi_cfg.sta.password, cfg->password, sizeof(wifi_cfg.sta.password));
    }
    wifi_cfg.sta.threshold.authmode = (cfg->password && cfg->password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config failed");

    s_first_retry = 0;
    xEventGroupClearBits(s_evt_group, GC_NET_CONNECTED_BIT | GC_NET_FAIL_BIT);
    notify_evt(GC_NET_EVT_CONNECTING);

    if (!s_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        s_started = true;
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi connect failed");
    }

    TickType_t timeout = cfg->timeout_ticks ? cfg->timeout_ticks : pdMS_TO_TICKS(20000);
    EventBits_t bits = xEventGroupWaitBits(s_evt_group,
                                           GC_NET_CONNECTED_BIT | GC_NET_FAIL_BIT,
                                           pdFALSE, pdFALSE, timeout);

    if (bits & GC_NET_CONNECTED_BIT) {
        /* 成功后缓存凭据，供后续自动重连/开机自动连接使用 */
        gc_net_save_credentials(cfg->ssid, cfg->password ? cfg->password : "");
        return ESP_OK;
    }
    notify_evt(GC_NET_EVT_FAILED);
    return (bits & GC_NET_FAIL_BIT) ? ESP_ERR_WIFI_PASSWORD : ESP_ERR_TIMEOUT;
}

esp_err_t gc_net_auto_connect(TickType_t timeout_ticks)
{
    char ssid[GC_NET_SSID_MAX_LEN];
    char pass[GC_NET_PASS_MAX_LEN];

    if (gc_net_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "auto connect to saved AP: %s", ssid);

    gc_net_sta_config_t cfg = {
        .ssid = ssid,
        .password = pass[0] ? pass : NULL,
        .timeout_ticks = timeout_ticks ? timeout_ticks : pdMS_TO_TICKS(20000),
        .max_retry = 3,
    };
    return gc_net_sta_connect(&cfg);
}

esp_err_t gc_net_sta_disconnect(void)
{
    if (!s_started) {
        return ESP_OK;
    }
    s_started = false;
    s_ever_connected = false;
    xEventGroupClearBits(s_evt_group, GC_NET_CONNECTED_BIT | GC_NET_FAIL_BIT | GC_NET_RECONNECT_BIT);
    ESP_RETURN_ON_ERROR(esp_wifi_disconnect(), TAG, "disconnect failed");
    return esp_wifi_stop();
}

bool gc_net_is_connected(void)
{
    if (!s_evt_group) {
        return false;
    }
    return (xEventGroupGetBits(s_evt_group) & GC_NET_CONNECTED_BIT) != 0;
}

esp_err_t gc_net_wait_connected(TickType_t timeout_ticks)
{
    if (!s_evt_group) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_evt_group, GC_NET_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, timeout_ticks);
    return (bits & GC_NET_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t gc_net_get_ip(char *buf, size_t len)
{
    ESP_RETURN_ON_FALSE(buf && len, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    if (!s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip_info;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_sta_netif, &ip_info), TAG, "get ip info failed");
    if (ip_info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

/* ================= 扫描 ================= */

static int ap_cmp(const void *a, const void *b)
{
    const gc_net_ap_info_t *pa = (const gc_net_ap_info_t *)a;
    const gc_net_ap_info_t *pb = (const gc_net_ap_info_t *)b;
    return pb->rssi - pa->rssi; /* 信号强的在前 */
}

esp_err_t gc_net_scan(gc_net_scan_result_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    ESP_RETURN_ON_ERROR(gc_net_init(), TAG, "net init failed");

    memset(out, 0, sizeof(*out));

    /* 扫描期间临时关闭后台自动重连：否则下面的 esp_wifi_disconnect() 会触发
       DISCONNECTED 事件，后台守护任务立刻 esp_wifi_connect()，与正在进行的扫描
       竞争。在部分 esp-hosted 从机固件（如 C6 3.0.7）上这会触发对端 assert，导致
       C6 重启 / SDIO 断开 / P4 端 panic（表现为点 Wi-Fi 后蓝屏）。 */
    bool prev_ar = gc_net_get_auto_reconnect();
    gc_net_set_auto_reconnect(false);

    /* 扫描前先脱离当前连接/连接尝试，否则会得到 0 个结果。
       关键：esp_wifi_disconnect() 后必须等 C6 侧确认真断开，再发 scan_start——
       否则两条 RPC 命令在 C6 3.0.7 侧串行竞争，触发对端 assert ->
       C6 重启 -> SDIO 断开 -> P4 panic(点 Wi-Fi 蓝屏)。原来固定 200ms 延时
       不够稳妥（从机处理断开有波动），改为轮询断开确认(上限 1s)。 */
    esp_wifi_disconnect();
    {
        int64_t t0 = esp_timer_get_time();
        while (gc_net_is_connected() && (esp_timer_get_time() - t0) < 1000000) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(err));
        gc_net_set_auto_reconnect(prev_ar);
        return err;
    }
    ESP_LOGI(TAG, "scan took %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));

    uint16_t num = GC_NET_SCAN_MAX;
    wifi_ap_record_t recs[GC_NET_SCAN_MAX];
    err = esp_wifi_scan_get_ap_records(&num, recs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get ap records failed: %s", esp_err_to_name(err));
        gc_net_set_auto_reconnect(prev_ar);
        return err;
    }

    for (uint16_t i = 0; i < num && i < GC_NET_SCAN_MAX; i++) {
        gc_net_ap_info_t *ap = &out->aps[i];
        strlcpy(ap->ssid, (const char *)recs[i].ssid, sizeof(ap->ssid));
        ap->rssi = recs[i].rssi;
        ap->authmode = recs[i].authmode;
        ap->encrypted = (recs[i].authmode != WIFI_AUTH_OPEN);
    }
    out->count = num;

    if (num > 1) {
        qsort(out->aps, num, sizeof(gc_net_ap_info_t), ap_cmp);
    }

    ESP_LOGI(TAG, "scan done: %u APs", num);
    gc_net_set_auto_reconnect(prev_ar);
    return ESP_OK;
}

/* ================= 凭据 ================= */

esp_err_t gc_net_save_credentials(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    ESP_RETURN_ON_ERROR(gc_net_init(), TAG, "net init failed");

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t gc_net_load_credentials(char *ssid, size_t ssid_len,
                                  char *password, size_t pass_len)
{
    ESP_RETURN_ON_FALSE(ssid && ssid_len, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = ssid_len;
    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    if (password && pass_len) {
        size_t plen = pass_len;
        if (nvs_get_str(h, NVS_KEY_PASS, password, &plen) != ESP_OK) {
            password[0] = '\0';
        }
    }
    nvs_close(h);
    return (ssid[0] != '\0') ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool gc_net_has_credentials(void)
{
    char ssid[GC_NET_SSID_MAX_LEN];
    return gc_net_load_credentials(ssid, sizeof(ssid), NULL, 0) == ESP_OK;
}

esp_err_t gc_net_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(h, NVS_KEY_SSID);
    nvs_erase_key(h, NVS_KEY_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ================= 自动重连 / 回调 ================= */

void gc_net_set_auto_reconnect(bool enable)
{
    s_auto_reconnect = enable;
    if (!enable && s_evt_group) {
        xEventGroupClearBits(s_evt_group, GC_NET_RECONNECT_BIT);
    }
    ESP_LOGI(TAG, "auto reconnect %s", enable ? "on" : "off");
}

bool gc_net_get_auto_reconnect(void)
{
    return s_auto_reconnect;
}

esp_err_t gc_net_register_event_cb(gc_net_event_cb_t cb, void *ctx)
{
    s_evt_cb = cb;
    s_evt_ctx = ctx;
    return ESP_OK;
}
