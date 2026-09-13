/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gc_api.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "cJSON.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "gc_api";

#define GC_PART_SUFFIX ".part"

/* 开放平台 OAuth2（client_credentials）缓存 */
#define GC_OPEN_TOKEN_LEN      1024
#define GC_API_LIST_PAGE_SIZE  24
/* 列表响应上限，防止异常响应把内存吃光 */
#define GC_HTTP_RESP_MAX       (512 * 1024)

static char     g_open_token[GC_OPEN_TOKEN_LEN];
static int64_t  g_open_token_expire_ms;   /* 0 = 无有效 token */
/* token 端点不可用（比如返回 501 / 鉴权失败）时置位：本次开机不再尝试开放平台，
   避免每次翻页都白等一次 2~3 秒的失败请求（列表会自动退回公开接口）。 */
static bool     g_open_unavailable;

static int64_t gc_api_now_ms(void)
{
    return (int64_t)esp_timer_get_time() / 1000;
}

/* =========================================================================
 * 机种工具
 * ========================================================================= */

gc_platform_t gc_api_platform_from_str(const char *str)
{
    if (!str || !str[0]) {
        return GC_PLATFORM_UNKNOWN;
    }
    if (!strcasecmp(str, "nes") || !strcasecmp(str, "fc") || !strcasecmp(str, "famicom")) {
        return GC_PLATFORM_NES;
    }
    if (!strcasecmp(str, "gb") || !strcasecmp(str, "gameboy")) {
        return GC_PLATFORM_GB;
    }
    if (!strcasecmp(str, "gbc") || !strcasecmp(str, "gameboycolor")) {
        return GC_PLATFORM_GBC;
    }
    if (!strcasecmp(str, "gba")) {
        return GC_PLATFORM_GBA;
    }
    if (!strcasecmp(str, "snes") || !strcasecmp(str, "sfc")) {
        return GC_PLATFORM_SNES;
    }
    if (!strcasecmp(str, "md") || !strcasecmp(str, "genesis") || !strcasecmp(str, "megadrive")) {
        return GC_PLATFORM_MD;
    }
    return GC_PLATFORM_UNKNOWN;
}

gc_platform_t gc_api_platform_from_ext(const char *ext)
{
    if (!ext || !ext[0]) {
        return GC_PLATFORM_UNKNOWN;
    }
    if (ext[0] == '.') {
        ext++;
    }
    if (!strcasecmp(ext, "nes")) {
        return GC_PLATFORM_NES;
    }
    if (!strcasecmp(ext, "gb")) {
        return GC_PLATFORM_GB;
    }
    if (!strcasecmp(ext, "gbc")) {
        return GC_PLATFORM_GBC;
    }
    if (!strcasecmp(ext, "gba")) {
        return GC_PLATFORM_GBA;
    }
    if (!strcasecmp(ext, "sfc") || !strcasecmp(ext, "smc")) {
        return GC_PLATFORM_SNES;
    }
    if (!strcasecmp(ext, "md") || !strcasecmp(ext, "gen") || !strcasecmp(ext, "bin")) {
        return GC_PLATFORM_MD;
    }
    return GC_PLATFORM_UNKNOWN;
}

const char *gc_api_platform_name(gc_platform_t platform)
{
    switch (platform) {
    case GC_PLATFORM_NES:   return "NES";
    case GC_PLATFORM_GB:    return "GB";
    case GC_PLATFORM_GBC:   return "GBC";
    case GC_PLATFORM_GBA:   return "GBA";
    case GC_PLATFORM_SNES:  return "SNES";
    case GC_PLATFORM_MD:    return "MD";
    default:                return "?";
    }
}

const char *gc_api_platform_ext(gc_platform_t platform)
{
    switch (platform) {
    case GC_PLATFORM_NES:   return ".nes";
    case GC_PLATFORM_GB:
    case GC_PLATFORM_GBC:   return ".gb";
    case GC_PLATFORM_GBA:   return ".gba";
    case GC_PLATFORM_SNES:  return ".sfc";
    case GC_PLATFORM_MD:    return ".md";
    default:                return ".rom";
    }
}

/* =========================================================================
 * URL 工具
 * ========================================================================= */

