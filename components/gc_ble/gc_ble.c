/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gc_ble.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ==================== 未启用 NimBLE 时的空实现 ==================== */
/* （CONFIG_BT_NIMBLE_ENABLED 未打开时，下面这些符号不会被引用，避免链接 NimBLE） */

#if !CONFIG_BT_NIMBLE_ENABLED

esp_err_t gc_ble_init(void)               { return ESP_OK; }
void      gc_ble_deinit(void)             { }
bool      gc_ble_is_enabled(void)         { return false; }
bool      gc_ble_is_advertising(void)     { return false; }
void      gc_ble_status_text(char *buf, size_t len)
{
    if (buf && len) {
        strlcpy(buf, "未编译", len);
    }
}
esp_err_t gc_ble_gamepad_report(uint16_t buttons, int8_t lx, int8_t ly)
{
    (void)buttons; (void)lx; (void)ly;
    return ESP_ERR_INVALID_STATE;
}
esp_err_t gc_ble_battery_set(uint8_t percent)
{
    (void)percent;
    return ESP_ERR_INVALID_STATE;
}

#else /* CONFIG_BT_NIMBLE_ENABLED */

static const char *TAG = "gc_ble";

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/dis/ble_svc_dis.h"
#include "esp_hosted_bt_host_stack.h" /* esp-hosted v3: hosted-HCI 绑定 */

static const char *DEVICE_NAME = "8BitGo-GP";

static bool               s_enabled;
static bool               s_adv;
static uint8_t            s_own_addr_type;
static uint16_t           s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool               s_pad_sub;   /* 手柄特征值是否被订阅 notify */
static bool               s_batt_sub;  /* 电量特征值是否被订阅 notify */

static uint8_t            s_pad[4];     /* buttons(2) + lx(1) + ly(1) */
static uint8_t            s_batt_pct;

/* 自定义服务 / 特征值句柄 */
static uint16_t           s_gamepad_val_handle;
static uint16_t           s_batt_val_handle;

/* 8BitGo Gamepad 服务 UUID（随机 128-bit） */
static const ble_uuid128_t GATT_SVC_UUID =
    BLE_UUID128_INIT(0x38, 0x42, 0x69, 0x74, 0x47, 0x6f, 0x2d, 0x67,
                     0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01);
/* 特征值 UUID（16-bit，便于区分） */
static const ble_uuid16_t GAMEPAD_CHR_UUID = BLE_UUID16_INIT(0xFEE1);
static const ble_uuid16_t BATTERY_CHR_UUID = BLE_UUID16_INIT(0xFEE2);

static int  gap_event(struct ble_gap_event *event, void *arg);
static void advertise(void);

/* ==================== GATT 服务定义 ==================== */

static int gamepad_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
    if (attr_handle == s_gamepad_val_handle) {
        return os_mbuf_append(ctxt->om, s_pad, sizeof(s_pad));
    }
    if (attr_handle == s_batt_val_handle) {
        return os_mbuf_append(ctxt->om, &s_batt_pct, 1);
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &GATT_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &GAMEPAD_CHR_UUID.u,
                .access_cb = gamepad_chr_access,
                .val_handle = &s_gamepad_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &BATTERY_CHR_UUID.u,
                .access_cb = gamepad_chr_access,
                .val_handle = &s_batt_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }, /* 特征值数组哨兵 */
        },
    },
    { 0 }, /* 服务数组哨兵 */
};

/* ==================== GAP / 广播 ==================== */

static void advertise(void)
{
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;   /* 可连接 + 可发现 */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = (uint8_t)strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.appearance = 0x03C4;   /* HID Gamepad */
    fields.appearance_is_present = 1;

    if (ble_gap_adv_set_fields(&fields) != 0) {
        return;
    }
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &adv_params, gap_event, NULL);
    if (rc == 0) {
        s_adv = true;
    } else {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    }
}

