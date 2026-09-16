#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pve_shutdown.h"

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

    /* Board status LED (see components/led_status). */
    bool     led_enabled;    /* user toggle; the pin itself may not exist */
    int16_t  led_gpio;       /* addressable-LED pin; -1 = off. Runtime-set
                                so a board's LED can be found without a
                                rebuild. Defaults to CONFIG_STATUS_LED_GPIO. */

    /* Device controls. Off by default: writing to the power station is
     * unverified against hardware and only makes sense for an EL10. */
    bool     controls_enabled;

    /* Usable battery energy, Wh. 0 = unknown. Used to estimate
     * battery.runtime on units that don't report a runtime themselves. */
    uint16_t battery_wh;

    /* Log verbosity, set from the Status page. 0 = off (esp_log silenced),
     * 1 = basic (INFO), 2 = verbose (INFO + BLE debugging mode: dump the
     * GATT tree and every notification instead of decoding). ble_probe is
     * kept equal to (log_level >= 2). */
    uint8_t  log_level;

    /* Fallback AP. In station mode, if the network stays unreachable for
     * APP_FALLBACK_AP_AFTER_S the bridge raises its own AP so it can still
     * be reached; it drops again when the network returns. Off by default:
     * an always-on device should not start broadcasting unasked. */
    bool     fb_ap_enabled;
    char     fb_ap_ssid[33];   /* blank = the board's default name */
    char     fb_ap_pass[65];   /* blank = open network             */

    /* Proxmox shutdown on UPS events (see components/pve_shutdown). The
     * token secrets live in here in the clear, like the Telegram token;
     * they are never sent back out through the config JSON. */
    pve_config_t pve;

    /* Telegram: a host shutdown, and a guest shutdown, pve_shutdown fires
     * (or would, in dry run) — separate toggles, so a guest-heavy setup
     * doesn't have to choose between silence and one alert per VM/CT.
     * Grouped with the other tg_on_* flags above in spirit, but appended
     * here — layout is append-only, and pve must stay the last field
     * appended before them. */
    bool     tg_on_pve_host;
    bool     tg_on_pve_guest;

    /* Master on/off for the NUT server, alongside pve.enabled and
     * tg_enabled — the three integrations the Settings tab's "Integrations"
     * box can turn off and hide from the nav bar. Bluetti itself has no
     * such switch: it's how the bridge reads the unit at all, not
     * optional. A fresh device starts with all three off; a device
     * upgrading from before this field existed comes up with NUT on
     * regardless (see app_config_load) since it had no such switch and
     * was never not running. */
    bool     nut_enabled;
} app_config_t;

/* How long the station has to be down before the fallback AP comes up.
 * Long enough to sit out a router reboot, short enough to be useful. */
#define APP_FALLBACK_AP_AFTER_S 60

/* Log-level values for app_config_t.log_level. */
#define APP_LOG_OFF     0
#define APP_LOG_BASIC   1
#define APP_LOG_VERBOSE 2

/* This board's default name, "esp-nut-bluetti-XXXX" (XXXX = last two
 * bytes of the Wi-Fi MAC). Used for both the setup AP SSID and the
 * default hostname, so the device answers to one name either way.
 * Reads the MAC from eFuse, so it is valid before Wi-Fi starts. */
void app_config_default_name(char *buf, size_t len);

/* This board's out-of-the-box NUT password, "bluetti<XXXX>" (XXXX = last
 * two MAC bytes). Only the initial value — the stored one is a hash. */
void app_config_default_nut_password(char *buf, size_t len);

/* Populate cfg from Kconfig defaults (no NVS access). */
void app_config_defaults(app_config_t *cfg);

/* Just the Proxmox block: off, dry run, 30 min / 10 % per host. */
void app_config_pve_defaults(pve_config_t *pv);

/* One host's guest rules — stored as their own NVS blob per host, not in
 * app_config_t, since there's no compile-time cap on how many a host can
 * have. `*out` is malloc'd (NULL if `*n` comes back 0); the caller frees
 * it. Never fails outright: a missing/corrupt blob just reads as no rules. */
void app_config_pve_guests_load(int host_i, pve_guest_t **out, int *n);

/* Replaces host `host_i`'s stored guest rules with `arr` (n entries; n=0
 * or arr=NULL erases the blob rather than storing an empty one). */
esp_err_t app_config_pve_guests_save(int host_i, const pve_guest_t *arr, int n);

/* Erases every host's stored guest rules — used when the whole Proxmox
 * block resets (an old config version, or the user clearing a host). */
void app_config_pve_guests_erase_all(void);

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
