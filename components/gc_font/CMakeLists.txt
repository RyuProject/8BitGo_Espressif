# 字体源文件位于项目根 font/ 目录，这里直接纳入构建（不移动用户文件）
#
# GC_FONT_FULL 控制是否纳入真实字库（编译后约 0.9MB rodata）。
# OFF：gc_font_default() 走 GC_FONT_STUB，回退 LVGL 内置字体（只有 ASCII/拉丁，
#      界面里的中文会显示为空白）；ON：使用 PuHui 16（含中日韩）。
# 当前置为 ON，界面中文才能正常显示；做字体瘦身时可临时改回 OFF。
set(GC_FONT_FULL ON)

set(FONT_SRCS "gc_font.c")
if(GC_FONT_FULL)
    list(APPEND FONT_SRCS "${CMAKE_SOURCE_DIR}/font/font_puhui_16_4.c")
else()
    set(GC_FONT_DEF GC_FONT_STUB)
endif()

idf_component_register(
    SRCS ${FONT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES lvgl
)

if(DEFINED GC_FONT_DEF)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE ${GC_FONT_DEF})
endif()

# 生成的字库文件很大且含大量常量表，屏蔽相关告警
target_compile_options(${COMPONENT_TARGET} PRIVATE
    -Wno-unused-const-variable
    -Wno-unused-variable
    -Wno-error=unused-const-variable
)
