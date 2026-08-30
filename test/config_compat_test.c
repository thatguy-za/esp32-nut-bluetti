/* app_config forward compatibility.
 *
 * From CFG_VERSION 4, app_config_load keeps a stored blob written by an
 * older-but-recognised firmware (v3) instead of wiping it: the fields it
 * carried still mean what they meant, and the newer trailing fields come
 * up at their defaults. That only holds if fields are strictly appended.
 *
 * These checks pin that invariant against the real struct, so a future
 * reorder that would silently corrupt or discard everyone's config fails
 * here first. They mirror the loader's arithmetic — the real function
 * needs NVS — but against the actual app_config.h layout. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

static int fails;
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

/* Same shape as the cfg_blob_t in app_config.c. */
typedef struct {
    uint32_t     version;
    app_config_t cfg;
} blob_t;

int main(void)
{
    /* led_gpio must be the final member: appended, nothing after it. */
    OKF(offsetof(app_config_t, led_gpio) + sizeof(int16_t) == sizeof(app_config_t),
        "led_gpio is the last field in app_config_t");

    /* led_enabled must sit immediately before it (only alignment padding
     * between), which is what makes a v3 blob's length equal the offset of
     * led_gpio. */
    OKF(offsetof(app_config_t, led_gpio) > offsetof(app_config_t, led_enabled) &&
        offsetof(app_config_t, led_gpio) - offsetof(app_config_t, led_enabled) <= 2,
        "led_enabled immediately precedes led_gpio");

    /* The loader's bounds. */
    const size_t v4_len  = sizeof(blob_t);
    const size_t min_len = offsetof(blob_t, cfg) + offsetof(app_config_t, led_gpio);
    const size_t v3_len  = min_len;   /* a v3 blob ended exactly here */

    OKF(v3_len < v4_len, "a v3 blob is shorter than a v4 blob");

    /* Simulate loading a v3 blob into v4. */
    blob_t b;
    app_config_t defaults;
    memset(&defaults, 0, sizeof defaults);
    defaults.led_gpio = 48;                 /* the compiled-in default */
    defaults.led_enabled = true;
    strcpy(defaults.wifi_ssid, "default-ssid");

    /* Pre-seed, exactly as app_config_load does. */
    b.version = 4;
    b.cfg = defaults;

    /* The stored v3 image: different ssid, LED off, and — being v3 — no
     * led_gpio bytes at all. Build it in a separate buffer and copy only
     * v3_len bytes over, which is what nvs_get_blob would deliver. */
    blob_t stored;
    memset(&stored, 0xEE, sizeof stored);   /* garbage past v3_len */
    stored.version = 3;
    memset(&stored.cfg, 0, sizeof stored.cfg);
    strcpy(stored.cfg.wifi_ssid, "home-net");
    stored.cfg.led_enabled = false;
    stored.cfg.provisioned = true;

    size_t len = v3_len;
    memcpy(&b, &stored, len);               /* the short read */

    /* Loader acceptance test. */
    bool accepted = !(len > v4_len || len < min_len ||
                      b.version < 3u || b.version > 4u);
    OKF(accepted, "a v3-length, v3-version blob is accepted");

    app_config_t out = b.cfg;
    OKF(strcmp(out.wifi_ssid, "home-net") == 0, "stored ssid survives the load");
    OKF(out.led_enabled == false, "stored led_enabled survives the load");
    OKF(out.provisioned == true, "stored provisioned survives the load");
    OKF(out.led_gpio == 48, "led_gpio, absent from a v3 blob, keeps its default");

    /* Rejections. */
    OKF(!(3u >= 3u && 2u >= 3u), "sanity");
    #define ACCEPT(L, V) !((size_t)(L) > v4_len || (size_t)(L) < min_len || \
                           (V) < 3u || (V) > 4u)
    OKF(!ACCEPT(v3_len, 2u),         "version 2 is rejected");
    OKF(!ACCEPT(v3_len, 99u),        "a newer version is rejected");
    OKF(!ACCEPT(min_len - 1, 4u),    "a blob shorter than v3 is rejected");
    OKF(!ACCEPT(v4_len + 1, 4u),     "a blob longer than v4 is rejected");
    OKF(ACCEPT(v4_len, 4u),          "a full v4 blob is accepted");
    OKF(ACCEPT(v3_len, 3u),          "a full v3 blob is accepted");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
