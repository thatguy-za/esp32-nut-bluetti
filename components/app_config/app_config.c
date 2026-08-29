#include "app_config.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "app_config";

#define CFG_NS      "efnut"
#define CFG_KEY     "cfg"
#define CFG_VERSION 3u

/* Stored blob = version word + struct. The version guards against a
 * struct-layout change in a future firmware. */
typedef struct {
    uint32_t     version;
    app_config_t cfg;
} cfg_blob_t;

void app_config_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->wifi_mode = APP_WIFI_STATION;   /* what nearly everyone wants */
    strlcpy(cfg->wifi_ssid, CONFIG_WIFI_SSID, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_pass, CONFIG_WIFI_PASSWORD, sizeof(cfg->wifi_pass));
    strlcpy(cfg->ble_addr, CONFIG_ECOFLOW_BLE_ADDRESS, sizeof(cfg->ble_addr));
    strlcpy(cfg->ble_name, CONFIG_ECOFLOW_BLE_NAME, sizeof(cfg->ble_name));
    strlcpy(cfg->ef_user_id, CONFIG_ECOFLOW_USER_ID, sizeof(cfg->ef_user_id));
    strlcpy(cfg->ups_name, CONFIG_NUT_UPS_NAME, sizeof(cfg->ups_name));
    cfg->nut_port = CONFIG_NUT_TCP_PORT;
    cfg->poll_ms  = CONFIG_ECOFLOW_POLL_INTERVAL_MS;
    cfg->low_pct  = CONFIG_NUT_BATTERY_LOW_PCT;
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
