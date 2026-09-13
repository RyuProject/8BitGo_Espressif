/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_ui.h
 * @brief SDL3 界面层公共 API（OS 风格页面栈：启动页 / 主页 / 游玩页 / 设置）
 *
 * 与原 LVGL 版保持 API 兼容：main / gc_library / gc_provision 无需改动，
 * 由 gc_ui_sdl 组件提供实现（分阶段落地，未实现的页面为安全桩）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "gc_api.h"
#include "gc_net.h"
#include "gc_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 主页卡片上限（超过部分靠分页继续加载，不无限建控件） */
#define GC_UI_HOME_MAX_CARDS 120

/* ==================== 生命周期 ==================== */

/** 初始化 UI 与显示/触摸绑定 */
esp_err_t gc_ui_init(esp_lcd_panel_handle_t panel,
                     esp_lcd_panel_io_handle_t io,
                     esp_lcd_touch_handle_t touch,
                     int hres, int vres);

/** UI 互斥锁，跨任务操作界面状态时必须成对调用（可重入） */
bool gc_ui_lock(uint32_t timeout_ms);
void gc_ui_unlock(void);

/* ==================== 导航 ==================== */

/** 返回上一页（主页为根页，无效果） */
void gc_ui_nav_back(void);
bool gc_ui_nav_can_back(void);
/** 回到主页（清空历史） */
void gc_ui_nav_home(void);

/* ==================== 启动页 ==================== */

esp_err_t gc_ui_boot_show(void);
/** percent < 0 表示不确定态 */
esp_err_t gc_ui_boot_progress(int percent, const char *text);

/* ==================== 状态 / 提示页 ==================== */

esp_err_t gc_ui_show_message(const char *title, const char *text);
/** 下载 / 加载进度条，percent < 0 表示不确定态 */
esp_err_t gc_ui_show_progress(int percent, const char *text);

/* ==================== 主页 ==================== */

typedef enum {
    GC_UI_SRC_CLOUD = 0,  /*!< 8BitGo 云库 */
    GC_UI_SRC_SD,         /*!< SD 卡本地 ROM */
} gc_ui_source_t;

/** 列表项被点击（index 对应各自数据源的下标） */
typedef void (*gc_ui_index_cb)(size_t index, void *ctx);
/** 数据源切换 */
typedef void (*gc_ui_source_cb)(gc_ui_source_t src, void *ctx);
/** 主页右上角「设置」按钮 */
typedef void (*gc_ui_home_settings_cb)(void *ctx);
/** 云库滚动到底需要加载下一页 */
typedef void (*gc_ui_loadmore_cb)(void *ctx);

/**
 * @brief 绑定主页数据源（指针需在页面存活期间保持有效）
 *
 * @param games       云库列表（可为 NULL）
 * @param sd_roms     SD 卡 ROM 数组（可为 NULL）
 * @param sd_count    SD ROM 数量
 * @param on_play_game 点击云库游戏回调
 * @param on_play_rom  点击 SD ROM 回调
 * @param ctx         回调上下文
 */
esp_err_t gc_ui_home_bind(const gc_game_list_t *games,
                          const gc_sd_rom_t *sd_roms, size_t sd_count,
                          gc_ui_index_cb on_play_game, gc_ui_index_cb on_play_rom,
                          void *ctx);

/** 显示 / 重建主页（src 决定当前 Tab），animate=false 用于首次进入 */
esp_err_t gc_ui_home_show(gc_ui_source_t src, bool animate);

/** 从任意页面返回主页并按当前数据源刷新 */
esp_err_t gc_ui_home_refresh(void);

/**
 * @brief 只刷新主页内容，**不改变当前显示页面**
 *
 * 后台任务（分页加载 / SD 扫描 / 封面下载）完成时用这个，
 * 避免把正在看设置页的用户强行拽回主页。
 */
esp_err_t gc_ui_home_rerender(void);

/** 同上，但整页重建（列表被替换时用，generation 会 +1） */
esp_err_t gc_ui_home_reload(void);

void gc_ui_set_source_cb(gc_ui_source_cb cb, void *ctx);
void gc_ui_set_loadmore_cb(gc_ui_loadmore_cb cb, void *ctx);
void gc_ui_set_home_settings_cb(gc_ui_home_settings_cb cb, void *ctx);

/**
 * @brief 主页内容版本号：每次整页重建都会 +1
 *
 * 封面异步加载任务用它判断「回调到达时页面是否已经变了」。
 */
uint32_t gc_ui_home_generation(void);

/**
 * @brief 某张卡片的封面文件已就绪，请渲染
 * @param gen   发起加载时记录的 generation
 * @param index 云库列表下标
 * @param path  封面文件绝对路径（仅 png/jpg/jpeg 会被渲染）
 */
