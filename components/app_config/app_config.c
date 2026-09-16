#include "app_config.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_mac.h"
#include <stdio.h>
#include "mbedtls/sha256.h"

static const char *TAG = "app_config";

#define CFG_NS      "efnut"
#define CFG_KEY     "cfg"
/*
 * Bumped whenever app_config_t's layout changes. A stored blob of a
 * different version (or a different size) is discarded and the device
 * comes up unprovisioned — there is no migration.
 *   2: dropped the advertised-name BLE targeting fallback.
 *   3: added the status-LED on/off toggle.
 *   4: added the status-LED pin.
 *   5: added the device-controls toggle.
 *   6: added the battery capacity (Wh) and the log level.
 *   7: added the fallback AP.
 *   8: added the Proxmox shutdown config.
 *   9: Proxmox triggers moved into each host. The block's layout changed,
 *      so a v8 block is reset to defaults (off, dry run) on load.
 *  10: each host's free-text guest list became per-guest rule slots
 *      (id + its own triggers). Layout changed again; v8 and v9 blocks are
 *      both reset to defaults on load.
 *  11: the guest rule array grew from 8 to 32 slots per host (the actual
 *      ceiling — one bit per guest in the engine's 32-bit fire bitmask).
 *      Layout changed again; v8, v9 and v10 blocks are all reset.
 *  12: guest rules moved out of app_config_t entirely — each host's list
 *      is now its own NVS blob (app_config_pve_guests_load/save), sized to
 *      however many are configured, not a compile-time cap. pve_host_t
 *      shrank; v8 through v11 blocks are all reset (their Proxmox settings,
 *      not their guest data — that lived inside the block being discarded
 *      and was never written to the new per-host keys, so it simply isn't
 *      there to read; nothing further to erase).
 *  13: added tg_on_pve (Telegram: a Proxmox host/guest shutdown). Appended
 *      after pve, not grouped with the other tg_on_* flags — layout is
 *      append-only and pve has to stay the field appended just before it.
 *  14: split tg_on_pve into tg_on_pve_host and tg_on_pve_guest. A v13 blob
 *      is kept, not reset — tg_on_pve_host lands on the old field's byte
 *      (so a v13 device's combined on/off carries over as its new "host"
 *      setting), and tg_on_pve_guest, new past the old struct's end, comes
 *      up at its pre-seeded default.
 *  15: added nut_enabled, the NUT server's on/off switch (pve.enabled and
 *      tg_enabled already existed). A pre-v15 blob is kept as usual, but
 *      nut_enabled is force-set true rather than left at its pre-seeded
 *      default (see app_config_load) — the field didn't exist, but NUT
 *      was never optional before now, and every device that already has a
 *      stored blob has been relying on it running.
 *
 * From v3 on, fields are only ever appended, and a stored blob of an
 * older-but-recognised version (3 to 7) is kept: the bytes that were
 * written still mean what they meant, and the newer trailing fields come
 * up at their defaults. A newer, much older, or unreadable blob is still
 * discarded.
 *
 * v8-v11's inline guest arrays made those blobs bigger than any version
 * since — the one case where an old blob is larger than sizeof(cfg_blob_t)
 * rather than smaller. app_config_load() sizes its read off the actual
 * on-disk length, not sizeof(cfg_blob_t), and for v8-v11 only trusts the
 * bytes before the pve field (see there for why): a fixed-size read once
 * discarded the whole blob outright on any v8-v11 device that upgraded
 * straight to v12+, since NVS refuses a read into a too-small buffer.
 */
#define CFG_VERSION 15u

/* Stored blob = version word + struct. The version guards against a
 * struct-layout change in a future firmware. */
typedef struct {
    uint32_t     version;
    app_config_t cfg;
} cfg_blob_t;

void app_config_default_name(char *buf, size_t len)
{
    uint8_t mac[6] = { 0 };
    /* eFuse read: works before esp_wifi_init(), unlike esp_wifi_get_mac(). */
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "esp-nut-bluetti-%02X%02X", mac[4], mac[5]);
}

void app_config_default_nut_password(char *buf, size_t len)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "bluetti%02X%02X", mac[4], mac[5]);
}

