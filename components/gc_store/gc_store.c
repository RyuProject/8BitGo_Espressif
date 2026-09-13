/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gc_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include "bsp/esp32_p4_function_ev_board.h"

static const char *TAG = "gc_store";

static bool s_inited;
static bool s_sd_ok;
static char s_root[64];
static char s_cover_dir[64];

static esp_err_t mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 8,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t gc_store_mkdir(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    /* 递归创建父目录，避免「父目录未就绪」导致本目录创建失败 */
    char tmp[256];
    if (strlen(path) >= sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (tmp[0]) {
                mkdir(tmp, 0777); /* 已存在则 EEXIST，忽略 */
            }
            *p = '/';
        }
    }
    int rc = mkdir(path, 0777);
    if (rc != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir(%s) failed: errno=%d (%s)", path, errno, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t gc_store_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* 1) 优先 SD 卡 */
    if (bsp_sdcard_mount() == ESP_OK) {
        s_sd_ok = true;
        strlcpy(s_root, CONFIG_GC_ROM_DIR, sizeof(s_root));
        snprintf(s_cover_dir, sizeof(s_cover_dir), "%s/covers", CONFIG_BSP_SD_MOUNT_POINT);
        ESP_LOGI(TAG, "storage: SD card -> %s", s_root);
    } else {
        ESP_LOGW(TAG, "SD card unavailable, fallback to SPIFFS");
        ESP_RETURN_ON_ERROR(mount_spiffs(), TAG, "no usable storage");
        s_sd_ok = false;
        strlcpy(s_root, "/spiffs/roms", sizeof(s_root));
        strlcpy(s_cover_dir, "/spiffs/covers", sizeof(s_cover_dir));
        ESP_LOGI(TAG, "storage: SPIFFS -> %s", s_root);
    }

    gc_store_mkdir(s_root);
    if (s_sd_ok) {
        gc_store_mkdir(s_cover_dir); /* 只有 SD 卡上才用封面缓存目录 */
    }
    s_inited = true;
    return ESP_OK;
}

bool gc_store_using_sd(void)
{
    return s_sd_ok;
}

const char *gc_store_root(void)
{
    return s_inited ? s_root : CONFIG_GC_ROM_DIR;
}

const char *gc_store_cover_dir(void)
{
    return s_inited ? s_cover_dir : CONFIG_BSP_SD_MOUNT_POINT "/covers";
}

esp_err_t gc_store_make_path(const char *game_id, gc_platform_t platform,
                             char *out_path, size_t len)
{
    ESP_RETURN_ON_FALSE(game_id && out_path, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "store not initialized");

    int n;
    if (s_sd_ok) {
        n = snprintf(out_path, len, "%s/%s/%s%s",
                     s_root, gc_api_platform_name(platform), game_id,
                     gc_api_platform_ext(platform));
    } else {
        /* SPIFFS 是扁平命名空间，且对象名上限只有 32 字节
           （CONFIG_SPIFFS_OBJ_NAME_LEN）：带上 "roms/<PLATFORM>/" 前缀后
           很容易超限导致 fopen 失败，所以回退存储时不再分机种子目录。 */
        n = snprintf(out_path, len, "%s/%s%s",
                     s_root, game_id, gc_api_platform_ext(platform));
    }
    ESP_RETURN_ON_FALSE(n > 0 && n < (int)len, ESP_ERR_INVALID_SIZE, TAG, "path too long");
    return ESP_OK;
}

