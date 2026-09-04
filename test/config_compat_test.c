/* app_config forward compatibility.
 *
 * From CFG_VERSION 3, app_config_load keeps a stored blob written by an
 * older-but-recognised firmware instead of wiping it: the fields it
 * carried still mean what they meant, and the newer trailing fields come
 * up at their defaults. That only holds if fields are strictly appended.
 *
 * The loader's smallest accepted blob is `offsetof(cfg_blob_t, cfg) +
 * offsetof(app_config_t, led_gpio)` — the size the struct was at v3/v4,
 * before controls_enabled. These checks pin the append-only invariant
 * against the real struct, so a reorder that would silently corrupt or
 * discard everyone's config fails here first. They mirror the loader's
 * arithmetic (the real function needs NVS) against the actual layout. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

/* Must track the #define in app_config.c. */
#define CFG_VERSION 5u

static int fails;
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

typedef struct {
    uint32_t     version;
    app_config_t cfg;
} blob_t;

/* The struct boundary a v3/v4 blob ended at — where the loader draws its
 * minimum length. Everything from here on was appended later. */
#define V3_END offsetof(app_config_t, led_gpio)

int main(void)
{
    /* Append-only: led_gpio (v4) and controls_enabled (v5) sit at the end,
     * in order, each right after the previous field bar alignment. */
    OKF(V3_END > offsetof(app_config_t, led_enabled) &&
        V3_END - offsetof(app_config_t, led_enabled) <= 2,
        "led_gpio was appended right after led_enabled (the v3 boundary)");
    OKF(offsetof(app_config_t, controls_enabled) >= V3_END + sizeof(int16_t) &&
        offsetof(app_config_t, controls_enabled) <= V3_END + sizeof(int16_t) + 1,
        "controls_enabled was appended right after led_gpio");
    OKF(offsetof(app_config_t, controls_enabled) + sizeof(bool) ==
        sizeof(app_config_t) ||
        offsetof(app_config_t, controls_enabled) + sizeof(bool) + 1 ==
        sizeof(app_config_t),
        "controls_enabled is the last field");

    const size_t full_len = sizeof(blob_t);
    const size_t min_len  = offsetof(blob_t, cfg) + V3_END;

    OKF(min_len < full_len, "a v3-boundary blob is shorter than the current one");

    /* Simulate loading a short (v3-boundary) blob into the current struct. */
    app_config_t defaults;
    memset(&defaults, 0, sizeof defaults);
    defaults.led_gpio = 48;
    defaults.controls_enabled = false;
    defaults.led_enabled = true;
    strcpy(defaults.wifi_ssid, "default-ssid");

    blob_t b = { .version = 5u, .cfg = defaults };   /* pre-seeded, as the loader does */

    blob_t stored;
    memset(&stored, 0xEE, sizeof stored);            /* garbage past the short length */
    memset(&stored.cfg, 0, sizeof stored.cfg);
    stored.version = 3u;
    strcpy(stored.cfg.wifi_ssid, "home-net");
    stored.cfg.led_enabled = false;
    stored.cfg.provisioned = true;

    size_t len = min_len;
    memcpy(&b, &stored, len);                        /* the short read */

    bool accepted = !(len > full_len || len < min_len ||
                      b.version < 3u || b.version > CFG_VERSION);
    OKF(accepted, "a v3-length, v3-version blob is accepted");

    OKF(strcmp(b.cfg.wifi_ssid, "home-net") == 0, "stored ssid survives the load");
    OKF(b.cfg.led_enabled == false, "stored led_enabled survives the load");
    OKF(b.cfg.provisioned == true, "stored provisioned survives the load");
    OKF(b.cfg.led_gpio == 48, "led_gpio, absent from the short blob, keeps its default");
    OKF(b.cfg.controls_enabled == false,
        "controls_enabled, absent from the short blob, keeps its default");

    #define ACCEPT(L, V) !((size_t)(L) > full_len || (size_t)(L) < min_len || \
                           (V) < 3u || (V) > CFG_VERSION)
    OKF(!ACCEPT(min_len, 2u),        "version 2 is rejected");
    OKF(!ACCEPT(min_len, CFG_VERSION + 1u), "a newer version is rejected");
    OKF(!ACCEPT(min_len - 1, 5u),    "a blob shorter than the v3 boundary is rejected");
    OKF(!ACCEPT(full_len + 1, 5u),   "a blob longer than the current struct is rejected");
    OKF(ACCEPT(full_len, CFG_VERSION), "a full current blob is accepted");
    OKF(ACCEPT(min_len, 3u),         "a full v3 blob is accepted");
    OKF(ACCEPT(min_len, 4u),         "a full v4 blob is accepted");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