void gc_api_resolve_url(const char *raw, char *out, size_t len)
{
    if (!out || !len) {
        return;
    }
    if (!raw || !raw[0]) {
        out[0] = '\0';
        return;
    }
    if (!strncasecmp(raw, "http://", 7) || !strncasecmp(raw, "https://", 8)) {
        strlcpy(out, raw, len);
        return;
    }
    snprintf(out, len, "%s/%s", CONFIG_GC_API_BASE_URL, raw[0] == '/' ? raw + 1 : raw);
}

/** 取小写扩展名（含点）；无扩展名返回 def */
static void url_ext(const char *url, char *ext, size_t len, const char *def)
{
    if (!len) {
        return;
    }
    ext[0] = '\0';
    const char *dot = NULL;
    if (url) {
        for (const char *p = url; *p; p++) {
            if (*p == '?' || *p == '&' || *p == '#') {
                break; /* 查询串不算路径 */
            }
            if (*p == '/') {
                dot = NULL; /* 扩展名必须在路径最后一段 */
            } else if (*p == '.') {
                dot = p;
            }
        }
    }
    if (!dot) {
        strlcpy(ext, def ? def : "", len);
        return;
    }
    size_t i = 0;
    for (; dot[i] && i < len - 1; i++) {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }
    ext[i] = '\0';
}

void gc_api_cover_ext(const char *cover_url, char *ext, size_t len)
{
    url_ext(cover_url, ext, len, ".png");
}

bool gc_api_cover_supported(const char *cover_url)
{
    char ext[8];
    url_ext(cover_url, ext, sizeof(ext), "");
    return !strcmp(ext, ".png") || !strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg");
}

/* =========================================================================
 * HTTP 公共部分
 * ========================================================================= */

/** 对本项目所有 8bitgo.com 主机（含 API 域与 assets.8bitgo.com 等 CDN 子域）
 *  固定同一根证书。openssl 比对确认 8bitgo.com 与 assets.8bitgo.com 均由
 *  Google Trust Services WE1 签发（同根 GTS Root R4），故固定证书可同时覆盖
 *  CDN，无需走系统 bundle（系统 bundle 不含 GTS Root R4）。
 *  非 8bitgo.com 的第三方主机（gstatic 等）仍走系统 bundle。 */
static bool host_is_8bitgo(const char *url)
{
    return url && strstr(url, "8bitgo.com") != NULL;
}

/* 8bitgo.com 证书固定：
 *   8bitgo.com --(Google Trust Services WE1)--> GTS Root R4
 * ESP-IDF 默认证书 bundle 不含 Google 较新的 GTS Root R4，故在 ca_8bitgo.pem
 * 中固定 GTS Root R4 作为受信任根锚点（证书从 8bitgo.com:443 抓取，有效期至
 * 2029-02-20，由 CMake EMBED_FILES 嵌入）。 */
extern const uint8_t ca_8bitgo_pem_start[] asm("_binary_ca_8bitgo_pem_start");

static void apply_tls(esp_http_client_config_t *cfg, const char *url)
{
    if (host_is_8bitgo(url)) {
        cfg->cert_pem = (char *)ca_8bitgo_pem_start;
        return;
    }
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    /* 封面/ROM 可能来自第三方 CDN（gstatic 等），用系统 bundle 校验 */
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
#else
    ESP_LOGW(TAG, "cert bundle disabled, skip TLS verify for %s", url);
    cfg->skip_cert_common_name_check = true;
#endif
}

static esp_err_t set_auth_header(esp_http_client_handle_t client)
{
    const char *token = CONFIG_GC_API_TOKEN;
    if (token && token[0] != '\0') {
        char buf[GC_OPEN_TOKEN_LEN + 16];   /* 容纳 "Bearer " + 完整静态 token */
        snprintf(buf, sizeof(buf), "Bearer %s", token);
        return esp_http_client_set_header(client, "Authorization", buf);
    }
    return ESP_OK;
}

/* =========================================================================
 * 列表：拉取 + 解析
 * ========================================================================= */

typedef struct {
    char  *buf;
    size_t len;
} http_resp_t;

