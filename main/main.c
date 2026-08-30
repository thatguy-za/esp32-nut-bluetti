/*
 * esp32-nut-bluetti
 *
 * Boot flow:
 *   1. load config from NVS
 *   2. if BOOT/reset button held  -> wipe config, force setup
 *   3. bring up the BLE host
 *   4. if not provisioned (or stored Wi-Fi won't associate)
 *          -> run the captive-portal setup, which blocks until the user
 *             supplies working Wi-Fi credentials
 *      else -> connect STA with the stored credentials
 *   5. start NUT server + BLUETTI BLE client + admin web server
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"

#include "app_config.h"
#include "wifi_mgr.h"
#include "provisioning.h"
#include "log_ring.h"
#include "nut_server.h"
#include "bluetti_ble.h"
#include "notify.h"
#include "led_status.h"

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
/* BLUETTI state  ->  NUT variables                                    */
/* ------------------------------------------------------------------ */

static void publish_nut_from_bluetti(const bluetti_state_t *st)
{
    if (!st->valid) {
        return;
    }
    if (st->soc_pct >= 0) {
        nut_server_set_var_int("battery.charge", st->soc_pct);
    }
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
    if (st->battery_voltage > 0.0f) {
        nut_server_set_var_float("battery.voltage", st->battery_voltage, 2);
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
    if (st->ac_out_watts >= 0.0f) {
        nut_server_set_var_float("output.realpower", st->ac_out_watts, 0);
    }
    if (st->ac_in_volts >= 0.0f) {
        nut_server_set_var_float("input.voltage", st->ac_in_volts, 1);
    }
    if (st->ac_in_amps >= 0.0f) {
        nut_server_set_var_float("input.current", st->ac_in_amps, 1);
    }
    if (st->ac_out_volts >= 0.0f) {
        nut_server_set_var_float("output.voltage", st->ac_out_volts, 1);
    }
    /* The unit's two output banks, as NUT outlets. */
    if (st->ac_switch >= 0) {
        nut_server_set_var("outlet.1.desc", "AC");
        nut_server_set_var("outlet.1.status", st->ac_switch ? "on" : "off");
    }
    if (st->dc_switch >= 0) {
        nut_server_set_var("outlet.2.desc", "DC");
        nut_server_set_var("outlet.2.status", st->dc_switch ? "on" : "off");
    }
    /* ups.load is a percentage of the unit's continuous AC rating, which
     * varies by model, so it comes from the config rather than a guess. */
    if (st->output_watts >= 0.0f && s_cfg.ac_rating_w > 0) {
        int load = (int)(st->output_watts * 100.0f / (float)s_cfg.ac_rating_w);
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
        /* Low battery on either trigger. The percentage alone is a poor
         * guide under heavy load — 20% of a 245 Wh pack is minutes at
         * 300 W but hours at 20 W — so honour the runtime threshold we
         * publish as battery.runtime.low as well. */
        bool low_charge = st->soc_pct >= 0 && st->soc_pct <= st->soc_low_pct;
        bool low_runtime = s_cfg.runtime_low_s > 0 &&
                           st->minutes_remaining >= 0 &&
                           st->minutes_remaining * 60 <= (int)s_cfg.runtime_low_s;
        if (low_charge || low_runtime) {
            strlcat(status, " LB", sizeof(status));
        }
        strlcat(status, " DISCHRG", sizeof(status));
    }
    nut_server_set_status(status);

    /* Same string the NUT clients see, so notifications and UPS state can
     * never disagree. Edge detection and rate limiting live in notify. */
    notify_ups_status(status, st->soc_pct,
                      st->ac_input_present ? -1 : st->minutes_remaining);
}

static void bluetti_cb(const bluetti_state_t *state, void *user)
{
    /* First decoded frame means the BLE link is up and useful — turn the
     * LED green. staleness_task takes it back to red if the link drops. */
    led_status_set(LED_STATE_LINKED);
    publish_nut_from_bluetti(state);
}

static void staleness_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        bluetti_state_t st;
        bool have = bluetti_ble_get_state(&st);
        bool stale = !have ||
                     (esp_timer_get_time() - st.updated_us) > STALE_AFTER_US;
        led_status_set(bluetti_ble_connected() && !stale
                           ? LED_STATE_LINKED : LED_STATE_BOOT);
        if (stale) {
            const char *s = bluetti_ble_connected() ? "OL WAIT" : "OFF";
            nut_server_set_status(s);
            nut_server_set_var("driver.state",
                               bluetti_ble_connected() ? "connected-no-data"
                                                       : "disconnected");
            notify_ups_status(s, have ? st.soc_pct : 0, -1);
        } else {
            nut_server_set_var("driver.state", "updated");
        }
    }
}

/* ------------------------------------------------------------------ */

/* Called from a NUT client task when a client sends LOGIN / PRIMARY. */
static bool nut_verify_login(const char *user, const char *pass, void *ctx)
{
    const app_config_t *cfg = ctx;
    if (!user || !pass || !user[0]) {
        return false;
    }
    return strcmp(user, cfg->nut_user) == 0 &&
           app_config_check_nut_password(cfg, pass);
}

