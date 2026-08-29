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
/* How the bridge puts itself on the network. */
typedef enum {
    APP_WIFI_STATION = 0,   /* join an existing network (default)  */
    APP_WIFI_AP      = 1,   /* run its own access point            */
} app_wifi_mode_t;

typedef struct {
    uint8_t  wifi_mode;      /* app_wifi_mode_t                           */
    char     wifi_ssid[33];  /* station: network to join                  */
    char     wifi_pass[65];
    char     ap_ssid[33];    /* AP mode: SSID to advertise (blank = auto) */
    char     ap_pass[65];    /* AP mode: blank = open network             */
    char     ble_addr[18];   /* "AA:BB:CC:DD:EE:FF"; empty => use ble_name */
    char     ble_name[24];   /* advertised-name prefix fallback           */
    char     ef_user_id[40]; /* EcoFlow account user id (BLE auth)        */
    char     ups_name[32];
    uint16_t nut_port;
    uint16_t poll_ms;
    uint8_t  low_pct;

    /* Admin-page login. The password is never stored in the clear: only a
     * random salt and SHA-256(salt || password) are kept. */
    char     auth_user[33];
    uint8_t  auth_salt[16];
    uint8_t  auth_hash[32];
    bool     auth_set;       /* a password has been chosen                 */

    bool     provisioned;    /* set true once Wi-Fi setup has succeeded    */
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

/* Choose the admin password: generates a fresh salt and stores the hash.
 * An empty password clears auth_set (leaves the UI unprotected). */
void app_config_set_password(app_config_t *cfg, const char *password);

/* Constant-time check of a candidate password against the stored hash.
 * Returns true when auth is unset (nothing to check yet). */
bool app_config_check_password(const app_config_t *cfg, const char *password);

#ifdef __cplusplus
}
#endif
