idf_component_register(
    SRCS "nut_server.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip
    PRIV_REQUIRES esp_timer
)
