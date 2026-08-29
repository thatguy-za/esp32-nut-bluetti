/*
 * esp32-nut-ecoflow
 *
 * Boot flow:
 *   1. load config from NVS
 *   2. if BOOT/reset button held  -> wipe config, force setup
 *   3. bring up the BLE host
 *   4. if not provisioned (or stored Wi-Fi won't associate)
 *          -> run the captive-portal setup, which blocks until the user
 *             supplies working Wi-Fi credentials
 *      else -> connect STA with the stored credentials
 *   5. start NUT server + EcoFlow BLE client + admin web server
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "driver/gpio.h"

#include "app_config.h"
#include "wifi_mgr.h"
#include "provisioning.h"
#include "log_ring.h"
#include "nut_server.h"
#include "ecoflow_ble.h"

static const char *TAG = "app";

#define STALE_AFTER_US (30 * 1000 * 1000LL)

/* Config outlives app_main() (its stack is reclaimed on return) because
 * the admin web server keeps a pointer to it. */
static app_config_t s_cfg;

/* ------------------------------------------------------------------ */
/* Reset button: held low through boot for CONFIG_RESET_HOLD_MS wipes  */
/* the stored config so the device returns to setup mode.             */
/* ------------------------------------------------------------------ */

static bool reset_button_held(void)
{
    const gpio_num_t pin = CONFIG_RESET_HOLD_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    if (gpio_get_level(pin) != 0) {
        return false;
    }
    ESP_LOGW(TAG, "reset button down; hold %d ms to wipe config",
             CONFIG_RESET_HOLD_MS);
    for (int waited = 0; waited < CONFIG_RESET_HOLD_MS; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(pin) != 0) {
            ESP_LOGI(TAG, "released early, keeping config");
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* EcoFlow state  ->  NUT variables                                    */
/* ------------------------------------------------------------------ */

static void publish_nut_from_ecoflow(const ecoflow_state_t *st)
{
    if (!st->valid) {
        return;
    }
    nut_server_set_var_int("battery.charge", st->soc_pct);
    nut_server_set_var_int("battery.charge.low", st->soc_low_pct);

    int rem = st->ac_input_present ? -1 : st->minutes_remaining;
    if (rem >= 0) {
        nut_server_set_var_int("battery.runtime", rem * 60);
    } else {
        nut_server_clear_var("battery.runtime");
    }
    if (st->battery_temp_c > -100.0f) {
        nut_server_set_var_float("battery.temperature", st->battery_temp_c, 1);
    }
    if (st->design_capacity_wh > 0) {
        nut_server_set_var_int("battery.capacity", st->design_capacity_wh);
    }
    if (st->output_watts >= 0.0f) {
        nut_server_set_var_float("ups.realpower", st->output_watts, 0);
    }
    if (st->input_watts >= 0.0f) {
        nut_server_set_var_float("input.realpower", st->input_watts, 0);
    }
    if (st->ac_in_watts >= 0.0f) {
        nut_server_set_var_float("input.realpower.ac", st->ac_in_watts, 0);
    }
    nut_server_set_var("ups.type", st->backup_mode_on ? "online" : "offline");
    if (st->output_watts >= 0.0f) {
        /* River 3 continuous AC rating is ~300 W. */
        int load = (int)(st->output_watts * 100.0f / 300.0f);
        nut_server_set_var_int("ups.load", load > 100 ? 100 : load);
    }
    if (st->error_code) {
        nut_server_set_var_int("ups.alarm", (int)st->error_code);
    } else {
        nut_server_clear_var("ups.alarm");
    }
    if (st->serial[0]) {
        nut_server_set_var("device.serial", st->serial);
        nut_server_set_var("ups.serial", st->serial);
    }
    if (st->model[0]) {
        nut_server_set_var("device.model", st->model);
        nut_server_set_var("ups.model", st->model);
    }

    char status[24];
    if (st->ac_input_present) {
        strlcpy(status, "OL", sizeof(status));
        if (st->charging) {
            strlcat(status, " CHRG", sizeof(status));
        }
    } else {
        strlcpy(status, "OB", sizeof(status));
        if (st->soc_pct <= st->soc_low_pct) {
            strlcat(status, " LB", sizeof(status));
        }
        strlcat(status, " DISCHRG", sizeof(status));
    }
    nut_server_set_status(status);
}

static void ecoflow_cb(const ecoflow_state_t *state, void *user)
{
    publish_nut_from_ecoflow(state);
}

static void staleness_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ecoflow_state_t st;
        bool have = ecoflow_ble_get_state(&st);
        bool stale = !have ||
                     (esp_timer_get_time() - st.updated_us) > STALE_AFTER_US;
        if (stale) {
            nut_server_set_status(ecoflow_ble_connected() ? "OL WAIT" : "OFF");
            nut_server_set_var("driver.state",
                               ecoflow_ble_connected() ? "connected-no-data"
                                                       : "disconnected");
        } else {
            nut_server_set_var("driver.state", "updated");
        }
    }
}