esp_err_t gc_store_cover_path(const char *slug, const char *cover_url,
                              char *out_path, size_t len)
{
    ESP_RETURN_ON_FALSE(slug && slug[0] && out_path, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    ESP_RETURN_ON_ERROR(gc_store_init(), TAG, "store init failed");

    /* 封面是「纯装饰 + 可缓存」的资源，只有在 SD 卡上才落盘。
       回退到 SPIFFS 时：分区只有 7MB、对象名上限 32 字节，
       存封面既容易失败又会挤占 ROM 空间，直接放弃缓存走内置占位图。
       这是预期分支（每张卡片都会问一次），静默返回即可，
       UI 侧会在主页提示「无 SD 卡，封面已关闭」。 */
    if (!s_sd_ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char ext[8];
    gc_api_cover_ext(cover_url, ext, sizeof(ext));

    int n = snprintf(out_path, len, "%s/%s%s", s_cover_dir, slug, ext);
    ESP_RETURN_ON_FALSE(n > 0 && n < (int)len, ESP_ERR_INVALID_SIZE, TAG, "path too long");
    gc_store_mkdir(s_cover_dir);
    return ESP_OK;
}

bool gc_store_exists(const char *path)
{
    if (!path) {
        return false;
    }
    struct stat st;
    return (stat(path, &st) == 0) && (st.st_size > 0);
}

esp_err_t gc_store_remove(const char *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    return (remove(path) == 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t gc_store_ensure_rom(const gc_game_item_t *game, char *out_path, size_t len,
                              gc_api_progress_cb cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(game, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    ESP_RETURN_ON_ERROR(gc_store_init(), TAG, "store init failed");

    gc_platform_t plat = gc_api_platform_from_str(game->platform);
    if (plat == GC_PLATFORM_UNKNOWN) {
        ESP_LOGW(TAG, "unsupported platform '%s' (id=%s)", game->platform, game->id);
    }

    char path[GC_STORE_PATH_MAX];
    ESP_RETURN_ON_ERROR(gc_store_make_path(game->id, plat, path, sizeof(path)), TAG, "path failed");

    /* 保证子目录存在（仅 SD 卡；SPIFFS 走扁平路径，见 gc_store_make_path） */
    if (s_sd_ok) {
        char dir[GC_STORE_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", s_root, gc_api_platform_name(plat));
        gc_store_mkdir(dir);
    }

    if (gc_store_exists(path)) {
        ESP_LOGI(TAG, "ROM cached: %s", path);
        if (out_path) {
            strlcpy(out_path, path, len);
        }
        return ESP_OK;
    }

    /* 列表接口按设计不含 ROM 地址：为空时走两步式凭据兑换
       （games/{slug}/rom -> 302 真实文件） */
    char grant_url[GC_GAME_URL_LEN];
    const char *url = game->rom_url;
    if (!url[0]) {
        ESP_LOGI(TAG, "game %s: no rom_url in list, requesting grant", game->id);
        ESP_RETURN_ON_ERROR(gc_api_get_rom_grant(game->id, grant_url, sizeof(grant_url)),
                            TAG, "rom grant failed");
        url = grant_url;
    }
    ESP_LOGI(TAG, "downloading ROM: %s", url);
    ESP_RETURN_ON_ERROR(gc_api_download_file(url, path, cb, ctx), TAG, "download failed");

    if (out_path) {
        strlcpy(out_path, path, len);
    }
    return ESP_OK;
}

int gc_store_clear_cache(void)
{
    if (gc_store_init() != ESP_OK) {
        return -1;
    }

    static const char *const dirs[] = {"NES", "GB", "GBC", "GBA", "SNES", "MD", "?"};
    int removed = 0;

    /* 下载的 ROM（按平台目录） */
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        char dir[GC_STORE_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", s_root, dirs[d]);
        DIR *dp = opendir(dir);
        if (!dp) {
            continue;
        }
        struct dirent *ent;
        while ((ent = readdir(dp)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            char path[GC_STORE_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            if (remove(path) == 0) {
                removed++;
            }
        }
        closedir(dp);
    }

    /* 下载的封面缓存 */
    DIR *dp = opendir(s_cover_dir);
    if (dp) {
        struct dirent *ent;
        while ((ent = readdir(dp)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            char path[GC_STORE_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", s_cover_dir, ent->d_name);
            if (remove(path) == 0) {
                removed++;
            }
        }
        closedir(dp);
    }
    return removed;
}

esp_err_t gc_store_list(gc_store_iter_cb cb, void *ctx)
{
    ESP_RETURN_ON_ERROR(gc_store_init(), TAG, "store init failed");
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    static const char *const dirs[] = {"NES", "GB", "GBC", "GBA", "SNES", "MD", "?"};
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        char dir[GC_STORE_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", s_root, dirs[d]);
        DIR *dp = opendir(dir);
        if (!dp) {
            continue;
        }
        struct dirent *ent;
        while ((ent = readdir(dp)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            char path[GC_STORE_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            if (!cb(path, ent->d_name, ctx)) {
                closedir(dp);
                return ESP_OK;
            }
        }
        closedir(dp);
    }
    return ESP_OK;
}

/* ==================== SD 卡 ROM 扫描 ==================== */

typedef struct {
    gc_sd_rom_t *out;
    size_t       max;
    size_t       count;
} sd_scan_ctx_t;

static void title_from_filename(const char *name, char *out, size_t len)
{
    strlcpy(out, name, len);
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

static void scan_dir(const char *dir, int depth, sd_scan_ctx_t *sc)
{
    if (depth > 4 || sc->count >= sc->max) {
        return;
    }
    DIR *dp = opendir(dir);
    if (!dp) {
        return;
    }

    char full[GC_STORE_PATH_MAX];
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (sc->count >= sc->max) {
            break;
        }
        const char *n = ent->d_name;
        if (n[0] == '.') {
            continue;
        }
        snprintf(full, sizeof(full), "%s/%s", dir, n);

        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!strcasecmp(n, "System Volume Information") || !strcasecmp(n, "covers")) {
                continue;
            }
            scan_dir(full, depth + 1, sc);
            continue;
        }
        if (st.st_size <= 0) {
            continue;
        }

        const char *dot = strrchr(n, '.');
        gc_platform_t plat = dot ? gc_api_platform_from_ext(dot) : GC_PLATFORM_UNKNOWN;
        if (!dot || plat == GC_PLATFORM_UNKNOWN) {
            continue; /* 只收录能识别扩展名的文件 */
        }

        gc_sd_rom_t *r = &sc->out[sc->count++];
        strlcpy(r->path, full, sizeof(r->path));
        title_from_filename(n, r->title, sizeof(r->title));
        strlcpy(r->ext, dot, sizeof(r->ext));
        for (char *p = r->ext; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        r->platform = plat;
        r->size = (size_t)st.st_size;
    }
    closedir(dp);
}

static int sd_rom_cmp(const void *a, const void *b)
{
    return strcasecmp(((const gc_sd_rom_t *)a)->title, ((const gc_sd_rom_t *)b)->title);
}

esp_err_t gc_store_scan_sd(gc_sd_rom_t *out, size_t max, size_t *count)
{
    ESP_RETURN_ON_FALSE(out && count, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    *count = 0;
    ESP_RETURN_ON_ERROR(gc_store_init(), TAG, "store init failed");
    if (!s_sd_ok) {
        return ESP_ERR_NOT_FOUND;
    }

    sd_scan_ctx_t sc = {.out = out, .max = max, .count = 0};
    scan_dir(CONFIG_BSP_SD_MOUNT_POINT, 0, &sc);

    if (sc.count > 1) {
        qsort(out, sc.count, sizeof(gc_sd_rom_t), sd_rom_cmp);
    }
    *count = sc.count;
    ESP_LOGI(TAG, "SD scan: %u ROMs", (unsigned)sc.count);
    return ESP_OK;
}

esp_err_t gc_store_sd_info(gc_sd_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    if (!s_sd_ok || !bsp_sdcard) {
        info->mounted = false;
        return ESP_OK;
    }
    info->mounted = true;
    const sdmmc_card_t *c = bsp_sdcard;

    /* 产品名（CID.name，可能不满 8 字节，确保结尾 '\0'） */
    memcpy(info->name, c->cid.name, sizeof(c->cid.name));
    info->name[sizeof(info->name) - 1] = '\0';

    /* 标称容量 = 扇区总数 × 每扇区字节数（csd.capacity 单位为扇区） */
    uint64_t size = (uint64_t)c->csd.capacity * (uint64_t)c->csd.sector_size;
    if (size > 0) {
        info->capacity = size;
    }

    /* 按容量粗分卡类型（esp-idf 不直接给出 SDSC/SDHC/SDXC 字符串） */
    if (info->capacity >= 32000000000ULL) {
        strlcpy(info->type, "SDXC", sizeof(info->type));
    } else if (info->capacity >= 2000000000ULL) {
        strlcpy(info->type, "SDHC", sizeof(info->type));
    } else if (info->capacity > 0) {
        strlcpy(info->type, "SDSC", sizeof(info->type));
    } else {
        strlcpy(info->type, "SD", sizeof(info->type));
    }

    /* 文件系统已用 / 剩余（FatFs 卷 "0:" 对应 /sdcard） */
    FATFS *fs = NULL;
    DWORD fre_clust = 0;
    if (f_getfree("0:", &fre_clust, &fs) == FR_OK && fs) {
        uint64_t sect = fs->csize; /* 每簇扇区数 */
        info->total = ((uint64_t)(fs->n_fatent - 2) * sect) * 512;
        info->free  = (uint64_t)fre_clust * sect * 512;
    }
    ESP_LOGI(TAG, "SD info: %s %llu MB, free %llu MB",
             info->type, (unsigned long long)(info->capacity / (1024 * 1024)),
             (unsigned long long)(info->free / (1024 * 1024)));
    return ESP_OK;
}
