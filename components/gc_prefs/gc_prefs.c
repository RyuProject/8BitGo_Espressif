/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gc_prefs.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "gc_prefs";

#define GC_PREFS_NS "gc_cfg"

static bool s_inited;

esp_err_t gc_prefs_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t gc_prefs_set_u8(const char *key, uint8_t val)
{
    if (!key || gc_prefs_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(GC_PREFS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t gc_prefs_get_u8(const char *key, uint8_t def)
{
    if (!key || gc_prefs_init() != ESP_OK) {
        return def;
    }
    nvs_handle_t h;
    if (nvs_open(GC_PREFS_NS, NVS_READONLY, &h) != ESP_OK) {
        return def;
    }
    uint8_t val = def;
    if (nvs_get_u8(h, key, &val) != ESP_OK) {
        val = def;
    }
    nvs_close(h);
    return val;
}

esp_err_t gc_prefs_set_str(const char *key, const char *val)
{
    if (!key || gc_prefs_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(GC_PREFS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, val ? val : "");
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void copy_default(char *buf, size_t len, const char *def)
{
    const char *d = def ? def : "";
    size_t n = strlen(d);
    if (n > len - 1) {
        n = len - 1;
    }
    memcpy(buf, d, n);
    buf[n] = '\0';
}

esp_err_t gc_prefs_get_str(const char *key, char *buf, size_t len, const char *def)
{
    if (!key || !buf || !len) {
        return ESP_ERR_INVALID_ARG;
    }
    copy_default(buf, len, def);
    if (gc_prefs_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    if (nvs_open(GC_PREFS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t n = len;
    esp_err_t err = nvs_get_str(h, key, buf, &n);
    nvs_close(h);
    if (err != ESP_OK) {
        copy_default(buf, len, def);
    }
    return err;
}