void app_config_pve_defaults(pve_config_t *pv)
{
    memset(pv, 0, sizeof(*pv));
    pv->enabled        = false;
    pv->armed          = false;
    pv->mains_back_min = 5;
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        pv->hosts[i].on_battery_min = 30;
        pv->hosts[i].charge_pct     = 10;
        pv->hosts[i].guest_wait_s   = 30;
        pv->hosts[i].shutdown_node  = true;
    }
}

/* One host's guest rules, kept out of the main blob so there's no
 * compile-time cap on how many. Key "pveg0".."pveg3" — well under NVS's
 * 15-character key limit even at PVE_MAX_HOSTS well past 4. */
static void guest_key(int host_i, char out[8])
{
    snprintf(out, 8, "pveg%d", host_i);
}

void app_config_pve_guests_load(int host_i, pve_guest_t **out, int *n)
{
    *out = NULL;
    *n = 0;
    if (host_i < 0 || host_i >= PVE_MAX_HOSTS) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        return;   /* nothing stored yet at all: no rules, not an error */
    }
    char key[8];
    guest_key(host_i, key);
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &len);
    if (err != ESP_OK || len == 0 || len % sizeof(pve_guest_t) != 0) {
        nvs_close(h);
        return;
    }
    pve_guest_t *buf = malloc(len);
    if (!buf) {
        ESP_LOGE(TAG, "no memory to load host %d's guest rules", host_i);
        nvs_close(h);
        return;
    }
    err = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        free(buf);
        return;
    }
    *out = buf;
    *n = (int)(len / sizeof(pve_guest_t));
}

esp_err_t app_config_pve_guests_save(int host_i, const pve_guest_t *arr, int n)
{
    if (host_i < 0 || host_i >= PVE_MAX_HOSTS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    guest_key(host_i, key);
    if (n <= 0 || !arr) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_blob(h, key, arr, (size_t)n * sizeof(pve_guest_t));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void app_config_pve_guests_erase_all(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        char key[8];
        guest_key(i, key);
        nvs_erase_key(h, key);   /* NOT_FOUND is fine: nothing to erase */
    }
    nvs_commit(h);
    nvs_close(h);
}

void app_config_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->wifi_mode = APP_WIFI_STATION;   /* what nearly everyone wants */
    strlcpy(cfg->wifi_ssid, CONFIG_WIFI_SSID, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_pass, CONFIG_WIFI_PASSWORD, sizeof(cfg->wifi_pass));
    strlcpy(cfg->ble_addr, CONFIG_BLUETTI_BLE_ADDRESS, sizeof(cfg->ble_addr));
    strlcpy(cfg->ups_name, CONFIG_NUT_UPS_NAME, sizeof(cfg->ups_name));
    cfg->nut_port = CONFIG_NUT_TCP_PORT;
    cfg->poll_ms  = CONFIG_BLUETTI_POLL_INTERVAL_MS;
    cfg->low_pct  = CONFIG_NUT_BATTERY_LOW_PCT;
    cfg->ac_rating_w = CONFIG_NUT_AC_RATING_W;
    cfg->runtime_low_s = CONFIG_NUT_RUNTIME_LOW_S;
    app_config_default_name(cfg->hostname, sizeof(cfg->hostname));
    cfg->use_static_ip = false;          /* DHCP unless asked otherwise */
    cfg->tg_on_power = true;             /* the events worth waking for */
    cfg->tg_on_low_batt = true;
    cfg->tg_on_link = false;
    cfg->tg_on_pve_host = true;           /* a host or guest going down is */
    cfg->tg_on_pve_guest = true;          /* worth it too */
    strlcpy(cfg->nut_user, "upsmon", sizeof(cfg->nut_user));
    /* A fresh device ships with a per-unit NUT password, "bluetti<XXXX>"
     * (XXXX = last two MAC bytes), so LOGIN is not wide open out of the
     * box. The UI shows it; the user can change or clear it. */
    {
        char pw[24];
        app_config_default_nut_password(pw, sizeof(pw));
        app_config_set_nut_password(cfg, pw);
    }
    strlcpy(cfg->auth_user, "admin", sizeof(cfg->auth_user));
    cfg->auth_set = false;           /* setup must choose a password */
    cfg->provisioned = false;
    cfg->led_enabled = true;
    cfg->led_gpio    = CONFIG_STATUS_LED_GPIO;
    cfg->controls_enabled = false;
    cfg->battery_wh = 0;
    cfg->log_level = APP_LOG_OFF;   /* quiet out of the box */
    cfg->ble_probe = false;   /* kept in sync with (log_level >= 2) */
    cfg->fb_ap_enabled = false;     /* don't broadcast unless asked */

    /* Every integration off on a genuinely fresh device — set up what you
     * want from the Settings tab. (A device upgrading from before these
     * switches existed keeps running what it already had; see the
     * version-gated overrides in app_config_load().) */
    cfg->nut_enabled = false;

    /* Proxmox shutdown: off, and dry-run even once on — nothing is powered
     * off until the user arms it. The triggers default to what a small
     * home unit can actually give you: half an hour, or 10%. */
    app_config_pve_defaults(&cfg->pve);

    /* A blank SSID from Kconfig means "must provision". */
    if (strcmp(cfg->wifi_ssid, "myssid") == 0) {
        cfg->wifi_ssid[0] = '\0';
        cfg->wifi_pass[0] = '\0';
    }
}

