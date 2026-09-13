/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_store.h
 * @brief ROM / 封面存储层：SD 卡优先，不可用时回退 SPIFFS
 *
 * 负责：挂载、路径生成、已下载判定、按需下载、SD 卡 ROM 扫描。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "gc_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC_STORE_PATH_MAX 256
/** SD 卡最多列出多少个 ROM（防止超大卡拖慢扫描） */
#define GC_STORE_MAX_SD_ROMS 128

/** 从 SD 卡扫描出来的 ROM 条目 */
typedef struct {
    char          path[GC_STORE_PATH_MAX];
    char          title[GC_GAME_TITLE_LEN];
    char          ext[8];              /*!< 含点的小写扩展名，如 ".gb" */
    gc_platform_t platform;
    size_t        size;                /*!< 字节数 */
} gc_sd_rom_t;

/**
 * @brief 挂载存储介质。可重复调用。
 */
esp_err_t gc_store_init(void);

/**
 * @brief 当前是否使用 SD 卡
 */
bool gc_store_using_sd(void);

/**
 * @brief 当前 ROM 根目录（如 "/sdcard/roms"）
 */
const char *gc_store_root(void);

/**
 * @brief 封面缓存目录（SD 卡挂载点下的 covers/，无 SD 时在 SPIFFS）
 */
const char *gc_store_cover_dir(void);

/**
 * @brief 生成 ROM 文件绝对路径
 *
 * 规则：<root>/<platform>/<game_id><ext>
 */
esp_err_t gc_store_make_path(const char *game_id, gc_platform_t platform,
                             char *out_path, size_t len);

/**
 * @brief 生成封面缓存路径：<cover_dir>/<slug><ext>
 *
 * ext 从 cover_url 推导（png/jpg/jpeg/webp...），目录会自动创建。
 */
esp_err_t gc_store_cover_path(const char *slug, const char *cover_url,
                              char *out_path, size_t len);

bool gc_store_exists(const char *path);

esp_err_t gc_store_remove(const char *path);

/** 递归创建目录（仅支持单层，避免引入额外依赖） */
esp_err_t gc_store_mkdir(const char *path);

/**
 * @brief 确保 ROM 在本地存在：不存在则从开放平台下载
 *
 * @param game      游戏条目
 * @param out_path  输出最终路径（可为 NULL）
 * @param len       out_path 长度
 * @param cb        下载进度回调（可为 NULL）
 * @param ctx       回调上下文
 */
esp_err_t gc_store_ensure_rom(const gc_game_item_t *game, char *out_path, size_t len,
                              gc_api_progress_cb cb, void *ctx);

/**
 * @brief 遍历已下载（云库）ROM
 */
typedef bool (*gc_store_iter_cb)(const char *path, const char *name, void *ctx);

esp_err_t gc_store_list(gc_store_iter_cb cb, void *ctx);

/**
 * @brief 清空下载缓存（ROM + 封面文件），返回删除的文件数（-1 = 存储不可用）
 *
 * 不删除 SD 卡上用户自放的 ROM 目录之外的内容；ROM 目录是下载专用目录，
 * 用户自放的文件若在其中也会被删除。
 */
int gc_store_clear_cache(void);

/**
 * @brief 扫描 SD 卡上的 ROM 文件（递归，深度有限），按标题排序
 *
 * 无 SD 卡时返回 ESP_ERR_NOT_FOUND。
 *
 * @param out   结果数组
 * @param max   数组容量
 * @param count 实际数量
 */
esp_err_t gc_store_scan_sd(gc_sd_rom_t *out, size_t max, size_t *count);

/**
 * @brief SD 卡健康 / 容量信息（无 SD 卡时 mounted=false）
 */
typedef struct {
    bool    mounted;        /*!< 是否已挂载 */
    char    type[16];       /*!< 卡类型，如 "SDHC"、"SDXC"、"MMC" */
    char    name[32];       /*!< 卡产品名（CID） */
    uint64_t capacity;      /*!< 卡标称容量（字节） */
    uint64_t total;         /*!< 文件系统总空间（字节） */
    uint64_t free;          /*!< 文件系统剩余空间（字节） */
} gc_sd_info_t;

/**
 * @brief 读取 SD 卡信息（容量 / 剩余空间），无卡时 mounted=false
 */
esp_err_t gc_store_sd_info(gc_sd_info_t *info);

#ifdef __cplusplus
}
#endif
