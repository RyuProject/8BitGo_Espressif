idf_component_register(
    SRCS "gc_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client cjson
    PRIV_REQUIRES esp-tls
    EMBED_FILES "ca_8bitgo.pem"
)
