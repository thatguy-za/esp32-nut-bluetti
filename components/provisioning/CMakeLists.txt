idf_component_register(
    SRCS
        "provisioning.c"
        "dns_server.c"
        "log_ring.c"
    INCLUDE_DIRS "include"
    
    REQUIRES esp_http_server app_config wifi_mgr ecoflow_ble
    PRIV_REQUIRES esp_wifi lwip esp_timer app_update esp_app_format
    EMBED_TXTFILES "portal.html" "admin.html"
)