static esp_err_t http_event_mem(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->data_len == 0) {
            break;
        }
        if (resp->len + evt->data_len > GC_HTTP_RESP_MAX) {
            ESP_LOGE(TAG, "response too large, abort");
            return ESP_FAIL;
        }
        char *tmp = realloc(resp->buf, resp->len + evt->data_len + 1);
        if (!tmp) {
            return ESP_FAIL;
        }
        resp->buf = tmp;
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ---- 开放平台 OAuth2：POST json + 取 token ---- */

static esp_err_t gc_api_post_json(const char *url, const char *body_json,
                                  char **out_buf, size_t *out_len)
{
    http_resp_t resp = {.buf = NULL, .len = 0};
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_mem,
        .user_data = &resp,
        .timeout_ms = 15000,
        /* 8bitgo.com(Cloudflare) 的响应头很肥大，且开放平台请求带 ~680 字节的
           Bearer 令牌，默认 512 字节的 TX 缓冲区会被截断导致服务器 RST。放大到 4K。 */
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    apply_tls(&cfg, url);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "http init failed");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body_json, (int)strlen(body_json));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(resp.buf);
        ESP_LOGE(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "POST %s -> HTTP %d", url, status);
        free(resp.buf);
        return ESP_FAIL;
    }
    if (!resp.buf) {
        resp.buf = calloc(1, 1);
    }
    *out_buf = resp.buf;
    if (out_len) {
        *out_len = resp.len;
    }
    return ESP_OK;
}

static esp_err_t gc_api_fetch_open_token(char *out_token, size_t out_len)
{
    char url[GC_GAME_URL_LEN];
    snprintf(url, sizeof(url), "%s%s",
             CONFIG_GC_API_OPEN_BASE_URL, CONFIG_GC_API_TOKEN_URL);

    char body[640];
    snprintf(body, sizeof(body),
             "{\"grant_type\":\"client_credentials\",\"client_id\":\"%s\","
             "\"client_secret\":\"%s\",\"scope\":\"games.read games.rom\"}",
             CONFIG_GC_API_CLIENT_ID, CONFIG_GC_API_CLIENT_SECRET);

    char *resp = NULL;
    esp_err_t err = gc_api_post_json(url, body, &resp, NULL);
    if (err != ESP_OK) {
        /* 应用未获批 games.rom 时服务端返回 400 invalid_scope：
           回退到默认 scope（games.read），ROM 下载在 grant 端点处报错 */
        ESP_LOGW(TAG, "scoped token rejected, falling back to default scope");
        snprintf(body, sizeof(body),
                 "{\"grant_type\":\"client_credentials\",\"client_id\":\"%s\","
                 "\"client_secret\":\"%s\"}",
                 CONFIG_GC_API_CLIENT_ID, CONFIG_GC_API_CLIENT_SECRET);
        err = gc_api_post_json(url, body, &resp, NULL);
        if (err != ESP_OK) {
            return err;
        }
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }
    const cJSON *t = cJSON_GetObjectItem(root, "access_token");
    const cJSON *e = cJSON_GetObjectItem(root, "expires_in");
    if (!cJSON_IsString(t) || t->valuestring[0] == '\0') {
        ESP_LOGE(TAG, "token 响应缺少 access_token");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strlcpy(out_token, t->valuestring, out_len);
    int64_t life = cJSON_IsNumber(e) ? (int64_t)e->valuedouble : 900;
    g_open_token_expire_ms = gc_api_now_ms() + life * 1000 - 60000; /* 提前 60s 刷新 */
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t gc_api_ensure_open_token(void)
{
    if (g_open_unavailable) {
        return ESP_FAIL;
    }
    if (g_open_token[0] != '\0' && gc_api_now_ms() < g_open_token_expire_ms) {
        return ESP_OK;
    }
    g_open_token[0] = '\0';
    ESP_LOGI(TAG, "open platform: fetching token via client_credentials");
    esp_err_t err = gc_api_fetch_open_token(g_open_token, sizeof(g_open_token));
    if (err != ESP_OK) {
        g_open_unavailable = true;
        ESP_LOGW(TAG, "open platform token unavailable, public list only this boot");
    }
    return err;
}

esp_err_t gc_api_get_text(const char *url, char **out_buf, size_t *out_len)
{
    ESP_RETURN_ON_FALSE(url && out_buf, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    http_resp_t resp = {.buf = NULL, .len = 0};
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_mem,
        .user_data = &resp,
        .timeout_ms = 15000,
        /* 8bitgo.com(Cloudflare) 的响应头很肥大，且开放平台请求带 ~680 字节的
           Bearer 令牌，默认 512 字节的 TX 缓冲区会被截断导致服务器 RST。放大到 4K。 */
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    apply_tls(&cfg, url);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "http init failed");

    /* 开放平台端点用 OAuth2 Bearer token；其余用静态 GC_API_TOKEN */
    if (strstr(url, CONFIG_GC_API_OPEN_BASE_URL) != NULL) {
        if (gc_api_ensure_open_token() == ESP_OK && g_open_token[0] != '\0') {
            char h[GC_OPEN_TOKEN_LEN + 16];   /* 容纳 "Bearer " + 完整 token（可能 >512） */
            snprintf(h, sizeof(h), "Bearer %s", g_open_token);
            esp_http_client_set_header(client, "Authorization", h);
        }
    } else {
        set_auth_header(client);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(resp.buf);
        ESP_LOGE(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "GET %s -> HTTP %d", url, status);
        free(resp.buf);
        return ESP_FAIL;
    }
    if (!resp.buf) {
        resp.buf = calloc(1, 1);
    }

    *out_buf = resp.buf;
    if (out_len) {
        *out_len = resp.len;
    }
    return ESP_OK;
}

static esp_err_t list_push(gc_game_list_t *list, const gc_game_item_t *item)
{
    if (list->count == list->capacity) {
        size_t cap = list->capacity ? list->capacity * 2 : 24;
        gc_game_item_t *tmp = realloc(list->items, cap * sizeof(gc_game_item_t));
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        list->items = tmp;
        list->capacity = cap;
    }
    list->items[list->count++] = *item;
    return ESP_OK;
}

/** 从 cJSON 里按候选键顺序取第一个非空字符串 */
static const char *json_str(const cJSON *obj, const char *const *keys, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const cJSON *j = cJSON_GetObjectItem(obj, keys[i]);
        if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
            return j->valuestring;
        }
    }
    return NULL;
}

