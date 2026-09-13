
idf_component_register(
    SRCS "esp32_p4_function_ev_board.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver esp_lcd esp_driver_ledc esp_driver_i2s esp_driver_sdmmc
    PRIV_REQUIRES usb spiffs fatfs
)
