idf_component_register(
    SRCS "wifi_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif
    PRIV_REQUIRES esp_timer
)