/**
 * roms 是「语言 -> 相对路径」对象，按中文优先挑一个可用 ROM。
 */
static const char *pick_rom_url(const cJSON *roms)
{
    if (!cJSON_IsObject(roms)) {
        return NULL;
    }
    static const char *const pref[] = {"zh-Hans", "zh-Hant", "zh", "en", "ja"};
    const char *v = json_str(roms, pref, sizeof(pref) / sizeof(pref[0]));
    if (v) {
        return v;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, roms) {
        if (cJSON_IsString(it) && it->valuestring[0]) {
            return it->valuestring;
        }
    }
    return NULL;
}

/** descriptionI18n 是 { "zh-Hant": "..." } 形态的兜底 */
static const char *pick_i18n(const cJSON *obj)
{
    if (!cJSON_IsObject(obj)) {
        return NULL;
    }
    static const char *const pref[] = {"zh-Hans", "zh-Hant", "zh", "en"};
    const char *v = json_str(obj, pref, sizeof(pref) / sizeof(pref[0]));
    if (v) {
        return v;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, obj) {
        if (cJSON_IsString(it) && it->valuestring[0]) {
            return it->valuestring;
        }
    }
    return NULL;
}

/**
 * 解析列表 JSON。真实结构：
 * { "items": [ { slug, title, titleZh, description, platform, developer, year,
 *               genres[], cover, roms{ lang: path }, rom_size } ],
 *   "total": 305, "page": 1, "pageSize": 24, "totalPages": 13 }
 */
