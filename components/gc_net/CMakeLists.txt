idf_component_register(
    SRCS "gc_net.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_wifi_remote
    PRIV_REQUIRES nvs_flash
)
