idf_component_register(
    SRCS "pve_engine.c" "pve_shutdown.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls json esp_timer
)
