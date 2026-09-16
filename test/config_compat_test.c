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
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

/* Must track the #define in app_config.c. */
#define CFG_VERSION 15u

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
    OKF(offsetof(app_config_t, pve) > offsetof(app_config_t, fb_ap_pass),
        "the Proxmox block (v8) was appended after the fallback AP");
    OKF(sizeof(pve_config_t) < 1300,
        "the Proxmox block is bounded (%zu bytes for %d hosts; guest rules "
        "live outside it now, unbounded)", sizeof(pve_config_t), PVE_MAX_HOSTS);
    OKF(offsetof(app_config_t, tg_on_pve_host) >= offsetof(app_config_t, pve) + sizeof(pve_config_t),
        "tg_on_pve_host (v13, split in v14) was appended after the Proxmox block");
    OKF(offsetof(app_config_t, tg_on_pve_guest) > offsetof(app_config_t, tg_on_pve_host),
        "tg_on_pve_guest (v14) follows tg_on_pve_host, in order");
    OKF(offsetof(app_config_t, nut_enabled) > offsetof(app_config_t, tg_on_pve_guest),
        "nut_enabled (v15) was appended after tg_on_pve_guest");
    OKF(offsetof(app_config_t, nut_enabled) + sizeof(bool) + 3 >= sizeof(app_config_t),
        "nut_enabled is the last field");

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

    /* v8-v11's inline guest arrays could make a real stored blob bigger
     * than sizeof(blob_t) — the loader sizes its read off the actual
     * on-disk length now, not the current struct, so "too long" is no
     * longer a rejection reason (an 8 KiB sanity cap still is). */
    #define MAX_ON_DISK 8192u
    #define ACCEPT(L, V) !((size_t)(L) > MAX_ON_DISK || (size_t)(L) < min_len || \
                           (V) < 3u || (V) > CFG_VERSION)
    OKF(!ACCEPT(min_len, 2u),        "version 2 is rejected");
    OKF(!ACCEPT(min_len, CFG_VERSION + 1u), "a newer version is rejected");
    OKF(!ACCEPT(min_len - 1, 5u),    "a blob shorter than the v3 boundary is rejected");
    OKF(!ACCEPT(MAX_ON_DISK + 1, 5u), "an implausibly large blob is rejected");
    OKF(ACCEPT(full_len + 1200, 10u), "a blob longer than the current struct is accepted "
        "(v8-v11's inline guest arrays could make one)");
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

    /* A v7 device upgrading to v8: the Proxmox block must come up with the
     * defaults the loader pre-seeded — off, dry-run — not with whatever
     * the short read left behind. Armed-by-accident is the one outcome
     * this feature must never produce. */
    const size_t v7_len = offsetof(blob_t, cfg) + offsetof(app_config_t, pve);
    OKF(ACCEPT(v7_len, 7u), "a v7-length blob is accepted by v14");
    blob_t v7 = { .version = 7u, .cfg = defaults };
    v7.cfg.pve.enabled = false;
    v7.cfg.pve.armed = false;
    v7.cfg.pve.hosts[0].on_battery_min = 30;
    blob_t stored7;
    memset(&stored7, 0xEE, sizeof stored7);          /* garbage past the read */
    memset(&stored7.cfg, 0, sizeof stored7.cfg);
    stored7.version = 7u;
    stored7.cfg.fb_ap_enabled = true;
    memcpy(&v7, &stored7, v7_len);
    OKF(v7.cfg.fb_ap_enabled == true, "a v7 blob's fallback-AP setting survives");
    OKF(!v7.cfg.pve.enabled && !v7.cfg.pve.armed,
        "Proxmox shutdown comes up OFF and in DRY RUN on a device upgrading from v7");

    /* v8 -> v9: the Proxmox block changed shape (triggers moved into the
     * hosts), so the loader resets it. The bytes of a v8 block read as a
     * v9 one are meaningless; this pins that they *would* be, so nobody
     * is tempted to drop the reset. */
    OKF(offsetof(pve_host_t, on_battery_min) > offsetof(pve_host_t, fingerprint),
        "v9 host triggers sit after the fingerprint (v8 had none)");
    OKF(ACCEPT(full_len, 8u), "a v8 blob is accepted (then its Proxmox block is reset)");

    /* The actual incident: a real v10 device (1.6.0, inline 8-guest arrays
     * per host) has an on-disk blob bigger than today's cfg_blob_t. Reading
     * it with a buffer sized to today's struct made NVS fail the read
     * outright (ESP_ERR_NVS_INVALID_LENGTH — it won't do a partial read
     * into a too-small buffer), discarding the WHOLE config, not just
     * Proxmox — the device came up unprovisioned, looking as if the OTA
     * that shipped v12+ had silently failed. Model that oversized blob
     * here and check the fields before `pve` still survive, and nothing
     * past it (which doesn't line up byte-for-byte with today's layout)
     * gets trusted. */
    {
        const size_t oversized_len = full_len + 1200;   /* bigger than sizeof(blob_t) */
        unsigned char *big = malloc(oversized_len);
        memset(big, 0xEE, oversized_len);                /* garbage past pve */
        blob_t *stored_big = (blob_t *)big;
        memset(&stored_big->cfg, 0, sizeof stored_big->cfg);
        stored_big->version = 10u;
        strcpy(stored_big->cfg.wifi_ssid, "home-net");
        stored_big->cfg.ble_probe = false;

        blob_t big_result = { .version = 10u, .cfg = defaults };  /* pre-seeded */
        big_result.cfg.pve.enabled = false;
        big_result.cfg.pve.armed = false;
        /* Only the pre-`pve` bytes are trusted for v8-v11 (see loader). */
        memcpy(&big_result.cfg, &stored_big->cfg, offsetof(app_config_t, pve));
        free(big);

        OKF(oversized_len > full_len,
            "the simulated v10 blob is bigger than today's struct, as the real one was");
        OKF(strcmp(big_result.cfg.wifi_ssid, "home-net") == 0,
            "an oversized v10 blob's ssid (before `pve`) survives the upgrade");
        OKF(!big_result.cfg.pve.enabled && !big_result.cfg.pve.armed,
            "Proxmox shutdown still comes up OFF and in DRY RUN from an oversized v10 blob");
    }

    /* v9 -> v10 and v10 -> v11 both reshaped the guest data that used to
     * live inside pve_host_t (free-text list, then 8 rule slots, then 32).
     * v12 removes it from pve_host_t altogether — moved to its own NVS
     * blob per host (app_config_pve_guests_load/save), sized to however
     * many rules are actually configured. A v9, v10 or v11 block's bytes
     * read as v12 are just as meaningless as v8's; same reset. */
    OKF(ACCEPT(full_len, 9u), "a v9 blob is accepted (then its Proxmox block is reset)");
    OKF(ACCEPT(full_len, 10u), "a v10 blob is accepted (then its Proxmox block is reset)");
    OKF(ACCEPT(full_len, 11u), "a v11 blob is accepted (then its Proxmox block is reset)");

    /* v11 -> v12: pve_host_t no longer carries any guest data at all —
     * pin that it doesn't, so nobody re-adds a fixed array here without
     * updating this suite. */
    OKF(offsetof(pve_host_t, guest_wait_s) < sizeof(pve_host_t) &&
        sizeof(pve_host_t) < 512,
        "pve_host_t is back to just its own fixed fields (%zu bytes)", sizeof(pve_host_t));

    /* v12 -> v13: tg_on_pve is a pure append (one bool, nothing existing
     * moved), so — unlike every pve bump above — a v12 block is simply
     * kept, not reset; the new field comes up at its pre-seeded default. */
    const size_t v12_len = offsetof(blob_t, cfg) + offsetof(app_config_t, tg_on_pve_host);
    OKF(ACCEPT(v12_len, 12u), "a v12-length blob is accepted by v14");
    blob_t v12 = { .version = 12u, .cfg = defaults };
    v12.cfg.tg_on_pve_host = true;                     /* defaults pre-seeded */
    blob_t stored12;
    memset(&stored12, 0xEE, sizeof stored12);
    memset(&stored12.cfg, 0, sizeof stored12.cfg);
    stored12.version = 12u;
    strcpy(stored12.cfg.wifi_ssid, "home-net");
    memcpy(&v12, &stored12, v12_len);                 /* the short read */
    OKF(strcmp(v12.cfg.wifi_ssid, "home-net") == 0,
        "a v12 blob's ssid survives the upgrade");
    OKF(v12.cfg.tg_on_pve_host == true,
        "the not-yet-split toggle, absent from the short blob, keeps its pre-seeded default");

    /* v13 -> v14: tg_on_pve split into two. tg_on_pve_host reuses the exact
     * byte tg_on_pve occupied — a v13 device's saved on/off carries over as
     * its new "host" setting — and tg_on_pve_guest, new past that byte,
     * comes up at its pre-seeded default rather than inheriting anything. */
    const size_t v13_len = offsetof(blob_t, cfg) + offsetof(app_config_t, tg_on_pve_host) + sizeof(bool);
    OKF(ACCEPT(v13_len, 13u), "a v13-length blob is accepted by v14");
    blob_t v13 = { .version = 13u, .cfg = defaults };
    v13.cfg.tg_on_pve_guest = true;                    /* defaults pre-seeded */
    blob_t stored13;
    memset(&stored13, 0xEE, sizeof stored13);
    memset(&stored13.cfg, 0, sizeof stored13.cfg);
    stored13.version = 13u;
    strcpy(stored13.cfg.wifi_ssid, "home-net");
    stored13.cfg.tg_on_pve_host = false;   /* the v13-era combined toggle, off */
    memcpy(&v13, &stored13, v13_len);      /* the short read */
    OKF(strcmp(v13.cfg.wifi_ssid, "home-net") == 0,
        "a v13 blob's ssid survives the upgrade");
    OKF(v13.cfg.tg_on_pve_host == false,
        "a v13 device's combined toggle carries over as tg_on_pve_host");
    OKF(v13.cfg.tg_on_pve_guest == true,
        "tg_on_pve_guest, new in v14 and absent from the short blob, keeps its default");

    /* v14 -> v15: nut_enabled is a pure append like tg_on_pve was, but
     * unlike every other new field, the loader does NOT leave it at its
     * pre-seeded (off) default for an upgrading device — see
     * app_config_load's `if (stored_version < 15u) cfg->nut_enabled =
     * true;`. NUT had no switch before v15 and was never not running, so
     * a stored blob existing at all means force it on. Only a genuinely
     * fresh device (app_config_defaults, no stored blob) starts with it
     * off. */
    const size_t v14_len = offsetof(blob_t, cfg) + offsetof(app_config_t, nut_enabled);
    OKF(ACCEPT(v14_len, 14u), "a v14-length blob is accepted by v15");
    blob_t v14 = { .version = 14u, .cfg = defaults };
    v14.cfg.nut_enabled = false;                       /* fresh-device default, pre-seeded */
    blob_t stored14;
    memset(&stored14, 0xEE, sizeof stored14);
    memset(&stored14.cfg, 0, sizeof stored14.cfg);
    stored14.version = 14u;
    strcpy(stored14.cfg.wifi_ssid, "home-net");
    memcpy(&v14, &stored14, v14_len);                  /* the short read */
    OKF(strcmp(v14.cfg.wifi_ssid, "home-net") == 0,
        "a v14 blob's ssid survives the upgrade");
    if (v14.version < 15u) {                           /* mirrors the loader's override */
        v14.cfg.nut_enabled = true;
    }
    OKF(v14.cfg.nut_enabled == true,
        "an upgrading v14 device keeps NUT on, despite nut_enabled's own fresh-device default being off");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
