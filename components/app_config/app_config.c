#include "app_config.h"

#include <string.h>
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
#define CFG_VERSION 1u   /* independent lineage from the EcoFlow fork */

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

void app_config_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->wifi_mode = APP_WIFI_STATION;   /* what nearly everyone wants */
    strlcpy(cfg->wifi_ssid, CONFIG_WIFI_SSID, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_pass, CONFIG_WIFI_PASSWORD, sizeof(cfg->wifi_pass));
    strlcpy(cfg->ble_addr, CONFIG_BLUETTI_BLE_ADDRESS, sizeof(cfg->ble_addr));
    strlcpy(cfg->ble_name, CONFIG_BLUETTI_BLE_NAME, sizeof(cfg->ble_name));
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
    strlcpy(cfg->nut_user, "upsmon", sizeof(cfg->nut_user));
    cfg->nut_auth_set = false;       /* LOGIN open until set */
    strlcpy(cfg->auth_user, "admin", sizeof(cfg->auth_user));
    cfg->auth_set = false;           /* setup must choose a password */
    cfg->provisioned = false;

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

    cfg_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, CFG_KEY, &blob, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(blob) || blob.version != CFG_VERSION) {
        ESP_LOGW(TAG, "stored config unusable (err=%s len=%u ver=%u), defaults",
                 esp_err_to_name(err), (unsigned)len,
                 err == ESP_OK ? (unsigned)blob.version : 0u);
        return ESP_OK;
    }

    *cfg = blob.cfg;
    ESP_LOGI(TAG, "loaded config: ssid='%s' ble='%s' ups='%s' provisioned=%d",
             cfg->wifi_ssid, cfg->ble_addr[0] ? cfg->ble_addr : cfg->ble_name,
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
    cfg_blob_t blob = { .version = CFG_VERSION, .cfg = *cfg };
    err = nvs_set_blob(h, CFG_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "save config: %s", esp_err_to_name(err));
    return err;
}

esp_err_t app_config_erase(void)
{
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
