idf_component_register(
    SRCS "bluetti_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt
    PRIV_REQUIRES esp_timer
)
