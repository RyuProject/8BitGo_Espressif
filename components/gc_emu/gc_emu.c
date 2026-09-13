/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gc_emu.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "gc_prefs.h"
#include "gc_sys.h"

static const char *TAG = "gc_emu";

/* 各机种默认实现（在 gc_emu_nes.c 等文件中定义） */
extern const gc_emu_ops_t gc_emu_ops_nes;

#define GC_EMU_REG_MAX 8

typedef struct {
    const char        *name;
    const gc_emu_ops_t *ops;
} reg_entry_t;

static reg_entry_t s_reg[GC_EMU_REG_MAX];
static size_t      s_reg_count;
static bool        s_inited;

static gc_emu_core_info_t s_info[GC_EMU_REG_MAX];

/* ---------------- 启用状态（持久化） ---------------- */

static void pref_key(gc_platform_t p, char *key, size_t len)
{
    snprintf(key, len, "core.%s", gc_api_platform_name(p));
}

static bool load_enabled(gc_platform_t p)
{
    char key[16];
    pref_key(p, key, sizeof(key));
    return gc_prefs_get_u8(key, 1) != 0; /* 默认启用 */
}

/* ---------------- 芯片性能门槛 ---------------- */

/** 各机种「流畅运行」所需的最低性能档位（档位定义见 gc_sys.h） */
static gc_perf_tier_t platform_min_tier(gc_platform_t platform)
{
    switch (platform) {
    case GC_PLATFORM_GBA:  /* 240x160 16-bit CPU：需要双核 + PSRAM */
        return GC_PERF_MID;
    case GC_PLATFORM_SNES: /* 256x224 + 多通道音频，较重 */
    case GC_PLATFORM_MD:
        return GC_PERF_HIGH;
    case GC_PLATFORM_NES:  /* 8-bit 轻量机种：单核也放行 */
    case GC_PLATFORM_GB:
    case GC_PLATFORM_GBC:
    default:
        return GC_PERF_LOW;
    }
}

bool gc_emu_platform_capable(gc_platform_t platform)
{
    const gc_sys_info_t *si = gc_sys_info();
    return si->tier >= platform_min_tier(platform);
}

esp_err_t gc_emu_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_inited = true;
    gc_prefs_init();
    /* 内置默认机种在此登记；其余由 gc_emu_register_ops() 动态加入。
       只有当前芯片「跑得动」的机种才会进注册表（硬件门槛，见 gc_sys）。 */
    if (gc_emu_platform_capable(GC_PLATFORM_NES)) {
        s_reg[s_reg_count].name = gc_api_platform_name(GC_PLATFORM_NES);
        s_reg[s_reg_count].ops = &gc_emu_ops_nes;
        s_reg_count++;
    } else {
        ESP_LOGW(TAG, "NES 超出本机档位(%s)，不注册", gc_sys_tier_name(gc_sys_info()->tier));
    }
    return ESP_OK;
}

