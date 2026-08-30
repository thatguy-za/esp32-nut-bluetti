idf_component_register(
    SRCS
        "provisioning.c"
        "dns_server.c"
        "log_ring.c"
    INCLUDE_DIRS "include"
    
    REQUIRES esp_http_server app_config wifi_mgr bluetti_ble notify ota_github
    PRIV_REQUIRES esp_wifi lwip esp_timer app_update esp_app_format mbedtls
    EMBED_TXTFILES "portal.html" "admin.html"
)
