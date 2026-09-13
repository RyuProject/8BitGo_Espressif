# NimBLE 主机协议栈由 IDF 的 bt 组件提供；esp-hosted 提供 hosted-HCI 绑定层。
# 二者仅在 CONFIG_BT_NIMBLE_ENABLED 时被 gc_ble.c 引用，禁用时为无副作用的依赖。
idf_component_register(
    SRCS "gc_ble.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bt esp_hosted
)
