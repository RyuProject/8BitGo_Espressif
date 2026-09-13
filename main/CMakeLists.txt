idf_component_register(SRCS "main.c"
                      INCLUDE_DIRS "."
                      REQUIRES gc_net gc_api gc_store gc_emu gc_ui_sdl gc_provision gc_library gc_ble gc_sys esp32_p4_function_ev_board)
