/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_library.h
 * @brief 游戏库编排（从 main.c 迁移）
 *
 * 把 main.c 里「云库分页 / 封面懒加载 / SD 扫描 / 模拟器运行循环」这一整块
 * 数据源编排抽成独立组件。向上只暴露：
 *   - gc_library_init()       分配 SD ROM 缓冲
 *   - gc_library_bind_ui()    注册主页回调 + 扫描 SD + 绑定列表
 *   - gc_library_on_connected() 网络连上后拉第一页云库
 * 主页只负责显示，不再关心数据从哪来、怎么下载。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 分配 SD ROM 缓冲（PSRAM），零初始化云库列表 */
esp_err_t gc_library_init(void);

/**
 * @brief 绑定主页 UI：注册数据源/加载更多/点击播放回调，扫描 SD 卡并绑定列表
 */
void gc_library_bind_ui(void);

/**
 * @brief 网络连上后的后续动作：刷新状态栏 + 若云库为空则拉第一页
 *        同时作为 gc_provision 连接成功回调。
 */
void gc_library_on_connected(void *ctx);

#ifdef __cplusplus
}
#endif
