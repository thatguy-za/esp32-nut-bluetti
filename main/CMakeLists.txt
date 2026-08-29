idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_timer
        driver
        app_config
        wifi_mgr
        provisioning
        nut_server
        ecoflow_ble
)