static esp_err_t parse_game_list_json(const char *json, gc_game_list_t *out,
                                      int *total_pages)
{
    cJSON *root = cJSON_Parse(json);
    ESP_RETURN_ON_FALSE(root, ESP_FAIL, TAG, "invalid json");

    if (total_pages) {
        const cJSON *tp = cJSON_GetObjectItem(root, "totalPages");
        if (cJSON_IsNumber(tp)) {
            *total_pages = tp->valueint;
        }
    }

    cJSON *arr = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_GetObjectItem(root, "data");
    }
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_GetObjectItem(root, "games");
    }
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_GetObjectItem(root, "list");
    }
    if (!cJSON_IsArray(arr) && cJSON_IsArray(root)) {
        arr = root;
    }
    if (!cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "json 中找不到游戏数组（尝试过 items/data/games/list/顶层数组）");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        gc_game_item_t item = {0};

        static const char *const id_keys[] = {"slug", "id", "name"};
        const char *v = json_str(it, id_keys, 3);
        const cJSON *jid = cJSON_GetObjectItem(it, "id");
        if (v) {
            strlcpy(item.id, v, sizeof(item.id));
        } else if (cJSON_IsNumber(jid)) {
            snprintf(item.id, sizeof(item.id), "%d", jid->valueint);
        }
        if (item.id[0] == '\0') {
            continue;
        }

        /* 标题优先英文（ASCII 用大字重渲染），回退中文 */
        static const char *const title_keys[] = {"title", "titleZh", "titleEn"};
        v = json_str(it, title_keys, 3);
        if (!v) {
            v = pick_i18n(cJSON_GetObjectItem(it, "titleI18n"));
        }
        strlcpy(item.title, v ? v : item.id, sizeof(item.title));

        v = json_str(it, (const char *const[]){"platform"}, 1);
        if (v) {
            strlcpy(item.platform, v, sizeof(item.platform));
        }

        /* 只保留本机可模拟的平台：NES / GB / GBC / GBA / SNES / MD。
           其它机种（街机、PS、NGP 等）ESP32-P4 无法模拟，直接跳过不入库，
           既不占云库列表，也不触发封面下载。 */
        if (gc_api_platform_from_str(item.platform) == GC_PLATFORM_UNKNOWN) {
            ESP_LOGD(TAG, "skip unsupported platform '%s' (slug=%s)", item.platform, item.id);
            continue;
        }

        static const char *const desc_keys[] = {"description", "descriptionZh", "descriptionEn"};
        v = json_str(it, desc_keys, 3);
        if (!v) {
            v = pick_i18n(cJSON_GetObjectItem(it, "descriptionI18n"));
        }
        if (v) {
            strlcpy(item.description, v, sizeof(item.description));
        }

        v = json_str(it, (const char *const[]){"developer"}, 1);
        if (v) {
            strlcpy(item.developer, v, sizeof(item.developer));
        }

        const cJSON *genres = cJSON_GetObjectItem(it, "genres");
        if (cJSON_IsArray(genres)) {
            const cJSON *g0 = cJSON_GetArrayItem(genres, 0);
            if (cJSON_IsString(g0)) {
                strlcpy(item.genre, g0->valuestring, sizeof(item.genre));
            }
        }

        const cJSON *yr = cJSON_GetObjectItem(it, "year");
        if (cJSON_IsNumber(yr)) {
            item.year = yr->valueint;
        }

        /* 封面：服务端返回相对路径（covers/xxx.webp），补全为绝对 URL */
        v = json_str(it, (const char *const[]){"cover", "cover_url"}, 2);
        gc_api_resolve_url(v, item.cover_url, sizeof(item.cover_url));

        /* ROM：roms{语言: 相对路径}，兜底 rom_url / rom */
        const char *rom = pick_rom_url(cJSON_GetObjectItem(it, "roms"));
        if (!rom) {
            rom = json_str(it, (const char *const[]){"rom_url", "rom"}, 2);
        }
        gc_api_resolve_url(rom, item.rom_url, sizeof(item.rom_url));

        const cJSON *sz = cJSON_GetObjectItem(it, "rom_size");
        if (cJSON_IsNumber(sz)) {
            item.rom_size = (size_t)sz->valuedouble;
        }

        ESP_RETURN_ON_ERROR(list_push(out, &item), TAG, "push failed");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t gc_api_fetch_game_page(gc_game_list_t *out, int page, int page_size,
                                 int *total_pages)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    if (page < 1) {
        page = 1;
    }
    if (page_size <= 0) {
        page_size = GC_API_LIST_PAGE_SIZE;
    }
    if (total_pages) {
        *total_pages = 0;
    }

    char *body = NULL;
    char url[GC_GAME_URL_LEN];

    /* 1) 优先开放平台（OAuth2 Bearer） */
    snprintf(url, sizeof(url), "%s%s?page=%d&page_size=%d",
             CONFIG_GC_API_OPEN_BASE_URL, CONFIG_GC_API_OPEN_LIST_PATH, page, page_size);
    if (gc_api_ensure_open_token() == ESP_OK &&
        gc_api_get_text(url, &body, NULL) == ESP_OK && body) {
        size_t before = out->count;
        esp_err_t perr = parse_game_list_json(body, out, total_pages);
        size_t added = out->count - before;
        free(body);
        if (perr == ESP_OK) {
            /* 解析成功即视为本页已拉取（即便全部被平台过滤也无妨），
               不再回退公开接口重复拉同一页。 */
            out->source = GC_API_SRC_OPEN;
            ESP_LOGI(TAG, "page %d: +%u games from OPEN platform (total %u)",
                     page, (unsigned)added, (unsigned)out->count);
            return ESP_OK;
        }
        /* 解析失败才回退公开接口 */
        body = NULL;
    }

    /* 2) 回退公开接口（无需鉴权） */
    snprintf(url, sizeof(url), "%s%s?page=%d&pageSize=%d",
             CONFIG_GC_API_BASE_URL, CONFIG_GC_API_LIST_PATH, page, page_size);
    ESP_LOGW(TAG, "open platform unavailable, fallback to public list: %s", url);

    if (gc_api_get_text(url, &body, NULL) != ESP_OK || !body) {
        return ESP_FAIL;
    }
    size_t before = out->count;
    esp_err_t err = parse_game_list_json(body, out, total_pages);
    size_t added = out->count - before;
    free(body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "parse public list failed");
        return err;
    }
    /* 解析成功即视为本页已拉取（即便全部被平台过滤），由上层按 totalPages 翻页 */
    out->source = GC_API_SRC_PUBLIC;
    ESP_LOGI(TAG, "page %d: +%u games from PUBLIC list (total %u)",
             page, (unsigned)added, (unsigned)out->count);
    return ESP_OK;
}

