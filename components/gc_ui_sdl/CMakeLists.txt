idf_component_register(
    SRCS "src/gc_ui_sdl.c" "src/ui_gfx.c" "src/stb_impl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES sdl3 gc_sdl_bsp gc_api gc_net gc_store gc_emu gc_prefs esp_lcd esp_lcd_touch
    PRIV_REQUIRES esp_timer pthread
)
