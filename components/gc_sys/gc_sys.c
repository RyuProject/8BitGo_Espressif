/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gc_sys.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "gc_sys";

static gc_sys_info_t s_info;
static bool          s_probed;
static char          s_summary[192];

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_perf_lock;   /*!< 最高主频锁（模拟器运行期间持有） */
#endif
static bool          s_perf_held;

/* ---------------- 型号映射 ---------------- */

static gc_chip_model_t map_model(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32:   return GC_CHIP_ESP32;
    case CHIP_ESP32S2: return GC_CHIP_ESP32S2;
    case CHIP_ESP32S3: return GC_CHIP_ESP32S3;
    case CHIP_ESP32C3: return GC_CHIP_ESP32C3;
    case CHIP_ESP32C6: return GC_CHIP_ESP32C6;
    case CHIP_ESP32P4: return GC_CHIP_ESP32P4;
    default:           return GC_CHIP_UNKNOWN;
    }
}

static const char *model_name(gc_chip_model_t m)
{
    switch (m) {
    case GC_CHIP_ESP32:   return "ESP32";
    case GC_CHIP_ESP32S2: return "ESP32-S2";
    case GC_CHIP_ESP32S3: return "ESP32-S3";
    case GC_CHIP_ESP32C3: return "ESP32-C3";
    case GC_CHIP_ESP32C6: return "ESP32-C6";
    case GC_CHIP_ESP32P4: return "ESP32-P4";
    default:              return "unknown";
    }
}

/** 各芯片的额定最高主频（MHz） */
static int model_max_mhz(gc_chip_model_t m)
{
    switch (m) {
    case GC_CHIP_ESP32:   return 240;
    case GC_CHIP_ESP32S2: return 240;
    case GC_CHIP_ESP32S3: return 240;
    case GC_CHIP_ESP32C3: return 160;
    case GC_CHIP_ESP32C6: return 160;
    case GC_CHIP_ESP32P4: return 400;
    default:              return 160;
    }
}

/* ---------------- 档位计算 ---------------- */

static gc_perf_tier_t compute_tier(const gc_sys_info_t *i)
{
    const size_t MB = 1024u * 1024u;

    /* HIGH：P4 这类高主频双核 + 大 PSRAM，才谈得上 16-bit 机种流畅 */
    if (i->model == GC_CHIP_ESP32P4 ||
        (i->cores >= 2 && i->cpu_mhz >= 240 && i->psram_bytes >= 4 * MB)) {
        return GC_PERF_HIGH;
    }
    /* MID：双核 + PSRAM，8-bit 舒适、GBA 可尝试 */
    if (i->cores >= 2 && i->psram_bytes >= 2 * MB) {
        return GC_PERF_MID;
    }
    /* LOW：单核 / 无 PSRAM / 低主频 */
    return GC_PERF_LOW;
}

/* ---------------- 探测 ---------------- */

esp_err_t gc_sys_probe(void)
{
    if (s_probed) {
        return ESP_OK;
    }

    esp_chip_info_t ci;
    esp_chip_info(&ci);

    s_info.model       = map_model(ci.model);
    s_info.model_name  = model_name(s_info.model);
    s_info.cores       = ci.cores;
    s_info.cpu_mhz     = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    s_info.cpu_max_mhz = model_max_mhz(s_info.model);
    s_info.psram_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    s_info.has_wifi    = (ci.features & CHIP_FEATURE_WIFI_BGN) != 0;
    s_info.has_ble     = (ci.features & CHIP_FEATURE_BLE) != 0;
    s_info.tier        = compute_tier(&s_info);

    s_probed = true;

    snprintf(s_summary, sizeof(s_summary),
             "%s / %d核 / %dMHz / PSRAM %uMB / 档位 %s",
             s_info.model_name, s_info.cores, s_info.cpu_mhz,
             (unsigned)(s_info.psram_bytes / (1024u * 1024u)),
             gc_sys_tier_name(s_info.tier));
    ESP_LOGI(TAG, "chip: %s (rev %u, features 0x%lx, wifi=%d ble=%d)",
             s_summary, (unsigned)ci.revision, (unsigned long)ci.features,
             s_info.has_wifi, s_info.has_ble);
    return ESP_OK;
}

const gc_sys_info_t *gc_sys_info(void)
{
    gc_sys_probe();
    return &s_info;
}

