#pragma once
/*
 * BLUETTI V2 session: the "2a2a" key exchange, the AES wrapper around
 * everything afterwards, and Modbus request/response on top.
 *
 * Byte-for-byte behaviour is documented in docs/PROTOCOL.md. The session
 * is driven entirely by what the device sends: it opens with a challenge
 * as soon as notifications are enabled.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_SESS_IDLE = 0,
    BT_SESS_CHALLENGE,   /* answered the challenge, awaiting the peer key */
    BT_SESS_PUBKEY,      /* sent our key, awaiting acceptance             */
    BT_SESS_READY,       /* secure key established, Modbus available      */
    BT_SESS_FAILED,
} bt_sess_state_t;

/* Emit bytes to the write characteristic. */
typedef int (*bt_sess_tx_t)(const uint8_t *data, size_t len, void *user);

/* A complete, validated Modbus response body (register data only). */
typedef void (*bt_sess_regs_t)(uint16_t start_addr, const uint8_t *data,
                               size_t len, void *user);

typedef struct bt_session bt_session_t;

bt_session_t *bt_session_new(bt_sess_tx_t tx, bt_sess_regs_t on_regs, void *user);
void          bt_session_free(bt_session_t *s);

/* Drop all negotiated state; call on every (re)connect. */
void bt_session_reset(bt_session_t *s);

bt_sess_state_t bt_session_state(const bt_session_t *s);
bool            bt_session_ready(const bt_session_t *s);

/* Feed bytes from a notification. Handles reassembly internally. */
void bt_session_feed(bt_session_t *s, const uint8_t *data, size_t len);

/* Request `count` holding registers from `addr`. Only valid once ready. */
int bt_session_read_regs(bt_session_t *s, uint16_t addr, uint16_t count);

#ifdef __cplusplus
}
#endif