esp_err_t gc_emu_register_ops_named(const char *name, const gc_emu_ops_t *ops)
{
    if (!ops || !ops->open || !ops->load_rom || !ops->render_frame) {
        ESP_LOGE(TAG, "register: ops 缺少必要回调 (open/load_rom/render_frame)");
        return ESP_ERR_INVALID_ARG;
    }
    gc_emu_init();
    if (s_reg_count >= GC_EMU_REG_MAX) {
        ESP_LOGE(TAG, "register: 注册表已满(%d)", GC_EMU_REG_MAX);
        return ESP_ERR_NO_MEM;
    }
    const char *core_name = (name && name[0]) ? name : gc_api_platform_name(ops->platform);

    /* 硬件性能门槛：跑不动的机种直接不注册，避免用户选到注定卡顿的核心 */
    if (!gc_emu_platform_capable(ops->platform)) {
        ESP_LOGW(TAG, "跳过核心 '%s'：%s 超出本机档位(%s)", core_name,
                 gc_api_platform_name(ops->platform), gc_sys_tier_name(gc_sys_info()->tier));
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 同 platform 覆盖，避免重复注册 */
    for (size_t i = 0; i < s_reg_count; i++) {
        if (s_reg[i].ops->platform == ops->platform) {
            ESP_LOGW(TAG, "register: 覆盖 %s 的旧实现", s_reg[i].name);
            s_reg[i].name = core_name;
            s_reg[i].ops = ops;
            return ESP_OK;
        }
    }
    s_reg[s_reg_count].name = core_name;
    s_reg[s_reg_count].ops = ops;
    s_reg_count++;
    ESP_LOGI(TAG, "registered core '%s' for %s", core_name, gc_api_platform_name(ops->platform));
    return ESP_OK;
}

esp_err_t gc_emu_register_ops(const gc_emu_ops_t *ops)
{
    return gc_emu_register_ops_named(ops ? gc_api_platform_name(ops->platform) : NULL, ops);
}

const gc_emu_ops_t *gc_emu_get_ops(gc_platform_t platform)
{
    gc_emu_init();
    for (size_t i = 0; i < s_reg_count; i++) {
        if (s_reg[i].ops->platform == platform) {
            if (!gc_emu_platform_capable(platform)) {
                return NULL; /* 硬件档位不够（注册时已拦截，这里兜底） */
            }
            if (!load_enabled(platform)) {
                ESP_LOGW(TAG, "%s core disabled by user", gc_api_platform_name(platform));
                return NULL;
            }
            return s_reg[i].ops;
        }
    }
    /* 主页每张卡片都会探测一次核心，这里用 DEBUG 级别，避免刷屏 */
    ESP_LOGD(TAG, "platform %s not supported yet", gc_api_platform_name(platform));
    return NULL;
}

const char *gc_emu_name(gc_platform_t platform)
{
    gc_emu_init();
    for (size_t i = 0; i < s_reg_count; i++) {
        if (s_reg[i].ops->platform == platform) {
            return s_reg[i].name;
        }
    }
    return NULL;
}

size_t gc_emu_list(const gc_emu_core_info_t **out)
{
    gc_emu_init();
    for (size_t i = 0; i < s_reg_count; i++) {
        s_info[i].name = s_reg[i].name;
        s_info[i].platform = s_reg[i].ops->platform;
        s_info[i].platform_name = gc_api_platform_name(s_reg[i].ops->platform);
        s_info[i].enabled = load_enabled(s_reg[i].ops->platform);
    }
    if (out) {
        *out = s_info;
    }
    return s_reg_count;
}

esp_err_t gc_emu_set_enabled(gc_platform_t platform, bool enabled)
{
    char key[16];
    pref_key(platform, key, sizeof(key));
    ESP_LOGI(TAG, "%s core %s", gc_api_platform_name(platform), enabled ? "enabled" : "disabled");
    return gc_prefs_set_u8(key, enabled ? 1 : 0);
}

bool gc_emu_is_enabled(gc_platform_t platform)
{
    return load_enabled(platform);
}

/** 汇总「本机够跑且用户已启用」的机种，预热时打印一次 */
static void log_available_platforms(void)
{
    char avail[160] = {0};
    size_t n = 0;
    for (size_t i = 0; i < s_reg_count; i++) {
        gc_platform_t p = s_reg[i].ops->platform;
        if (!gc_emu_platform_capable(p) || !load_enabled(p)) {
            continue;
        }
        if (n) {
            strlcat(avail, ",", sizeof(avail));
        }
        strlcat(avail, gc_api_platform_name(p), sizeof(avail));
        n++;
    }
    ESP_LOGI(TAG, "可开放机种(%u)：%s", (unsigned)n, n ? avail : "无");
}

esp_err_t gc_emu_warmup(void)
{
    gc_emu_init();

    /* 预热阶段先报一次硬件档位与可开放机种，现场可据此定位某机种为何不可用 */
    ESP_LOGI(TAG, "硬件：%s", gc_sys_summary());
    log_available_platforms();

    /* 按「核心名」去重（而不是 ops 指针）：GB/GBC 是两个独立 ops 但同一个核心，
       只预热一次即可，没必要重复初始化大块内存。 */
    const char *done[GC_EMU_REG_MAX];
    size_t done_n = 0;
    esp_err_t last = ESP_OK;

    for (size_t i = 0; i < s_reg_count; i++) {
        const gc_emu_ops_t *ops = s_reg[i].ops;
        if (!gc_emu_platform_capable(ops->platform) || !load_enabled(ops->platform)) {
            continue;
        }
        bool seen = false;
        for (size_t k = 0; k < done_n; k++) {
            if (done[k] && s_reg[i].name && strcmp(done[k], s_reg[i].name) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        done[done_n++] = s_reg[i].name;

        int w = 0, h = 0;
        if (gc_emu_screen_size(ops->platform, &w, &h) != ESP_OK) {
            continue;
        }
        gc_emu_info_t info = {.screen_w = w, .screen_h = h, .fps = 60};
        gc_emu_t *emu = NULL;
        if (ops->open(&emu, &info) != ESP_OK || !emu) {
            ESP_LOGW(TAG, "warmup %s: open failed", s_reg[i].name);
            last = ESP_FAIL;
            continue;
        }
        if (ops->close) {
            ops->close(emu);
        }
        ESP_LOGI(TAG, "warmup %s ok (%dx%d)", s_reg[i].name, w, h);
    }
    return last;
}

esp_err_t gc_emu_screen_size(gc_platform_t platform, int *w, int *h)
{
    switch (platform) {
    case GC_PLATFORM_NES:
        if (w) { *w = 256; }
        if (h) { *h = 240; }
        return ESP_OK;
    case GC_PLATFORM_GB:
    case GC_PLATFORM_GBC:
        if (w) { *w = 160; }
        if (h) { *h = 144; }
        return ESP_OK;
    case GC_PLATFORM_GBA:
        if (w) { *w = 240; }
        if (h) { *h = 160; }
        return ESP_OK;
    case GC_PLATFORM_SNES:
        if (w) { *w = 256; }
        if (h) { *h = 224; }
        return ESP_OK;
    case GC_PLATFORM_MD:
        if (w) { *w = 320; }
        if (h) { *h = 224; }
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

size_t gc_emu_supported_count(void)
{
    gc_emu_init();
    return s_reg_count;
}