esp_err_t gc_ui_home_cover_ready(uint32_t gen, size_t index, const char *path);

/** 底部「加载更多」状态（true 显示 spinner） */
esp_err_t gc_ui_home_set_loading(bool loading);

/* ==================== 游玩页（模拟器 + 简介） ==================== */

/**
 * @brief 进入游玩页
 *
 * @param w,h     模拟器原生分辨率
 * @param fb      RGB565 帧缓冲（由模拟器任务写，UI 只读；不拷贝，零额外内存）
 * @param title   游戏标题
 * @param platform 机种字符串（如 "gb"）
 * @param meta    右上角副标题（年份 / 开发商，可为 NULL）
 * @param desc    游戏简介（可为 NULL）
 */
esp_err_t gc_ui_play_enter(int w, int h, const uint16_t *fb,
                           const char *title, const char *platform,
                           const char *meta, const char *desc);

/** 一帧写好后调用：让画布重绘（不拷贝像素） */
esp_err_t gc_ui_play_blit(void);

/** 离开游玩页（回主页） */
esp_err_t gc_ui_play_exit(void);

/** 当前虚拟手柄状态（gc_pad_button_t 按位或） */
uint32_t gc_ui_get_pad(void);

/** 用户是否请求退出游戏 */
bool gc_ui_exit_requested(void);
void gc_ui_clear_exit_request(void);

/* ==================== 状态栏 ==================== */

/** 更新所有页面的网络状态栏文本 */
esp_err_t gc_ui_update_net_status(const char *text);

/* ==================== Wi-Fi 配网页 ==================== */

typedef void (*gc_ui_ap_selected_cb)(size_t index, void *ctx);
/** 密码输入完成；password 为 NULL 表示用户取消 */
typedef void (*gc_ui_password_cb)(const char *ssid, const char *password, void *ctx);
typedef void (*gc_ui_rescan_cb)(void *ctx);
/** 用户在配网页点了返回：调用方应结束配网流程 */
typedef void (*gc_ui_cancel_cb)(void *ctx);

void gc_ui_set_wifi_rescan_cb(gc_ui_rescan_cb cb, void *ctx);
void gc_ui_set_wifi_manual_cb(gc_ui_rescan_cb cb, void *ctx);
void gc_ui_set_wifi_cancel_cb(gc_ui_cancel_cb cb, void *ctx);

esp_err_t gc_ui_show_wifi_scan(const gc_net_scan_result_t *result,
                               gc_ui_ap_selected_cb cb, void *ctx);
esp_err_t gc_ui_show_wifi_scanning(void);
esp_err_t gc_ui_show_wifi_password(const char *ssid, bool ssid_editable,
                                   gc_ui_password_cb cb, void *ctx);
esp_err_t gc_ui_show_wifi_connecting(const char *ssid);
esp_err_t gc_ui_show_wifi_result(bool ok, const char *msg);

/* ==================== 设置页 ==================== */

typedef enum {
    GC_UI_SET_WIFI = 0,   /*!< 由调用方接管（需要配网流程） */
    GC_UI_SET_BT,         /*!< 下面这些 gc_ui 内部直接跳转 */
    GC_UI_SET_CORES,
    GC_UI_SET_GITHUB,
    GC_UI_SET_ABOUT,
} gc_ui_settings_action_t;

typedef void (*gc_ui_settings_cb)(gc_ui_settings_action_t action, void *ctx);

/**
 * @brief 设置页。
 *
 * 只有 GC_UI_SET_WIFI 会回调给调用方；其余条目由 gc_ui 自己跳转对应子页。
 */
esp_err_t gc_ui_show_settings(gc_ui_settings_cb cb, void *ctx);

/** 子页（也可外部直接调用） */
esp_err_t gc_ui_show_cores(void);
esp_err_t gc_ui_show_bluetooth(void);
esp_err_t gc_ui_show_github(void);
esp_err_t gc_ui_show_about(void);

/* ==================== 屏幕旋转 ==================== */

/**
 * @brief 设置屏幕旋转角度（0/1/2/3 = 0°/90°/180°/270° 顺时针，超出自动取模）
 */
esp_err_t gc_ui_set_rotation(uint8_t rot);

/** 当前旋转角度（0/1/2/3） */
uint8_t gc_ui_get_rotation(void);

/* ==================== SD 卡 / 硬件诊断页 ==================== */

/** SD 卡状态页（挂载 / 容量 / 剩余空间 / ROM 目录） */
esp_err_t gc_ui_show_storage(void);

/** 硬件诊断页（RTC 时间 / 电池电压） */
esp_err_t gc_ui_show_diagnostics(void);

#ifdef __cplusplus
}
#endif
