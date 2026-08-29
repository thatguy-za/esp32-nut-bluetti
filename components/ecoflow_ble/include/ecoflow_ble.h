#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of the EcoFlow unit's electrical state. Fields set to the
 * sentinel below were not present in the last decoded frame. */
#define ECOFLOW_UNKNOWN_F (-1000.0f)
#define ECOFLOW_UNKNOWN_I (-1)

typedef struct {
    bool  valid;              /* at least one full decode has happened */
    int64_t updated_us;       /* esp_timer_get_time() of last update   */

    int   soc_pct;            /* battery state of charge, 0..100       */
    int   soc_low_pct;        /* effective low-battery threshold       */
    bool  ac_input_present;   /* mains / adapter feeding the unit      */
    bool  charging;           /* battery is charging                   */
    int   minutes_remaining;  /* runtime estimate on battery, -1 = n/a */
    int   minutes_to_full;    /* charge time remaining, -1 = n/a       */

    float input_watts;        /* total input power (W)                 */
    float output_watts;       /* total output/load power (W)           */
    float ac_in_watts;        /* AC-input power (W), -1 = n/a          */
    float ac_out_watts;       /* AC-output power (W), -1 = n/a         */
    float battery_watts;      /* pack power (W); >0 discharging         */
    float battery_voltage;    /* pack voltage (V), 0 = n/a             */
    float battery_temp_c;     /* pack temperature (deg C)              */
    int   design_capacity_wh; /* nominal battery capacity (Wh)         */

    bool     backup_mode_on;  /* "energy backup" (UPS) mode enabled     */
    int      backup_reserve_pct;
    uint32_t error_code;      /* device fault code, 0 = none           */

    char  serial[24];         /* device serial, if learned            */
    char  model[24];          /* model string, if learned             */
} ecoflow_state_t;

/* Called on the BLE host task each time a fresh state is decoded.
 * Keep it short; copy what you need. */
typedef void (*ecoflow_state_cb_t)(const ecoflow_state_t *state, void *user);

typedef struct {
    const char *ble_address;     /* "AA:BB:CC:DD:EE:FF" or NULL/"" */
    const char *ble_name_prefix; /* used when address is empty     */
    const char *user_id;         /* EcoFlow account user id (auth) */
    uint32_t    poll_interval_ms;
    int         low_battery_pct;
} ecoflow_ble_config_t;

/* One BLE device seen during a scan. */
typedef struct {
    char   addr[18];   /* "AA:BB:CC:DD:EE:FF" */
    char   name[32];   /* advertised name, "" if none */
    int8_t rssi;
    bool   looks_like_ecoflow; /* name starts with a known EcoFlow prefix */
} ecoflow_scan_entry_t;

typedef void (*ecoflow_scan_cb_t)(const ecoflow_scan_entry_t *entry, void *user);

/* Bring up the NimBLE host (port init + host task) without connecting or
 * scanning. Idempotent; blocks up to ~5s for the controller to sync.
 * Call once, after nvs_flash_init(). */
int ecoflow_ble_host_init(void);

/* Start a discovery scan for the given duration. `cb` is invoked once per
 * newly-seen device from the BLE host task. Non-blocking; the scan stops
 * itself after duration_ms (or call ecoflow_ble_scan_stop). Not allowed
 * while a connect session is active. */
int  ecoflow_ble_scan(uint32_t duration_ms, ecoflow_scan_cb_t cb, void *user);
void ecoflow_ble_scan_stop(void);
bool ecoflow_ble_scanning(void);

/* Start the connect/subscribe/poll state machine toward the configured
 * target. Requires ecoflow_ble_host_init() first. */
int ecoflow_ble_start(const ecoflow_ble_config_t *config,
                      ecoflow_state_cb_t cb, void *user);

/* Copy the most recent state. Returns false if nothing decoded yet. */
bool ecoflow_ble_get_state(ecoflow_state_t *out);

/* True while a BLE link to the unit is up. */
bool ecoflow_ble_connected(void);

/* One-shot EcoFlow account login (HTTPS) to resolve the account user id
 * that the BLE auth handshake requires. `region` is "" / "auto" / "api" /
 * "api-e" / "api-a" / "api-j" / "api-r" / "api-cn". The password is used
 * only for this call. Returns 0 and fills `out` on success; non-zero and
 * fills `err` otherwise. Blocking; call from a normal task with Wi-Fi up. */
int ecoflow_resolve_user_id(const char *identifier, const char *password,
                            const char *region,
                            char *out, size_t out_sz,
                            char *err, size_t err_sz);

#ifdef __cplusplus
}
#endif
