/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_prefs.h
 * @brief 极简配置持久化（NVS）：给 UI / 模拟器保存少量用户偏好
 *
 * 只提供 u8 / str 两种类型，够用即可；未初始化时读写均安全降级（返回默认值）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 幂等；可在任意时间调用 */
esp_err_t gc_prefs_init(void);

esp_err_t gc_prefs_set_u8(const char *key, uint8_t val);

/** 不存在或未初始化时返回 def */
uint8_t gc_prefs_get_u8(const char *key, uint8_t def);

esp_err_t gc_prefs_set_str(const char *key, const char *val);

/** 不存在时写入 def 并返回 def；buf 一定以 '\0' 结尾 */
esp_err_t gc_prefs_get_str(const char *key, char *buf, size_t len, const char *def);

#ifdef __cplusplus
}
#endif
