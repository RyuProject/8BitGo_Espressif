/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_sys.h
 * @brief 设备 / 芯片能力探测与性能档位
 *
 * 预热阶段调用一次 gc_sys_probe()，得到芯片型号、核心数、主频、PSRAM 大小，
 * 并据此计算性能档位（LOW / MID / HIGH）。上层（模拟器注册表 / UI）用档位决定
 * 「哪些机种可以开放」，以及把设备调到「模拟器优先」的最佳状态。
 *
 * 档位划分（综合核数 / 主频 / PSRAM，不针对某一颗芯片写死）：
 *   LOW  —— 单核 / 低主频 / 无 PSRAM：只开放 8-bit 轻量机种（GB/GBC/NES）
 *   MID  —— 双核 + PSRAM：8-bit 舒适，可尝试 GBA
 *   HIGH —— 高主频双核 + 大 PSRAM：可开放 16-bit 机种（GBA/SNES/MD）
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 识别到的芯片型号（未识别为 UNKNOWN） */
typedef enum {
    GC_CHIP_UNKNOWN = 0,
    GC_CHIP_ESP32,
    GC_CHIP_ESP32S2,
    GC_CHIP_ESP32S3,
    GC_CHIP_ESP32C3,
    GC_CHIP_ESP32C6,
    GC_CHIP_ESP32P4,
} gc_chip_model_t;

/** 性能档位：越高可开放的机种越多 */
typedef enum {
    GC_PERF_LOW = 0, /*!< 单核 / 低主频 / 无 PSRAM */
    GC_PERF_MID,     /*!< 双核 + PSRAM */
    GC_PERF_HIGH,    /*!< 高主频双核 + 大 PSRAM */
} gc_perf_tier_t;

/** 探测结果快照 */
typedef struct {
    gc_chip_model_t model;        /*!< 芯片型号 */
    const char     *model_name;   /*!< 显示名，如 "ESP32-P4" */
    int             cores;        /*!< CPU 核数 */
    int             cpu_mhz;      /*!< 启动主频（由 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 决定） */
    int             cpu_max_mhz;  /*!< 该芯片支持的最高主频 */
    size_t          psram_bytes;  /*!< PSRAM 总量；0 表示无 PSRAM */
    bool            has_wifi;     /*!< 是否带 Wi-Fi 射频 */
    bool            has_ble;      /*!< 是否带蓝牙射频 */
    gc_perf_tier_t  tier;         /*!< 计算出的性能档位 */
} gc_sys_info_t;

/** 探测芯片能力并计算档位（幂等；预热阶段调用，重复调用直接返回缓存） */
esp_err_t gc_sys_probe(void);

/** 取探测结果（未 probe 会自动 probe） */
const gc_sys_info_t *gc_sys_info(void);

/** 档位名（"LOW" / "MID" / "HIGH"） */
const char *gc_sys_tier_name(gc_perf_tier_t tier);

/** 一行芯片 / 性能摘要，供日志与设置页展示 */
const char *gc_sys_summary(void);

/**
 * @brief 预热：把设备调到「模拟器优先」的最佳状态
 *        - 若编译开启了 PM：允许 DFS 动态调频（空闲降频省电），关闭 light sleep，
 *          并把频率上限设为芯片最高主频；
 *        - 记录 flash / PSRAM / CPU 现状，对明显非最优的配置给出提示。
 *
 * 注意：真正的「游玩期间满速」由 gc_sys_perf_lock_acquire() 在模拟器运行循环
 * 前后加/解锁来实现，避免全局常驻满频带来的发热与耗电。
 */
esp_err_t gc_sys_tune_for_emulation(void);

/**
 * @brief 取「最高主频」PM 锁：持有期间禁止 DFS 降频，CPU 被钉在最高主频。
 *
 * 在模拟器进入运行循环前调用，退出后 gc_sys_perf_lock_release()。
 * 这样「关闭动态降频」只在游玩时生效，保证模拟器全程满速，其它时间仍可省电。
 *
 * @return ESP_OK；未开启 CONFIG_PM_ENABLE 时为空操作，返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t gc_sys_perf_lock_acquire(void);

/** 释放最高主频 PM 锁，恢复 DFS 动态调频 */
void gc_sys_perf_lock_release(void);

/** 当前是否持有最高主频 PM 锁 */
bool gc_sys_perf_lock_held(void);

#ifdef __cplusplus
}
#endif
