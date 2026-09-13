idf_component_register(
    SRCS "gc_library.c"
    INCLUDE_DIRS "include"
    REQUIRES gc_api gc_store gc_emu gc_ui_sdl gc_net gc_sys esp32_p4_function_ev_board esp_codec_dev
)