esp_err_t app_config_load(app_config_t *cfg)
{
    app_config_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no stored config (%s), using defaults",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    /* Find out how big the stored blob actually is before reading it. v8
     * through v11 embedded each host's guest rules inline (up to 32
     * pve_guest_t per host); that pve_config_t was far bigger than today's
     * (guests moved to their own per-host NVS blobs at v12), so an old
     * blob can be considerably LARGER than sizeof(cfg_blob_t) — the
     * reverse of every other version bump, which only ever appended
     * fields. Reading with a buffer sized to today's struct then fails
     * outright with ESP_ERR_NVS_INVALID_LENGTH — NVS won't do a partial
     * read into a too-small buffer — discarding a perfectly good config
     * instead of just resetting the part whose layout changed. */
    size_t on_disk = 0;
    err = nvs_get_blob(h, CFG_KEY, NULL, &on_disk);
    if (err != ESP_OK || on_disk > 8192) {
        nvs_close(h);
        ESP_LOGI(TAG, "no stored config (%s), using defaults",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    /* Pre-seed the struct with defaults so that a short read from an
     * older, smaller layout leaves the newer trailing fields alone.
     *
     * On the heap, not the stack: this runs on the main task, whose stack
     * is a few KB, and the blob can run past 2 KB for an old, guest-heavy
     * Proxmox block. v1.3.0 put it on the stack and overflowed on boot —
     * before the image could mark itself valid, so the bootloader quietly
     * rolled every device back to v1.2.0. */
    size_t alloc_len = on_disk > sizeof(cfg_blob_t) ? on_disk : sizeof(cfg_blob_t);
    cfg_blob_t *blob = malloc(alloc_len);
    if (!blob) {
        nvs_close(h);
        ESP_LOGE(TAG, "no memory to load config; using defaults");
        return ESP_OK;
    }
    blob->version = CFG_VERSION;
    blob->cfg = *cfg;
    size_t len = alloc_len;
    err = nvs_get_blob(h, CFG_KEY, blob, &len);
    nvs_close(h);

    /* Accept any v3+ blob: every version since only appends or reshuffles
     * fields, so a shorter blob still means what it says and the trailing
     * fields stay at their defaults. Anything older, newer, or a length
     * that cannot be a v3-or-later blob is discarded and the device
     * re-provisions. */
    const size_t min_len =
        offsetof(cfg_blob_t, cfg) + offsetof(app_config_t, led_gpio);
    if (err != ESP_OK || len < min_len ||
        blob->version < 3u || blob->version > CFG_VERSION) {
        ESP_LOGW(TAG, "stored config unusable (err=%s len=%u ver=%u), defaults",
                 esp_err_to_name(err), (unsigned)len,
                 err == ESP_OK ? (unsigned)blob->version : 0u);
        free(blob);
        return ESP_OK;
    }

    if (blob->version < CFG_VERSION) {
        ESP_LOGW(TAG, "config v%u < v%u: kept, new fields at defaults",
                 (unsigned)blob->version, (unsigned)CFG_VERSION);
    }
    if (blob->version <= 11u) {
        /* v8-v11's pve_config_t (inline guest arrays) is not today's —
         * neither is anything laid out after it (tg_on_pve_host/guest).
         * Only the bytes before it line up field-for-field with today's
         * struct; leave the rest at the defaults already seeded above. */
        memcpy(cfg, &blob->cfg, offsetof(app_config_t, pve));
    } else {
        *cfg = blob->cfg;
    }
    uint32_t stored_version = blob->version;
    free(blob);
    if (stored_version >= 8u && stored_version <= 11u) {
        /* v8's Proxmox block had global triggers and a different host
         * layout; v9 replaced a free-text guest list with 8 per-guest rule
         * slots; v10 grew that to 32; v11 moved guest rules out of the
         * block entirely. None of their bytes mean what v12's do. Off and
         * dry-run is the only safe reading of a block we cannot interpret. */
        ESP_LOGW(TAG, "config v%u: Proxmox settings reset (host layout changed in v12)",
                 (unsigned)stored_version);
        app_config_pve_defaults(&cfg->pve);
    }
    if (stored_version < 15u) {
        /* NUT had no switch before v15 — every provisioned device had it
         * running, unconditionally. A stored blob existing at all means
         * this device was already set up, so keep it running rather than
         * letting the field's fresh-device default (off) apply here. */
        cfg->nut_enabled = true;
    }
    if (stored_version < 6u) {
        /* Pre-v6 had only the ble_probe bool; map it onto the new level.
         * A device that wasn't in probe mode comes up quiet (the default). */
        cfg->log_level = cfg->ble_probe ? APP_LOG_VERBOSE : APP_LOG_OFF;
    }
    cfg->ble_probe = (cfg->log_level >= APP_LOG_VERBOSE);
    ESP_LOGI(TAG, "loaded config: ssid='%s' ble='%s' ups='%s' provisioned=%d",
             cfg->wifi_ssid, cfg->ble_addr,
             cfg->ups_name, cfg->provisioned);
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    cfg_blob_t *blob = malloc(sizeof(*blob));   /* same reason as in load */
    if (!blob) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    blob->version = CFG_VERSION;
    blob->cfg = *cfg;
    err = nvs_set_blob(h, CFG_KEY, blob, sizeof(*blob));
    free(blob);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "save config: %s", esp_err_to_name(err));
    return err;
}

