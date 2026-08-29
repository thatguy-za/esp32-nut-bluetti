#pragma once
/*
 * EcoFlow BLE "V2" session: drives the ECDH + session-key + auth
 * handshake, then decodes the telemetry stream into an ecoflow_state_t.
 *
 * Event-driven: feed inbound notification bytes with ef_session_feed(),
 * call ef_session_tick() ~1 Hz for handshake timeouts, and provide a
 * write callback for outbound BLE writes.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ecoflow_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int  (*ef_write_fn)(const uint8_t *data, size_t len, void *user);
typedef void (*ef_state_fn)(const ecoflow_state_t *st, void *user);

typedef enum {
    EF_SESS_IDLE,
    EF_SESS_PUBKEY,
    EF_SESS_KEYINFO,
    EF_SESS_AUTHSTATUS,
    EF_SESS_AUTH,
    EF_SESS_READY,
    EF_SESS_ERROR,
} ef_sess_state_t;

typedef struct ef_session ef_session_t;

ef_session_t *ef_session_create(void);

/* Start a fresh handshake. `sn` is the 16-char serial from the advert,
 * `user_id` the EcoFlow account id. Sends the first request via `write`.
 * Returns 0 on success. */
int ef_session_begin(ef_session_t *s,
                     const char *sn, const char *user_id, int encrypt_type,
                     int low_battery_pct,
                     ef_write_fn write, void *write_user,
                     ef_state_fn on_state, void *state_user);

void ef_session_feed(ef_session_t *s, const uint8_t *data, size_t len);
void ef_session_tick(ef_session_t *s);        /* call ~1 Hz */
void ef_session_reset(ef_session_t *s);       /* on BLE disconnect */

ef_sess_state_t ef_session_state(ef_session_t *s);
const char     *ef_session_error(ef_session_t *s);

#ifdef __cplusplus
}
#endif