static void on_sync(void)
{
    /* 确保有身份地址，并推断广播要用的地址类型 */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed: %d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason %d", reason);
    s_adv = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_pad_sub = s_batt_sub = false;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_conn_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "BLE connected (handle %d)", s_conn_handle);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected; reason %d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_pad_sub = s_batt_sub = false;
        advertise(); /* 断线后继续广播 */
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_gamepad_val_handle) {
            s_pad_sub = event->subscribe.cur_notify != 0;
        } else if (event->subscribe.attr_handle == s_batt_val_handle) {
            s_batt_sub = event->subscribe.cur_notify != 0;
        }
        break;
    default:
        break;
    }
    return 0;
}

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE host task start");
    nimble_port_run();
    nimble_port_freertos_deinit();
    s_enabled = false;
}

/* ==================== 公开 API ==================== */

esp_err_t gc_ble_init(void)
{
    if (s_enabled) {
        return ESP_OK;
    }

    /* esp-hosted v3：先把 CP（C6）控制器拉起来并把 NimBLE 绑到 HCI 字节管道上。
       必须在 WiFi/esp-hosted 传输已连接之后调用（gc_net 已经连过）。 */
    esp_hosted_bt_host_stack_cfg_t bt_cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    esp_err_t herr = esp_hosted_bt_host_stack_setup(&bt_cfg);
    if (herr != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_host_stack_setup failed: %s", esp_err_to_name(herr));
        return ESP_FAIL;
    }

    esp_err_t rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        esp_hosted_bt_host_stack_teardown();
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = NULL;     /* 不持久化绑定信息 */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    ble_svc_gap_init();
    ble_svc_dis_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    int rc2 = ble_gatts_count_cfg(gatt_svcs);
    if (rc2 != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc2);
        nimble_port_deinit();
        esp_hosted_bt_host_stack_teardown();
        return ESP_FAIL;
    }
    rc2 = ble_gatts_add_svcs(gatt_svcs);
    if (rc2 != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc2);
        nimble_port_deinit();
        esp_hosted_bt_host_stack_teardown();
        return ESP_FAIL;
    }

    nimble_port_freertos_init(ble_host_task); /* 返回 void */
    s_enabled = true;
    ESP_LOGI(TAG, "BLE initialized (NimBLE)");
    return ESP_OK;
}

void gc_ble_deinit(void)
{
    if (!s_enabled) {
        return;
    }
    if (s_adv) {
        ble_gap_adv_stop();
        s_adv = false;
    }
    nimble_port_stop();                  /* 让 host task 退出 */
    nimble_port_deinit();
    esp_hosted_bt_host_stack_teardown(); /* 解绑 HCI + 关闭 CP 控制器 */
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_pad_sub = s_batt_sub = false;
    s_enabled = false;
}

bool gc_ble_is_enabled(void)      { return s_enabled; }
bool gc_ble_is_advertising(void)  { return s_adv; }

void gc_ble_status_text(char *buf, size_t len)
{
    if (!buf || !len) {
        return;
    }
    if (!s_enabled) {
        strlcpy(buf, "未启用", len);
    } else if (s_adv) {
        strlcpy(buf, "广播中", len);
    } else {
        strlcpy(buf, "已初始化", len);
    }
}

/** 用 notify_custom 发送任意数据（NimBLE 的 ble_gatts_notify 只发 DB 里的值） */
static void notify_chr(uint16_t handle, const uint8_t *data, uint16_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, handle, om);
    }
}

esp_err_t gc_ble_gamepad_report(uint16_t buttons, int8_t lx, int8_t ly)
{
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pad[0] = (uint8_t)(buttons & 0xFF);
    s_pad[1] = (uint8_t)(buttons >> 8);
    s_pad[2] = (uint8_t)lx;
    s_pad[3] = (uint8_t)ly;
    if (s_pad_sub) {
        notify_chr(s_gamepad_val_handle, s_pad, sizeof(s_pad));
    }
    return ESP_OK;
}

esp_err_t gc_ble_battery_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_batt_pct = percent;
    if (s_batt_sub) {
        notify_chr(s_batt_val_handle, &s_batt_pct, 1);
    }
    return ESP_OK;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
