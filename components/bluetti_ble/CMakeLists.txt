idf_component_register(
    SRCS
        "bluetti_ble.c"
        "bt_crypto.c"
        "bt_session.c"
        "bt_regs.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES bt
    PRIV_REQUIRES esp_timer mbedtls esp_hw_support
)
