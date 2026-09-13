# SPDX-FileCopyrightText: 2026 8BitGo
#
# Game Boy / Game Boy Color 核心：libretro/gambatte-libretro（子模块在 ./gambatte）
#
# 编译选项对齐上游 Makefile.common / Makefile.libretro：
#   VIDEO_RGB565=1  -> 核心直接输出 RGB565 uint16，省去像素格式转换
#   -fno-exceptions -fno-rtti（C++）
# 平台适配符号（linearMemAlign / audio_out_buffer_write 等）由本目录的
# platform_mem.c 提供，见下方 SRCS。

set(GAMBATTE_DIR ${CMAKE_CURRENT_LIST_DIR}/gambatte)

# 头文件路径（对应上游 Makefile.common 的 INCFLAGS）
# 注：上游 INCFLAGS 里的 common/resample 在当前子模块版本中已不存在
#     （重采样实现与头文件都在 libgambatte/libretro/ 下），保留会直接导致 CMake 报错。
set(INCS
    ${GAMBATTE_DIR}/libgambatte/src
    ${GAMBATTE_DIR}/libgambatte/include
    ${GAMBATTE_DIR}/common
    ${GAMBATTE_DIR}/libgambatte/libretro
    ${GAMBATTE_DIR}/libgambatte/libretro-common/include
)

# 源文件（对应上游 Makefile.common 的 SOURCES_C / SOURCES_CXX）
# HAVE_NETWORK=0：不编 net_serial.cpp，减少依赖
set(SRCS_VENDOR
    # --- libretro glue (C) ---
    ${GAMBATTE_DIR}/libgambatte/libretro/gambatte_log.c
    ${GAMBATTE_DIR}/libgambatte/libretro/blipper.c
    ${GAMBATTE_DIR}/libgambatte/libretro/cc_resampler.c
    # --- libretro-common (C)，非 static linking 所需 ---
    ${GAMBATTE_DIR}/libgambatte/libretro-common/compat/compat_posix_string.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/compat/compat_snprintf.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/compat/compat_strcasestr.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/compat/compat_strl.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/compat/fopen_utf8.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/encodings/encoding_utf.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/file/file_path.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/file/file_path_io.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/streams/file_stream.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/streams/file_stream_transforms.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/string/stdstring.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/time/rtime.c
    ${GAMBATTE_DIR}/libgambatte/libretro-common/vfs/vfs_implementation.c
    # --- core (C++) ---
    ${GAMBATTE_DIR}/libgambatte/src/bootloader.cpp
    ${GAMBATTE_DIR}/libgambatte/src/cpu.cpp
    ${GAMBATTE_DIR}/libgambatte/src/gambatte.cpp
    ${GAMBATTE_DIR}/libgambatte/src/initstate.cpp
    ${GAMBATTE_DIR}/libgambatte/src/interrupter.cpp
    ${GAMBATTE_DIR}/libgambatte/src/interruptrequester.cpp
    ${GAMBATTE_DIR}/libgambatte/src/gambatte-memory.cpp
    ${GAMBATTE_DIR}/libgambatte/src/sound.cpp
    ${GAMBATTE_DIR}/libgambatte/src/statesaver.cpp
    ${GAMBATTE_DIR}/libgambatte/src/tima.cpp
    ${GAMBATTE_DIR}/libgambatte/src/video.cpp
    ${GAMBATTE_DIR}/libgambatte/src/video_libretro.cpp
    ${GAMBATTE_DIR}/libgambatte/src/mem/cartridge.cpp
    ${GAMBATTE_DIR}/libgambatte/src/mem/cartridge_libretro.cpp
    ${GAMBATTE_DIR}/libgambatte/src/mem/huc3.cpp
    ${GAMBATTE_DIR}/libgambatte/src/mem/memptrs.cpp
    ${GAMBATTE_DIR}/libgambatte/src/mem/rtc.cpp
    ${GAMBATTE_DIR}/libgambatte/src/sound/channel1.cpp
    ${GAMBATTE_DIR}/libgambatte/src/sound/channel2.cpp
    ${GAMBATTE_DIR}/libgambatte/src/sound/channel3.cpp
    ${GAMBATTE_DIR}/libgambatte/src/sound/channel4.cpp
    ${GAMBATTE_DIR}/libgambatte/src/sound/duty_unit.cpp
    ${GAMBATTE_DIR}/libgambatte/src/sound/envelope_unit.cpp
    ${GAMBATTE_DIR}/libgambatte/src/sound/length_counter.cpp
    ${GAMBATTE_DIR}/libgambatte/src/video/ly_counter.cpp
    ${GAMBATTE_DIR}/libgambatte/src/video/lyc_irq.cpp
    ${GAMBATTE_DIR}/libgambatte/src/video/next_m0_time.cpp
    ${GAMBATTE_DIR}/libgambatte/src/video/ppu.cpp
    ${GAMBATTE_DIR}/libgambatte/src/video/sprite_mapper.cpp
    ${GAMBATTE_DIR}/libgambatte/libretro/libretro.cpp
)

# 本组件自己写的平台适配层（linearMemAlign / audio_out_buffer_write 等）保留 -Werror
set(SRCS ${SRCS_VENDOR} ${CMAKE_CURRENT_LIST_DIR}/platform_mem.c)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCS}
)

# 第三方核心源码不保证无告警（-Wreorder / -Wsign-compare ...），
# 在 -Werror 下会直接编译失败，所以只对上游文件关闭告警，本组件自己的代码不受影响。
set_source_files_properties(${SRCS_VENDOR} PROPERTIES COMPILE_OPTIONS "-w")

# 关键编译选项（对齐上游 Makefile.libretro）
#
# 额外必须项 HAVE_NO_LANGEXTRA：
#   gambatte 自带 26 种语言的核心选项翻译表，全部是「已初始化的静态数组」，
#   合计 ~800KB 静态 .data（实测 libretro.cpp.obj 的 .data = 815KB）。
#   而 ESP32-P4 的 dram_seg（sram_low）只有 ~179KB 且与 IRAM 共用，
#   会导致链接期直接溢出（ld 报 "discards section"，再 SIGSEGV）。
#   本前端不暴露核心选项（env_cb 对 SET_CORE_OPTIONS 返回 false），
#   HAVE_NO_LANGEXTRA 是上游提供的官方开关，可一次性裁掉全部翻译表。
target_compile_definitions(${COMPONENT_TARGET} PRIVATE
    VIDEO_RGB565
    __LIBRETRO__
    HAVE_STDINT_H
    HAVE_INTTYPES_H
    CC_RESAMPLER_NO_HIGHPASS
    HAVE_NO_LANGEXTRA
)
target_compile_options(${COMPONENT_TARGET} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -std=gnu++11>
)
# 链接 C++ 标准库；--allow-multiple-definition 容忍与 newlib 可能的重复 compat 符号
target_link_libraries(${COMPONENT_TARGET} INTERFACE stdc++ -Wl,--allow-multiple-definition)
