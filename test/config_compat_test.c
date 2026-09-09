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
#define CFG_VERSION 7u

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
    /* Append-only: led_gpio (v4), controls_enabled (v5), log_level (v6) and
     * the fallback AP (v7) sit at the end, in order, each right after the
     * previous field bar alignment. */
    OKF(V3_END > offsetof(app_config_t, led_enabled) &&
        V3_END - offsetof(app_config_t, led_enabled) <= 2,
        "led_gpio was appended right after led_enabled (the v3 boundary)");
    OKF(offsetof(app_config_t, controls_enabled) >= V3_END + sizeof(int16_t) &&
        offsetof(app_config_t, controls_enabled) <= V3_END + sizeof(int16_t) + 1,
        "controls_enabled was appended right after led_gpio");
    OKF(offsetof(app_config_t, log_level) > offsetof(app_config_t, controls_enabled) &&
        offsetof(app_config_t, log_level) <= offsetof(app_config_t, controls_enabled) + 4,
        "log_level was appended right after controls_enabled");
    OKF(offsetof(app_config_t, fb_ap_enabled) > offsetof(app_config_t, log_level) &&
        offsetof(app_config_t, fb_ap_enabled) <= offsetof(app_config_t, log_level) + 4,
        "fb_ap_enabled was appended right after log_level");
    OKF(offsetof(app_config_t, fb_ap_ssid) > offsetof(app_config_t, fb_ap_enabled) &&
        offsetof(app_config_t, fb_ap_pass) > offsetof(app_config_t, fb_ap_ssid),
        "the fallback AP's SSID and password follow it, in order");
    OKF(offsetof(app_config_t, fb_ap_pass) + sizeof(((app_config_t *)0)->fb_ap_pass)
            + 3 >= sizeof(app_config_t),
        "fb_ap_pass is the last field");

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

    /* A v6 device upgrading to v7: its blob ends where the fallback AP
     * begins, so the new fields must come up off/blank rather than
     * inheriting whatever was in memory. */
    const size_t v6_len = offsetof(blob_t, cfg) +
                          offsetof(app_config_t, fb_ap_enabled);
    OKF(ACCEPT(v6_len, 6u), "a v6-length blob is accepted by v7");

    blob_t v6 = { .version = 6u, .cfg = defaults };   /* defaults pre-seeded */
    v6.cfg.fb_ap_enabled = false;
    v6.cfg.fb_ap_ssid[0] = '\0';
    blob_t stored6;
    memset(&stored6, 0xEE, sizeof stored6);
    memset(&stored6.cfg, 0, sizeof stored6.cfg);
    stored6.version = 6u;
    strcpy(stored6.cfg.wifi_ssid, "home-net");
    memcpy(&v6, &stored6, v6_len);                    /* the short read */
    OKF(strcmp(v6.cfg.wifi_ssid, "home-net") == 0,
        "a v6 blob's ssid survives the upgrade");
    OKF(v6.cfg.fb_ap_enabled == false,
        "the fallback AP comes up off on a device upgrading from v6");
    OKF(v6.cfg.fb_ap_ssid[0] == '\0',
        "the fallback AP SSID comes up blank on a device upgrading from v6");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
