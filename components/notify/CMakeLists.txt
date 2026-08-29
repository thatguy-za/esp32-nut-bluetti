idf_component_register(
    SRCS "notify.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client esp-tls json esp_timer
)
