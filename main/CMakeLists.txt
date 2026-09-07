idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_timer
        esp_netif
        app_update
        esp_app_format
        driver
        app_config
        wifi_mgr
        provisioning
        nut_server
        bluetti_ble
        notify
        led_status
)
