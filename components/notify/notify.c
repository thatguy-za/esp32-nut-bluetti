#include "notify.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "notify";

#define MSG_MAX     256
#define QUEUE_DEPTH 6
/* Ignore a repeat of the same event inside this window (mains flapping). */
#define REPEAT_GUARD_US (60 * 1000 * 1000LL)

typedef struct { char text[MSG_MAX]; } msg_t;

/* What we last told the user, so we only send on real transitions. */
typedef enum { PWR_UNKNOWN, PWR_LINE, PWR_BATTERY, PWR_OFFLINE } power_state_t;

static struct {
    notify_config_t   cfg;
    char              label[40];
    QueueHandle_t     q;
    SemaphoreHandle_t cfg_lock;
    bool              started;

    power_state_t     power;
    bool              low_batt;
    int64_t           last_event_us;
    power_state_t     last_event_state;
} N;

/* ------------------------------------------------------------------ */
/* Telegram transport                                                  */
/* ------------------------------------------------------------------ */

static int telegram_post(const notify_config_t *cfg, const char *text,
                         char *err, size_t err_sz)
{
    if (!cfg->bot_token[0] || !cfg->chat_id[0]) {
        if (err) snprintf(err, err_sz, "bot token and chat id are required");
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        if (err) snprintf(err, err_sz, "out of memory");
        return -1;
    }
    cJSON_AddStringToObject(root, "chat_id", cfg->chat_id);
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddBoolToObject(root, "disable_notification", false);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        if (err) snprintf(err, err_sz, "json build failed");
        return -1;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage",
             cfg->bot_token);

    esp_http_client_config_t hc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) {
        free(body);
        if (err) snprintf(err, err_sz, "http init failed");
        return -1;
    }
    esp_http_client_set_header(cli, "Content-Type", "application/json");

    int rc = esp_http_client_open(cli, strlen(body));
    if (rc != ESP_OK) {
        esp_http_client_cleanup(cli);
        free(body);
        if (err) snprintf(err, err_sz, "connect failed: %s", esp_err_to_name(rc));
        return -1;
    }
    esp_http_client_write(cli, body, strlen(body));
    free(body);

    esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);

    /* Telegram explains failures in the body; keep enough for the UI. */
    char resp[192] = "";
    int n = esp_http_client_read(cli, resp, sizeof(resp) - 1);
    if (n > 0) {
        resp[n] = '\0';
    }
    esp_http_client_cleanup(cli);

    if (status == 200) {
        return 0;
    }
    if (err) {
        const char *why = strstr(resp, "\"description\":\"");
        if (why) {
            why += 15;
            size_t k = 0;
            while (why[k] && why[k] != '"' && k < err_sz - 1) k++;
            snprintf(err, err_sz, "%.*s", (int)k, why);
        } else {
            snprintf(err, err_sz, "Telegram HTTP %d", status);
        }
    }
    ESP_LOGW(TAG, "sendMessage failed: HTTP %d", status);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

