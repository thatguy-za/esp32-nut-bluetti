#pragma once
/*
 * Telegram push notifications for UPS events.
 *
 * Sending happens on a worker task fed by a queue, so the BLE and NUT
 * paths never block on HTTPS. Events are edge-triggered and rate-limited;
 * a flapping mains supply will not spam the chat.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    char bot_token[64];   /* from @BotFather, "123456:ABC-..."       */
    char chat_id[24];     /* user or group id; groups are negative   */
    bool on_power;        /* mains lost / restored                   */
    bool on_low_batt;     /* battery crossed the low threshold       */
    bool on_link;         /* EcoFlow BLE link lost / restored        */
} notify_config_t;

/* Start the worker. `label` prefixes every message so several bridges in
 * one chat stay distinguishable (typically the UPS name). */
int notify_start(const notify_config_t *cfg, const char *label);

/* Apply a new configuration at runtime (used after the settings change). */
void notify_reconfigure(const notify_config_t *cfg);

/* Feed the current NUT status string ("OL", "OB LB DISCHRG", "OFF", …).
 * Cheap and non-blocking; only transitions produce a message. */
void notify_ups_status(const char *status, int soc_pct, int runtime_min);

/* Send an arbitrary message (queued). Returns false if the queue is full
 * or notifications are disabled. */
bool notify_send(const char *text);

/* Blocking send used by the "send test message" button. Returns 0 on
 * success, non-zero with a reason in `err`. */
int notify_send_test(const notify_config_t *cfg, const char *label,
                     char *err, size_t err_sz);

#ifdef __cplusplus
}
#endif