const char *gc_sys_tier_name(gc_perf_tier_t tier)
{
    switch (tier) {
    case GC_PERF_HIGH: return "HIGH";
    case GC_PERF_MID:  return "MID";
    default:           return "LOW";
    }
}

const char *gc_sys_summary(void)
{
    gc_sys_probe();
    return s_summary;
}

/* ---------------- 预热调优 ---------------- */

esp_err_t gc_sys_tune_for_emulation(void)
{
    gc_sys_probe();

#if CONFIG_PM_ENABLE
    /* ⚠️ 实测：只要 esp_pm_configure() 成功生效（哪怕 min==max、不做 DFS），
       PM 接管 CPU/APB 时钟后，esp-hosted 的 SDIO(Wi-Fi) 链路就会在
       `eh_wifi: set_config` 处触发 Core1 Load access fault panic → 反复重启。
       因此本工程默认 **关闭 CONFIG_PM_ENABLE**，CPU 由
       CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 固定（默认 360MHz，本就无降频）。

       下面这段仅在有人重新打开 PM 时才会编译，保留以备将来解决时钟耦合后启用。
       若启用：P4 需在 menuconfig 选 400MHz 才能配到 400，否则 400 会被判非法。 */
    int pm_max_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    if (pm_max_mhz > s_info.cpu_max_mhz) {
        pm_max_mhz = s_info.cpu_max_mhz;
    }
    esp_pm_config_t pm = {
        .max_freq_mhz = pm_max_mhz,
        .min_freq_mhz = pm_max_mhz,   /* 关 DFS：不做任何频率切换 */
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "PM: locked %dMHz (DFS off, light sleep off)", pm_max_mhz);
    } else {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
    }
#else
    /* 未开启 PM：主频由 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 固定，只能在配置层提升 */
    if (s_info.cpu_mhz < s_info.cpu_max_mhz) {
        ESP_LOGW(TAG, "CPU %dMHz < 上限 %dMHz：可把 CPU 主频调到 %dMHz 以提升帧率",
                 s_info.cpu_mhz, s_info.cpu_max_mhz, s_info.cpu_max_mhz);
    }
#endif

#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "perf: flash %s, psram %dMHz, cpu %dMHz, compiler -O2",
             CONFIG_ESPTOOLPY_FLASHFREQ, CONFIG_SPIRAM_SPEED, s_info.cpu_mhz);
#else
    ESP_LOGW(TAG, "perf: flash %s, cpu %dMHz, 无 PSRAM（封面缓存与部分机种会受限）",
             CONFIG_ESPTOOLPY_FLASHFREQ, s_info.cpu_mhz);
#endif

    return ESP_OK;
}

/* ---------------- 运行时最高主频锁（模拟器运行期间） ---------------- */

#if CONFIG_PM_ENABLE

esp_err_t gc_sys_perf_lock_acquire(void)
{
    if (!s_perf_lock) {
        esp_err_t err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "gc_emu", &s_perf_lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PM lock create failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (s_perf_held) {
        return ESP_OK; /* 已持有，避免重复 acquire 造成计数不平衡 */
    }
    esp_err_t err = esp_pm_lock_acquire(s_perf_lock);
    if (err == ESP_OK) {
        s_perf_held = true;
        ESP_LOGI(TAG, "PM lock: CPU pinned to max (%dMHz), DFS suspended",
                 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    } else {
        ESP_LOGW(TAG, "PM lock acquire failed: %s", esp_err_to_name(err));
    }
    return err;
}

void gc_sys_perf_lock_release(void)
{
    if (s_perf_lock && s_perf_held) {
        esp_pm_lock_release(s_perf_lock);
        s_perf_held = false;
        ESP_LOGI(TAG, "PM lock released: DFS restored");
    }
}

bool gc_sys_perf_lock_held(void)
{
    return s_perf_held;
}

#else /* !CONFIG_PM_ENABLE：无 PM，主频固定，锁为空操作 */

esp_err_t gc_sys_perf_lock_acquire(void)
{
    s_perf_held = true; /* 记录语义上的「满速运行中」，便于上层查询 */
    return ESP_ERR_NOT_SUPPORTED;
}

void gc_sys_perf_lock_release(void)
{
    s_perf_held = false;
}

bool gc_sys_perf_lock_held(void)
{
    return s_perf_held;
}

#endif /* CONFIG_PM_ENABLE */