static void start_services(const app_config_t *cfg)
{
    /* NUT login. Standard NUT: this gates LOGIN/PRIMARY only — reads stay
     * anonymous so `upsc` keeps working. */
    if (cfg->nut_auth_set) {
        nut_server_set_auth(nut_verify_login, &s_cfg);
        ESP_LOGI(TAG, "NUT login required for LOGIN/PRIMARY (user '%s')",
                 cfg->nut_user);
    }

    nut_server_config_t nut_cfg = {
        .ups_name = cfg->ups_name,
        .ups_desc = "BLUETTI via ESP32",
        .tcp_port = cfg->nut_port,
        .max_clients = 4,
    };
    if (nut_server_start(&nut_cfg) != 0) {
        ESP_LOGE(TAG, "nut_server_start failed");
    }

    /* Fixed for this configuration, so publish once. */
    if (cfg->ac_rating_w > 0) {
        nut_server_set_var_int("ups.realpower.nominal", cfg->ac_rating_w);
    }
    if (cfg->runtime_low_s > 0) {
        nut_server_set_var_int("battery.runtime.low", cfg->runtime_low_s);
    }

    /* The BLUETTI side is configured from the admin page after Wi-Fi setup,
     * so on a fresh device there is nothing to connect to yet. */
    bool have_target = cfg->ble_addr[0] != '\0';
    if (!have_target) {
        ESP_LOGW(TAG, "BLUETTI not configured yet — open the admin page's "
                      "BLUETTI tab to pick your unit");
    } else {
        bluetti_ble_config_t ef_cfg = {
            .ble_address = cfg->ble_addr,
            .probe = cfg->ble_probe,
            .poll_interval_ms = cfg->poll_ms,
            .low_battery_pct = cfg->low_pct,
        };
        if (bluetti_ble_start(&ef_cfg, bluetti_cb, NULL) != 0) {
            ESP_LOGE(TAG, "bluetti_ble_start failed");
        }
    }

    notify_config_t ncfg = {
        .enabled = cfg->tg_enabled,
        .on_power = cfg->tg_on_power,
        .on_low_batt = cfg->tg_on_low_batt,
        .on_link = cfg->tg_on_link,
    };
    strlcpy(ncfg.bot_token, cfg->tg_token, sizeof(ncfg.bot_token));
    strlcpy(ncfg.chat_id, cfg->tg_chat, sizeof(ncfg.chat_id));
    notify_start(&ncfg, cfg->ups_name);

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

    /* Up as early as possible so the red "booting" light is on for the
     * whole startup, not just the tail of it. */
    led_status_init(cfg->led_gpio, cfg->led_enabled);

    ESP_ERROR_CHECK(wifi_mgr_init());

    if (reset_button_held()) {
        ESP_LOGW(TAG, "wiping stored config");
        app_config_erase();
        app_config_defaults(cfg);
    }

    if (bluetti_ble_host_init() != 0) {
        ESP_LOGW(TAG, "BLE host init incomplete; continuing");
    }

    bool ap_mode = cfg->provisioned && cfg->wifi_mode == APP_WIFI_AP;
    bool need_setup = !cfg->provisioned ||
                      (!ap_mode && cfg->wifi_ssid[0] == '\0');

    if (ap_mode) {
        ESP_LOGI(TAG, "running as access point '%s'", cfg->ap_ssid);
        ESP_ERROR_CHECK(wifi_mgr_ap_start(cfg->ap_ssid, cfg->ap_pass));
    } else if (!need_setup) {
        wifi_mgr_ipv4_t ipv4 = {
            .hostname   = cfg->hostname,
            .use_static = cfg->use_static_ip,
            .ip         = cfg->static_ip,
            .mask       = cfg->static_mask,
            .gw         = cfg->static_gw,
            .dns        = cfg->static_dns,
        };
        wifi_mgr_set_ipv4(&ipv4);
        ESP_LOGI(TAG, "connecting to '%s'", cfg->wifi_ssid);
        if (wifi_mgr_sta_connect(cfg->wifi_ssid, cfg->wifi_pass, 30000) != ESP_OK) {
            ESP_LOGW(TAG, "stored Wi-Fi failed; falling back to setup");
            need_setup = true;
        }
    }

    if (need_setup) {
        ESP_ERROR_CHECK(provisioning_run(cfg));   /* blocks until connected */
        ap_mode = cfg->wifi_mode == APP_WIFI_AP;
    }

    /* BLUETTI's BLE auth doesn't need the clock, but the device asks for
     * time once connected; give it a real one. In AP mode there's no route
     * to an NTP server, so don't bother. */
    if (!ap_mode) {
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp);
    }

    char ip[16];
    if (ap_mode) {
        wifi_mgr_ap_ip(ip, sizeof(ip));
    } else {
        wifi_mgr_sta_ip(ip, sizeof(ip));
    }
    ESP_LOGI(TAG, "online at %s (%s) — NUT ':%u' UPS '%s', BLE target '%s'",
             ip, ap_mode ? "access point" : "station",
             cfg->nut_port, cfg->ups_name,
             cfg->ble_addr);

    start_services(cfg);

    /* We reached "online + services up" — if this build arrived via OTA and is
     * still on probation, confirm it so the bootloader keeps it. */
    esp_ota_img_states_t ota_state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA image confirmed valid");
    }
}
