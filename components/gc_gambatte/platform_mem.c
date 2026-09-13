/*
 * SPDX-FileCopyrightText: 2026 8BitGo
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file platform_mem.c
 * @brief ESP32 平台适配：gambatte 的 libretro 入口声明但未在核心内定义的符号
 *
 * 注意：本文件以 C 编译，函数默认即为 C 链接，与 libretro.cpp 的
 * `extern "C"` 声明匹配，无需也不能写 extern "C"（那是 C++ 语法）。
 * 音频出口 audio_out_buffer_write 改放在桥接（gc_emu_gb.cpp，C++），
 * 因为它需要把采样写入与 audio_batch_cb 共享的环形缓冲。
 */

#include <stdlib.h>
#include "esp_heap_caps.h"

/* gambatte 用它对 video 缓冲做 128 字节对齐分配 */
void *linearMemAlign(size_t size, size_t alignment)
{
    return heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_SPIRAM);
}

void linearFree(void *mem)
{
    heap_caps_free(mem);
}
