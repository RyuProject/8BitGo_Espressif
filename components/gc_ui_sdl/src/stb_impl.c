/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file stb_impl.c
 * @brief stb_image 唯一编译单元（JPEG/PNG 软解码，内存走 PSRAM）
 */

#include "esp_heap_caps.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MALLOC(sz)      heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_REALLOC(p, sz)  heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_FREE(p)         heap_caps_free(p)
#include "stb_image.h"
