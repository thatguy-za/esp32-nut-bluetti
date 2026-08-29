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

    /* Station addressing. IPv4 only; DHCP unless use_static_ip. */
    char     hostname[33];   /* DHCP client name / mDNS label             */
    bool     use_static_ip;
    char     static_ip[16];
    char     static_mask[16];
    char     static_gw[16];
    char     static_dns[16];
    char     ble_addr[18];   /* "AA:BB:CC:DD:EE:FF", the only way to target */
    bool     ble_probe;      /* log GATT + notifications, decode nothing  */
    char     ups_name[32];
    uint16_t nut_port;
    uint16_t ac_rating_w;    /* continuous AC output rating, for ups.load  */
    uint16_t runtime_low_s;  /* battery.runtime.low, seconds               */

    /* NUT login. Standard NUT semantics: these gate LOGIN / PRIMARY (what
     * upsmon uses to coordinate shutdown); LIST/GET stay anonymous, as
     * upsd does and as `upsc` requires. Stored as a salted SHA-256. */
    char     nut_user[33];
    uint8_t  nut_salt[16];
    uint8_t  nut_hash[32];
    bool     nut_auth_set;
    uint16_t poll_ms;
    uint8_t  low_pct;

    /* Telegram notifications (see components/notify). */
    bool     tg_enabled;
    char     tg_token[64];
    char     tg_chat[24];
    bool     tg_on_power;
    bool     tg_on_low_batt;
    bool     tg_on_link;

    /* Admin-page login. The password is never stored in the clear: only a
     * random salt and SHA-256(salt || password) are kept. */
    char     auth_user[33];
    uint8_t  auth_salt[16];
    uint8_t  auth_hash[32];
    bool     auth_set;       /* a password has been chosen                 */

    bool     provisioned;    /* set true once Wi-Fi setup has succeeded    */
} app_config_t;

/* This board's default name, "esp-nut-bluetti-XXXX" (XXXX = last two
 * bytes of the Wi-Fi MAC). Used for both the setup AP SSID and the
 * default hostname, so the device answers to one name either way.
 * Reads the MAC from eFuse, so it is valid before Wi-Fi starts. */
void app_config_default_name(char *buf, size_t len);

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

/* Same, for the NUT protocol login. An empty password clears it, which
 * leaves LOGIN/PRIMARY open to anyone (upsd's default too). */
void app_config_set_nut_password(app_config_t *cfg, const char *password);
bool app_config_check_nut_password(const app_config_t *cfg, const char *password);

#ifdef __cplusplus
}
#endif
