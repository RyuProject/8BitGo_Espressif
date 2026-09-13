/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file gc_api.h
 * @brief 8BitGo 开放平台 API 适配层
 *
 * 这一层是「骨架」里唯一假设了服务端数据格式的地方。
 * 真实接口字段已按 8bitgo.com/api/games 的实际返回对齐（slug / title / cover /
 * roms{语言} / description / platform ...），上层（UI / 存储 / 模拟器）无需感知。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 手柄按键位（与 NES 标准顺序一致，各层共用） ---------------- */
typedef enum {
    GC_PAD_A      = (1u << 0),
    GC_PAD_B      = (1u << 1),
    GC_PAD_SELECT = (1u << 2),
    GC_PAD_START  = (1u << 3),
    GC_PAD_UP     = (1u << 4),
    GC_PAD_DOWN   = (1u << 5),
    GC_PAD_LEFT   = (1u << 6),
    GC_PAD_RIGHT  = (1u << 7),
} gc_pad_button_t;

/* ---------------- 机种 ---------------- */

typedef enum {
    GC_PLATFORM_NES = 0,
    GC_PLATFORM_GB,
    GC_PLATFORM_GBC,
    GC_PLATFORM_GBA,
    GC_PLATFORM_SNES,
    GC_PLATFORM_MD,
    GC_PLATFORM_UNKNOWN,
} gc_platform_t;

/* ---------------- 数据模型 ---------------- */

#define GC_GAME_ID_LEN      64
#define GC_GAME_TITLE_LEN   96
#define GC_GAME_PLAT_LEN    16
#define GC_GAME_DESC_LEN    360
#define GC_GAME_DEV_LEN     48
#define GC_GAME_GENRE_LEN   32
#define GC_GAME_URL_LEN     512

/* 列表数据来源：开放平台 OAuth2 接口 / 公开接口回退 */
typedef enum {
    GC_API_SRC_PUBLIC = 0,  /*!< 公开 /api/games（无鉴权） */
    GC_API_SRC_OPEN,        /*!< 开放平台 /api/open/v1/games（Bearer token） */
} gc_api_source_t;

typedef struct {
    char   id[GC_GAME_ID_LEN];         /*!< slug，唯一标识 */
    char   title[GC_GAME_TITLE_LEN];   /*!< 优先英文标题，回退中文标题 */
    char   platform[GC_GAME_PLAT_LEN]; /*!< 服务端原始机种字符串，如 "nes" */
    char   description[GC_GAME_DESC_LEN]; /*!< 简介（优先中文） */
    char   developer[GC_GAME_DEV_LEN];
    char   genre[GC_GAME_GENRE_LEN];
    int    year;                       /*!< 未知为 0 */
    char   cover_url[GC_GAME_URL_LEN]; /*!< 已解析为绝对 URL */
    char   rom_url[GC_GAME_URL_LEN];   /*!< 已解析为绝对 URL；可能为空 */
    size_t rom_size;                   /*!< 字节数，未知为 0 */
} gc_game_item_t;

typedef struct {
    gc_game_item_t *items;
    size_t          count;
    size_t          capacity;
    gc_api_source_t source;            /*!< 本次列表来自哪个接口 */
} gc_game_list_t;

/**
 * @brief 下载进度回调
 * @param downloaded 已下载字节数
 * @param total      总字节数（未知为 0）
 */
typedef void (*gc_api_progress_cb)(size_t downloaded, size_t total, void *ctx);

/* ---------------- 机种工具 ---------------- */

gc_platform_t gc_api_platform_from_str(const char *str);
/** 由文件扩展名（含点或不含点均可）判断机种 */
gc_platform_t gc_api_platform_from_ext(const char *ext);
const char   *gc_api_platform_name(gc_platform_t platform);
const char   *gc_api_platform_ext(gc_platform_t platform); /*!< 返回 ".nes" 之类的扩展名 */

/* ---------------- 列表接口 ---------------- */

/**
 * @brief 拉取游戏列表第 1 页（等价于 gc_api_fetch_game_page(out, 1, 24, NULL)）
 */
esp_err_t gc_api_fetch_game_list(gc_game_list_t *out);

/**
 * @brief 拉取列表某一页并 **追加** 到 out（out 需先零初始化）
 *
 * 分页数据源优先开放平台（OAuth2 Bearer），失败回退公开接口。
 *
 * @param out          结果（重复调用会持续追加）
 * @param page         从 1 开始
 * @param page_size    每页数量，<=0 时取 24
 * @param total_pages  可选输出：服务端总页数（未知为 0）
 */
esp_err_t gc_api_fetch_game_page(gc_game_list_t *out, int page, int page_size,
                                 int *total_pages);

/**
 * @brief 释放 gc_api_fetch_game_list / gc_api_fetch_game_page 申请的内存
 */
void gc_api_free_game_list(gc_game_list_t *list);

/* ---------------- 资源工具 ---------------- */

/** 把服务端返回的相对路径补全为绝对 URL（已是 http(s) 则原样拷贝） */
void gc_api_resolve_url(const char *raw, char *out, size_t len);

/**
 * @brief 该封面能否被本地解码器渲染（LVGL 内置 lodepng/tjpgd）
 *
 * 服务端封面以 WebP 为主；WebP/AVIF 当前无法解码，调用方应回退到占位图。
 */
bool gc_api_cover_supported(const char *cover_url);

/** 取封面的扩展名（含点，小写），默认 ".png" */
void gc_api_cover_ext(const char *cover_url, char *ext, size_t len);

/* ---------------- 下载接口 ---------------- */

/**
 * @brief 把 url 下载到 dest_path（流式写文件，不占用大块 RAM）
 *
 * 会先写 "<dest_path>.part"，成功后 rename 为 dest_path，避免半包 ROM。
 */
/**
 * @brief 获取游戏 ROM 的短期下载凭据 URL（开放平台两步式兑换第一步）
 *
 * 需要 client_credentials 令牌携带 games.rom scope。返回的 url 有效期
 * 300 秒，GET 它（不带 Authorization）会 302 到真实 ROM 文件。
 *
 * @param slug    游戏 slug（gc_game_item_t.id）
 * @param out_url 输出凭据 URL
 */
esp_err_t gc_api_get_rom_grant(const char *slug, char *out_url, size_t url_len);

esp_err_t gc_api_download_file(const char *url, const char *dest_path,
                               gc_api_progress_cb cb, void *ctx);

/**
 * @brief 附带 Authorization: Bearer <token> 的 GET（返回 malloc 的字符串，需 free）
 *
 * @param[out] out_buf  响应体（'\0' 结尾），调用方负责 free
 * @param[out] out_len  响应体长度，可为 NULL
 */
esp_err_t gc_api_get_text(const char *url, char **out_buf, size_t *out_len);

#ifdef __cplusplus
}
#endif
