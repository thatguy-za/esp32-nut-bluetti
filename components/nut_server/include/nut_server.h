#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUT_MAX_VARS      48
#define NUT_VAR_NAME_LEN  40
#define NUT_VAR_VALUE_LEN 64

typedef struct {
    const char *ups_name;      /* e.g. "ecoflow" */
    const char *ups_desc;      /* e.g. "EcoFlow Delta 2" */
    uint16_t    tcp_port;      /* usually 3493 */
    uint8_t     max_clients;   /* concurrent NUT connections */
} nut_server_config_t;

/* Start the NUT (upsd-compatible) TCP server. Spawns its own task(s).
 * Returns ESP_OK on success. Safe to call once. */
int nut_server_start(const nut_server_config_t *config);

/* Create or update a UPS variable (e.g. "battery.charge" = "87").
 * Thread-safe; call from your polling loop. Unknown names are added
 * until NUT_MAX_VARS is reached. */
void nut_server_set_var(const char *name, const char *value);
void nut_server_set_var_int(const char *name, long value);
void nut_server_set_var_float(const char *name, float value, int decimals);

/* Remove a variable so it no longer appears in LIST VAR. */
void nut_server_clear_var(const char *name);

/* Convenience: set ups.status, the field upsmon watches
 * (e.g. "OL", "OB", "OB LB", "OL CHRG"). */
void nut_server_set_status(const char *status);

/*
 * Optional NUT login, with upsd's semantics: the credentials gate LOGIN
 * and PRIMARY/MASTER (what upsmon uses to coordinate shutdown). LIST and
 * GET stay anonymous either way — `upsc` has no way to send credentials,
 * so requiring them for reads would break every read-only client.
 *
 * `verify` is called with the USERNAME/PASSWORD the client supplied and
 * must return true to accept. Pass NULL to leave LOGIN open to anyone.
 */
typedef bool (*nut_auth_cb_t)(const char *user, const char *pass, void *ctx);
void nut_server_set_auth(nut_auth_cb_t verify, void *ctx);

#ifdef __cplusplus
}
#endif
