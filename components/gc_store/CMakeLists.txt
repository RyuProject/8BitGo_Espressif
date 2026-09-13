idf_component_register(
    SRCS "gc_store.c"
    INCLUDE_DIRS "include"
    REQUIRES gc_api esp32_p4_function_ev_board vfs
    PRIV_REQUIRES fatfs spiffs sdmmc
)
