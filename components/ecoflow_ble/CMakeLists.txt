idf_component_register(
    SRCS
        "ecoflow_ble.c"
        "ef_crypto.c"
        "ef_frame.c"
        "ef_session.c"
        "ef_proto.c"
        "ef_cloud.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES bt
    PRIV_REQUIRES esp_timer micro_ecc mbedtls esp_http_client esp-tls json
    EMBED_FILES "ef_keydata.bin"
)
