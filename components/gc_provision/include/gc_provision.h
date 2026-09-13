/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_provision.h
 * @brief Wi-Fi 配网向导
 *
 * 把 main.c 里的配网状态机（信号量 + 重试 + Rescan + 手动输入）抽成独立组件。
 * 对外只暴露：开始 / 取消 / 是否进行中 / 连接成功回调。
 * 连接成功后通过 ready_cb 通知上层（上层负责拉云库、初始化存储等后续动作）。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建内部信号量；可重复调用 */
esp_err_t gc_provision_init(void);

/**
 * @brief 开始配网
 *
 * 有已保存凭据则后台自动回连（成功也走 ready_cb）；
 * 没有则弹出扫描 / 选择 / 输密码流程。
 */
void gc_provision_start(void);

/** 取消进行中的配网（唤醒等待中的流程并退出） */
void gc_provision_cancel(void);

/** 配网流程是否正在运行 */
bool gc_provision_is_running(void);

/** 连接成功（自动回连或手动配网）后回调，用于拉云库等后续动作 */
void gc_provision_set_ready_cb(void (*cb)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