esp_err_t gc_api_fetch_game_list(gc_game_list_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    memset(out, 0, sizeof(*out));
    return gc_api_fetch_game_page(out, 1, GC_API_LIST_PAGE_SIZE, NULL);
}

void gc_api_free_game_list(gc_game_list_t *list)
{
    if (!list) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* =========================================================================
 * 下载到文件
 * ========================================================================= */

typedef struct {
    FILE        *fp;
    size_t       written;
    size_t       total;
    gc_api_progress_cb cb;
    void        *ctx;
} dl_ctx_t;

static esp_err_t http_event_file(esp_http_client_event_t *evt)
{
    dl_ctx_t *dl = (dl_ctx_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        dl->total = esp_http_client_get_content_length(evt->client);
        break;
    case HTTP_EVENT_ON_DATA:
        /* 关键：跟随 302 重定向时，中间响应（"Found. Redirecting..."）
           的 body 也会到达这里，必须按最终状态码过滤，否则会把
           重定向页写进文件（表现为下载成功但 ROM 头校验失败）。 */
        if (esp_http_client_get_status_code(evt->client) != 200) {
            break;
        }
        if (evt->data_len && dl->fp) {
            size_t w = fwrite(evt->data, 1, evt->data_len, dl->fp);
            if (w != evt->data_len) {
                ESP_LOGE(TAG, "write failed (disk full?)");
                return ESP_FAIL;
            }
            dl->written += w;
            if (dl->cb) {
                dl->cb(dl->written, dl->total, dl->ctx);
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/** 递归创建 dest_path 的父目录（用于下载封面/ROM 前确保目录存在） */
static void mkdir_parent(const char *path)
{
    if (!path || !path[0]) {
        return;
    }
    char tmp[256];
    if (strlen(path) >= sizeof(tmp)) {
        return;
    }
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (tmp[0]) {
                mkdir(tmp, 0755);
            }
            *p = '/';
        }
    }
}

esp_err_t gc_api_get_rom_grant(const char *slug, char *out_url, size_t url_len)
{
    ESP_RETURN_ON_FALSE(slug && slug[0] && out_url, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    /* 两步式 ROM 凭据兑换第一步：GET /games/{slug}/rom（Bearer + games.rom
       scope）-> { url, expires_in:300, filename }。gc_api_get_text 会自动
       为开放平台端点附带 Bearer 头。
       lang 必须显式传：不带 lang 时按「通用件」匹配，只挂了语言版 ROM 的
       游戏会返回 rom_unavailable。当前统一请求 en（列表里所有条目都有 en）。 */
    char url[GC_GAME_URL_LEN];
    snprintf(url, sizeof(url), "%s/games/%s/rom?lang=en",
             CONFIG_GC_API_OPEN_BASE_URL, slug);

    char *resp = NULL;
    esp_err_t err = gc_api_get_text(url, &resp, NULL);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        ESP_LOGE(TAG, "rom grant: bad json");
        return ESP_FAIL;
    }
    const cJSON *u = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(u) || u->valuestring[0] == '\0') {
        const cJSON *msg = cJSON_GetObjectItem(root, "error");
        ESP_LOGE(TAG, "rom grant: no url (%s)",
                 cJSON_IsString(msg) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(out_url, u->valuestring, url_len);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "rom grant ok (expires 300s)");
    return ESP_OK;
}

esp_err_t gc_api_download_file(const char *url, const char *dest_path,
                               gc_api_progress_cb cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(url && url[0] && dest_path, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    ESP_RETURN_ON_FALSE(strlen(dest_path) + sizeof(GC_PART_SUFFIX) < 256,
                        ESP_ERR_INVALID_ARG, TAG, "path too long");

    mkdir_parent(dest_path); /* 确保 /sdcard/covers 等目录存在 */

    char part[288];
    snprintf(part, sizeof(part), "%s%s", dest_path, GC_PART_SUFFIX);

    dl_ctx_t dl = {.fp = fopen(part, "wb"), .cb = cb, .ctx = ctx};
    if (!dl.fp) {
        /* 目录可能因 SD 初始化竞态等瞬时原因没建好：补建一次再试 */
        ESP_LOGW(TAG, "fopen %s failed (errno=%d %s), retrying after mkdir",
                 part, errno, strerror(errno));
        mkdir_parent(dest_path);
        dl.fp = fopen(part, "wb");
    }
    if (!dl.fp) {
        ESP_LOGE(TAG, "cannot create %s: errno=%d (%s)", part, errno, strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }

    /* CDN 经 C6 SDIO 链路偶发 TLS 握手失败（FATAL_ALERT / ALLOC_FAILED）或
       INCOMPLETE_DATA，多为瞬时抖动。重试若干次提高成功率。 */
    esp_err_t err = ESP_FAIL;
    int status = 0;
    for (int attempt = 1; attempt <= 3; attempt++) {
        esp_http_client_config_t cfg = {
            .url = url,
            .event_handler = http_event_file,
            .user_data = &dl,
            .timeout_ms = 30000,
            .buffer_size = 4096,
            .buffer_size_tx = 4096,
        };
        apply_tls(&cfg, url);

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "download %s: init failed (attempt %d)", url, attempt);
            err = ESP_ERR_NO_MEM;
            break;
        }
        set_auth_header(client);

        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK && status == 200) {
            break;
        }
        ESP_LOGW(TAG, "download %s failed (attempt %d): %s HTTP %d, retrying",
                 url, attempt, esp_err_to_name(err), status);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "download %s failed: %s (HTTP %d)", url, esp_err_to_name(err), status);
        fclose(dl.fp);
        remove(part);
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    fclose(dl.fp);

    /* 原子替换，避免半包文件被当成完整文件 */
    remove(dest_path);
    if (rename(part, dest_path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed", part, dest_path);
        remove(part);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "downloaded %u bytes -> %s", (unsigned)dl.written, dest_path);
    return ESP_OK;
}
