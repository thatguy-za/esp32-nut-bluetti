#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime configuration, persisted in NVS (namespace "efnut", key "cfg").
 * Kconfig values are compile-time DEFAULTS only; whatever the user saves
 * through the provisioning portal wins.
 */
typedef struct {
    char     wifi_ssid[33];
    char     wifi_pass[65];
    char     ble_addr[18];   /* "AA:BB:CC:DD:EE:FF"; empty => use ble_name */
    char     ble_name[24];   /* advertised-name prefix fallback           */
    char     ef_user_id[40]; /* EcoFlow account user id (BLE auth)        */
    char     ups_name[32];
    uint16_t nut_port;
    uint16_t poll_ms;
    uint8_t  low_pct;
    bool     provisioned;    /* set true once a Wi-Fi STA connect succeeds */
} app_config_t;

/* Populate cfg from Kconfig defaults (no NVS access). */
void app_config_defaults(app_config_t *cfg);

/* Defaults, then overlay any values stored in NVS. Always succeeds
 * (falls back to defaults on a missing/corrupt blob). */
esp_err_t app_config_load(app_config_t *cfg);

/* Persist cfg to NVS. */
esp_err_t app_config_save(const app_config_t *cfg);

/* Wipe the stored blob so the next boot re-enters provisioning. */
esp_err_t app_config_erase(void);

#ifdef __cplusplus
}
#endif
