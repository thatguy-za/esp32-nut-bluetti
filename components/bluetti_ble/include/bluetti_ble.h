#pragma once
/*
 * BLUETTI power-station link over Bluetooth LE.
 *
 * Connects to the unit's ff00 service, runs the "2a2a" key exchange
 * (bt_session.c), then polls Modbus holding registers one field at a
 * time and decodes them (bt_regs.c). The port follows
 * Patrick762/bluetti-bt-lib — see docs/PROTOCOL.md — but has not been run
 * against a physical unit. A unit that turns out not to encrypt falls
 * back to plain Modbus after the handshake window; PROBE mode enumerates
 * GATT and hex-dumps notifications without decoding, for when a reading
 * looks wrong.
 *
 * The state struct below is the shape the NUT layer consumes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of the power station's electrical state. Fields left at the
 * sentinels below were not present in the last decoded frame. */
#define BLUETTI_UNKNOWN_F (-1000.0f)
#define BLUETTI_UNKNOWN_I (-1)

typedef struct {
    bool     valid;             /* at least one full decode has happened */
    int64_t  updated_us;        /* esp_timer_get_time() of last update   */

    int      soc_pct;           /* battery state of charge, 0..100       */
    int      soc_low_pct;       /* effective low-battery threshold       */
    bool     ac_input_present;  /* mains feeding the unit                */
    bool     charging;
    int      minutes_remaining; /* runtime estimate, -1 = n/a            */

    float    input_watts;       /* ac_in + dc_in                          */
    float    output_watts;      /* ac_out + dc_out                        */
    float    ac_in_watts;       /* <0 = n/a */
    float    ac_out_watts;      /* <0 = n/a */
    float    dc_in_watts;       /* <0 = n/a */
    float    dc_out_watts;      /* <0 = n/a */
    float    battery_watts;     /* >0 discharging */
    float    battery_voltage;   /* 0 = n/a */
    float    battery_temp_c;
    int      design_capacity_wh;

    float    ac_in_volts;       /* -1 = n/a */
    float    ac_in_amps;        /* -1 = n/a */
    float    ac_out_volts;      /* -1 = n/a */
    int      ac_switch;         /* -1 = n/a, else 0/1 */
    int      dc_switch;         /* -1 = n/a, else 0/1 */

    bool     ups_mode_on;       /* the unit's UPS / backup mode           */
    uint32_t error_code;

    char     serial[24];
    char     model[24];
} bluetti_state_t;

typedef void (*bluetti_state_cb_t)(const bluetti_state_t *state, void *user);

typedef struct {
    const char *ble_address;     /* "AA:BB:CC:DD:EE:FF"; required */
    uint32_t    poll_interval_ms;
    int         low_battery_pct;
    bool        probe;           /* dump GATT + notifications to the log */
} bluetti_ble_config_t;

/* One device seen during a scan. */
typedef struct {
    char   addr[18];
    char   name[32];
    int8_t rssi;
    bool   looks_like_bluetti;   /* advertised name matches a known prefix */
} bluetti_scan_entry_t;

typedef void (*bluetti_scan_cb_t)(const bluetti_scan_entry_t *e, void *user);

/* Bring up the NimBLE host without connecting. Idempotent; blocks up to
 * ~5 s for the controller to sync. Call once, after nvs_flash_init(). */
int bluetti_ble_host_init(void);

/* Discovery scan for `duration_ms`; `cb` fires once per new device. */
int  bluetti_ble_scan(uint32_t duration_ms, bluetti_scan_cb_t cb, void *user);
void bluetti_ble_scan_stop(void);
bool bluetti_ble_scanning(void);

/* Connect to the configured target and start the session. With
 * `probe` set this enumerates GATT and logs notifications instead of
 * attempting to decode anything. */
int bluetti_ble_start(const bluetti_ble_config_t *config,
                      bluetti_state_cb_t cb, void *user);

/* Copy the most recent state. False if nothing has been decoded yet. */
bool bluetti_ble_get_state(bluetti_state_t *out);

/* True while a BLE link to the unit is up. */
bool bluetti_ble_connected(void);

#ifdef __cplusplus
}
#endif
