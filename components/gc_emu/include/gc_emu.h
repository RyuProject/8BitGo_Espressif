/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_emu.h
 * @brief 模拟器抽象层
 *
 * 上层（UI / 主循环）只依赖这一层，具体模拟器核心（gambatte / nofrendo ...）
 * 以「ops 表」的形式注册进来，新增机种不需要改动上层。
 *
 * 核心可以按机种启用/禁用，选择结果持久化到 NVS（见 gc_emu_set_enabled）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "gc_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 手柄按键位定义见 gc_api.h（gc_pad_button_t） */

typedef struct gc_emu_t gc_emu_t; /*!< 由具体核心定义的不透明句柄 */

typedef struct {
    int screen_w;  /*!< 原生画面宽，如 NES=256 */
    int screen_h;  /*!< 原生画面高，如 NES=240 */
    int fps;       /*!< 目标帧率，NES=60 */
} gc_emu_info_t;

typedef struct {
    gc_platform_t platform;

    /** 创建实例 */
    esp_err_t (*open)(gc_emu_t **emu, const gc_emu_info_t *info);
    /** 加载 ROM 文件 */
    esp_err_t (*load_rom)(gc_emu_t *emu, const char *rom_path);
    /** 复位 */
    esp_err_t (*reset)(gc_emu_t *emu);
    /** 输入：state 为 gc_pad_button_t 的按位或 */
    void      (*set_pad)(gc_emu_t *emu, uint32_t state);
    /** 渲染一帧到 RGB565 缓冲（长度 = screen_w * screen_h） */
    esp_err_t (*render_frame)(gc_emu_t *emu, uint16_t *fb, size_t fb_px);
    /** 填充一段音频采样，可为空实现 */
    esp_err_t (*audio_fill)(gc_emu_t *emu, int16_t *buf, size_t samples);
    /** 销毁 */
    void      (*close)(gc_emu_t *emu);
} gc_emu_ops_t;

/** 已注册核心的只读快照（供设置页展示） */
typedef struct {
    const char   *name;      /*!< 核心名，如 "gambatte" */
    gc_platform_t platform;
    const char   *platform_name; /*!< 机种显示名，如 "GBC" */
    bool          enabled;
} gc_emu_core_info_t;

/**
 * @brief 初始化注册表（幂等）；开机预热前调用
 */
esp_err_t gc_emu_init(void);

/**
 * @brief 取机种对应的实现；未支持或已被禁用返回 NULL
 */
const gc_emu_ops_t *gc_emu_get_ops(gc_platform_t platform);

/**
 * @brief 运行时注册一个机种实现（名字自动取机种名）
 */
esp_err_t gc_emu_register_ops(const gc_emu_ops_t *ops);

/**
 * @brief 同上，但显式指定核心名（同名核心的多个机种共用一份实现）
 */
esp_err_t gc_emu_register_ops_named(const char *name, const gc_emu_ops_t *ops);

/**
 * @brief 枚举已注册核心；返回数量，*out 指向内部快照（勿修改/释放）
 */
size_t gc_emu_list(const gc_emu_core_info_t **out);

/** 启用/禁用某机种的核心（持久化到 NVS） */
esp_err_t gc_emu_set_enabled(gc_platform_t platform, bool enabled);

bool gc_emu_is_enabled(gc_platform_t platform);

/** 核心名；未注册返回 NULL */
const char *gc_emu_name(gc_platform_t platform);

/**
 * @brief 预热：对每个已启用核心执行一次 open/close，提前把大块内存与
 *        静态初始化做好，避免用户点「开始游戏」时卡一下。
 */
esp_err_t gc_emu_warmup(void);

/**
 * @brief 示例模板机种注册（见 gc_emu_template.c）
 */
esp_err_t gc_emu_template_register(void);

/**
 * @brief 注册 Game Boy / Game Boy Color 机种（基于 libretro/gambatte-libretro）
 */
esp_err_t gc_emu_gb_register(void);

/**
 * @brief GB/GBC 核心音频采样率（经 retro_get_system_av_info），用于初始化 I2S
 */
int gc_gb_audio_get_sample_rate(void);

/**
 * @brief 取机种的原生画面尺寸
 */
esp_err_t gc_emu_screen_size(gc_platform_t platform, int *w, int *h);

/**
 * @brief 已注册机种数量（用于 UI 提示「尚未支持」）
 */
size_t gc_emu_supported_count(void);

/**
 * @brief 当前芯片是否「跑得动」该机种（硬件性能门槛，与用户开关无关）
 *
 * 门槛由 gc_sys 探测出的性能档位（LOW/MID/HIGH）决定：LOW 只放 8-bit 机种，
 * MID 增加 GBA，HIGH 才开放 SNES/MD 等 16-bit 机种。
 * 不达标的机种在注册阶段就被跳过，gc_emu_get_ops 恒返回 NULL。
 */
bool gc_emu_platform_capable(gc_platform_t platform);

#ifdef __cplusplus
}
#endif