static void worker(void *arg)
{
    msg_t m;
    for (;;) {
        if (xQueueReceive(N.q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        notify_config_t cfg;
        xSemaphoreTake(N.cfg_lock, portMAX_DELAY);
        cfg = N.cfg;
        xSemaphoreGive(N.cfg_lock);

        if (!cfg.enabled) {
            continue;
        }
        char err[96] = "";
        if (telegram_post(&cfg, m.text, err, sizeof(err)) != 0) {
            ESP_LOGW(TAG, "notification not delivered: %s", err);
        } else {
            ESP_LOGI(TAG, "notified: %s", m.text);
        }
    }
}

bool notify_send(const char *text)
{
    if (!N.started || !text) {
        return false;
    }
    xSemaphoreTake(N.cfg_lock, portMAX_DELAY);
    bool on = N.cfg.enabled;
    xSemaphoreGive(N.cfg_lock);
    if (!on) {
        return false;
    }

    msg_t m;
    snprintf(m.text, sizeof(m.text), "%s%s%s",
             N.label[0] ? N.label : "", N.label[0] ? ": " : "", text);
    /* Never block a caller: drop rather than stall the BLE/NUT path. */
    if (xQueueSend(N.q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping: %s", text);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Event detection                                                     */
/* ------------------------------------------------------------------ */

static bool guard_allows(power_state_t s)
{
    int64_t now = esp_timer_get_time();
    if (s == N.last_event_state && (now - N.last_event_us) < REPEAT_GUARD_US) {
        return false;
    }
    N.last_event_state = s;
    N.last_event_us = now;
    return true;
}

void notify_ups_status(const char *status, int soc_pct, int runtime_min)
{
    if (!N.started || !status) {
        return;
    }
    xSemaphoreTake(N.cfg_lock, portMAX_DELAY);
    notify_config_t cfg = N.cfg;
    xSemaphoreGive(N.cfg_lock);
    if (!cfg.enabled) {
        return;
    }

    power_state_t now;
    if (strncmp(status, "OFF", 3) == 0) {
        now = PWR_OFFLINE;
    } else if (strstr(status, "OB")) {
        now = PWR_BATTERY;
    } else if (strstr(status, "OL")) {
        now = PWR_LINE;
    } else {
        return;                       /* nothing meaningful to report */
    }

    bool low = strstr(status, "LB") != NULL;
    power_state_t was = N.power;
    char text[MSG_MAX];

    if (now != was && was != PWR_UNKNOWN && guard_allows(now)) {
        switch (now) {
        case PWR_BATTERY:
            if (cfg.on_power) {
                if (runtime_min > 0) {
                    snprintf(text, sizeof(text),
                             "\xE2\x9A\xA1 Mains lost — running on battery. "
                             "%d%% charge, about %d min left.", soc_pct, runtime_min);
                } else {
                    snprintf(text, sizeof(text),
                             "\xE2\x9A\xA1 Mains lost — running on battery. "
                             "%d%% charge.", soc_pct);
                }
                notify_send(text);
            }
            break;
        case PWR_LINE:
            if (cfg.on_power) {
                snprintf(text, sizeof(text),
                         "\xE2\x9C\x85 Mains restored. %d%% charge.", soc_pct);
                notify_send(text);
            }
            break;
        case PWR_OFFLINE:
            if (cfg.on_link) {
                notify_send("\xE2\x9A\xA0\xEF\xB8\x8F Lost contact with the "
                            "EcoFlow unit — UPS status is unknown.");
            }
            break;
        default:
            break;
        }
    } else if (was == PWR_OFFLINE && now != PWR_OFFLINE && cfg.on_link &&
               guard_allows(now)) {
        notify_send("\xE2\x9C\x85 EcoFlow unit is back.");
    }

    /* Low battery is its own edge, independent of the mains transition. */
    if (low && !N.low_batt && cfg.on_low_batt) {
        if (runtime_min > 0) {
            snprintf(text, sizeof(text),
                     "\xF0\x9F\x94\xB4 Battery low: %d%%, about %d min left. "
                     "Shut things down.", soc_pct, runtime_min);
        } else {
            snprintf(text, sizeof(text),
                     "\xF0\x9F\x94\xB4 Battery low: %d%%. Shut things down.",
                     soc_pct);
        }
        notify_send(text);
    }
    if (!low && N.low_batt && cfg.on_low_batt && now == PWR_LINE) {
        notify_send("\xF0\x9F\x94\x8B Battery no longer low.");
    }

    N.power = now;
    N.low_batt = low;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void notify_reconfigure(const notify_config_t *cfg)
{
    if (!N.cfg_lock || !cfg) {
        return;
    }
    xSemaphoreTake(N.cfg_lock, portMAX_DELAY);
    N.cfg = *cfg;
    xSemaphoreGive(N.cfg_lock);
    ESP_LOGI(TAG, "notifications %s", cfg->enabled ? "enabled" : "disabled");
}

int notify_send_test(const notify_config_t *cfg, const char *label,
                     char *err, size_t err_sz)
{
    char text[MSG_MAX];
    snprintf(text, sizeof(text), "%s%sTest message — notifications are working.",
             label && label[0] ? label : "", label && label[0] ? ": " : "");
    return telegram_post(cfg, text, err, err_sz);
}

int notify_start(const notify_config_t *cfg, const char *label)
{
    if (N.started) {
        notify_reconfigure(cfg);
        return 0;
    }
    N.cfg_lock = xSemaphoreCreateMutex();
    N.q = xQueueCreate(QUEUE_DEPTH, sizeof(msg_t));
    if (!N.cfg_lock || !N.q) {
        return -1;
    }
    N.cfg = *cfg;
    strlcpy(N.label, label ? label : "", sizeof(N.label));
    N.power = PWR_UNKNOWN;
    N.low_batt = false;
    N.last_event_state = PWR_UNKNOWN;

    /* TLS needs a roomy stack. */
    if (xTaskCreate(worker, "notify", 6144, NULL, 4, NULL) != pdPASS) {
        return -1;
    }
    N.started = true;
    ESP_LOGI(TAG, "notifier ready (%s)", cfg->enabled ? "enabled" : "disabled");
    return 0;
}