esp_err_t app_config_erase(void)
{
    app_config_pve_guests_erase_all();
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, CFG_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "erase config: %s", esp_err_to_name(err));
    return err;
}

/* ---- admin password ------------------------------------------------ */

static void hash_password(const uint8_t salt[16], const char *password,
                          uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);            /* 0 = SHA-256, not SHA-224 */
    mbedtls_sha256_update(&c, salt, 16);
    mbedtls_sha256_update(&c, (const uint8_t *)password, strlen(password));
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

static void set_secret(uint8_t salt[16], uint8_t hash[32], bool *is_set,
                       const char *password)
{
    if (!password || password[0] == '\0') {
        memset(salt, 0, 16);
        memset(hash, 0, 32);
        *is_set = false;
        return;
    }
    esp_fill_random(salt, 16);
    hash_password(salt, password, hash);
    *is_set = true;
}

static bool check_secret(const uint8_t salt[16], const uint8_t hash[32],
                         bool is_set, const char *password)
{
    if (!is_set) {
        return true;              /* nothing set yet: nothing to enforce */
    }
    if (!password) {
        return false;
    }
    uint8_t want[32];
    hash_password(salt, password, want);
    /* Constant time: never leak how much of the hash matched. */
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(want); i++) {
        diff |= want[i] ^ hash[i];
    }
    return diff == 0;
}

void app_config_set_password(app_config_t *cfg, const char *password)
{
    set_secret(cfg->auth_salt, cfg->auth_hash, &cfg->auth_set, password);
}

bool app_config_check_password(const app_config_t *cfg, const char *password)
{
    return check_secret(cfg->auth_salt, cfg->auth_hash, cfg->auth_set, password);
}

void app_config_set_nut_password(app_config_t *cfg, const char *password)
{
    set_secret(cfg->nut_salt, cfg->nut_hash, &cfg->nut_auth_set, password);
}

bool app_config_check_nut_password(const app_config_t *cfg, const char *password)
{
    return check_secret(cfg->nut_salt, cfg->nut_hash, cfg->nut_auth_set, password);
}