/* ------------------------------------------------------------------ */

static void start_services(const app_config_t *cfg)
{
    nut_server_config_t nut_cfg = {
        .ups_name = cfg->ups_name,
        .ups_desc = "EcoFlow via ESP32",
        .tcp_port = cfg->nut_port,
        .max_clients = 4,
    };
    if (nut_server_start(&nut_cfg) != 0) {
        ESP_LOGE(TAG, "nut_server_start failed");
    }

    ecoflow_ble_config_t ef_cfg = {
        .ble_address = cfg->ble_addr,
        .ble_name_prefix = cfg->ble_name,
        .user_id = cfg->ef_user_id,
        .poll_interval_ms = cfg->poll_ms,
        .low_battery_pct = cfg->low_pct,
    };
    if (cfg->ef_user_id[0] == '\0') {
        ESP_LOGW(TAG, "no EcoFlow user_id set — BLE auth will fail; re-provision");
    }
    if (ecoflow_ble_start(&ef_cfg, ecoflow_cb, NULL) != 0) {
        ESP_LOGE(TAG, "ecoflow_ble_start failed");
    }

    provisioning_admin_start(cfg);
    xTaskCreate(staleness_task, "staleness", 3072, NULL, 4, NULL);
}

void app_main(void)
{
    log_ring_init();   /* tee the log stream to the web UI, from here on */

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    app_config_t *cfg = &s_cfg;
    app_config_load(cfg);

    ESP_ERROR_CHECK(wifi_mgr_init());

    if (reset_button_held()) {
        ESP_LOGW(TAG, "wiping stored config");
        app_config_erase();
        app_config_defaults(cfg);
    }

    if (ecoflow_ble_host_init() != 0) {
        ESP_LOGW(TAG, "BLE host init incomplete; continuing");
    }

    bool need_setup = !cfg->provisioned || cfg->wifi_ssid[0] == '\0';

    if (!need_setup) {
        ESP_LOGI(TAG, "connecting to '%s'", cfg->wifi_ssid);
        if (wifi_mgr_sta_connect(cfg->wifi_ssid, cfg->wifi_pass, 30000) != ESP_OK) {
            ESP_LOGW(TAG, "stored Wi-Fi failed; falling back to setup");
            need_setup = true;
        }
    }

    if (need_setup) {
        ESP_ERROR_CHECK(provisioning_run(cfg));   /* blocks until connected */
    }

    /* EcoFlow's BLE auth doesn't need the clock, but the device asks for
     * time once connected; give it a real one. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);

    char ip[16];
    wifi_mgr_sta_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "online at %s — NUT ':%u' UPS '%s', BLE target '%s'",
             ip, cfg->nut_port, cfg->ups_name,
             cfg->ble_addr[0] ? cfg->ble_addr : cfg->ble_name);

    start_services(cfg);
}
