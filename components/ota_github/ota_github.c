#include "ota_github.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"

static const char *TAG = "ota_gh";

#define GH_OWNER CONFIG_OTA_GITHUB_OWNER
#define GH_REPO  CONFIG_OTA_GITHUB_REPO

/* GitHub rejects requests without one. */
#define UA "esp32-nut-bluetti"

/* ------------------------------------------------------------------ */
/* Version comparison                                                  */
/* ------------------------------------------------------------------ */

int ota_gh_vercmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        long x = strtol(a, (char **)&a, 10);
        long y = strtol(b, (char **)&b, 10);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

static void running_version(char *out, size_t len)
{
    const esp_app_desc_t *d = esp_app_get_description();
    strlcpy(out, d ? d->version : "0.0.0", len);
    /* Tags carry a leading v; esp_app_desc does not, but be tolerant. */
    if (out[0] == 'v' || out[0] == 'V') {
        memmove(out, out + 1, strlen(out));
    }
}

/* ------------------------------------------------------------------ */
/* Listing releases                                                    */
/* ------------------------------------------------------------------ */

/*
 * The releases response runs to tens of kilobytes and only a handful of
 * bytes matter, so it is scanned as it streams rather than buffered and
 * parsed. A rolling match against the literal "tag_name":" survives a
 * token split across two reads, which a naive per-chunk strstr would not.
 */
typedef struct {
    const char *needle;
    size_t      matched;      /* how much of needle has been seen */
    bool        capturing;
    char        val[16];
    size_t      val_len;
    ota_gh_release_t *out;
    size_t      max;
    size_t      n;
    char        running[16];
} scan_t;

static void scan_byte(scan_t *s, char c)
{
    if (s->capturing) {
        if (c == '"') {
            s->val[s->val_len] = '\0';
            if (s->n < s->max) {
                const char *v = s->val;
                if (*v == 'v' || *v == 'V') {
                    v++;
                }
                strlcpy(s->out[s->n].version, v, sizeof(s->out[s->n].version));
                s->out[s->n].prerelease = false;
                s->out[s->n].newer =
                    ota_gh_vercmp(v, s->running) > 0;
                s->n++;
            }
            s->capturing = false;
            s->val_len = 0;
        } else if (s->val_len + 1 < sizeof(s->val)) {
            s->val[s->val_len++] = c;
        }
        return;
    }
    if (c == s->needle[s->matched]) {
        s->matched++;
        if (s->needle[s->matched] == '\0') {
            s->capturing = true;
            s->val_len = 0;
            s->matched = 0;
        }
    } else {
        /* Restart, allowing this byte to begin a fresh match. */
        s->matched = (c == s->needle[0]) ? 1 : 0;
    }
}

int ota_gh_check(ota_gh_release_t *out, size_t max, char *err, size_t errlen)
{
    char url[160];
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/%s/releases?per_page=%d",
             GH_OWNER, GH_REPO, OTA_GH_MAX_RELEASES);

    esp_http_client_config_t hc = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) {
        strlcpy(err, "could not start the HTTPS client", errlen);
        return -1;
    }
    esp_http_client_set_header(cli, "User-Agent", UA);
    esp_http_client_set_header(cli, "Accept", "application/vnd.github+json");

    int rc = esp_http_client_open(cli, 0);
    if (rc != ESP_OK) {
        snprintf(err, errlen, "could not reach github.com (%s)",
                 esp_err_to_name(rc));
        esp_http_client_cleanup(cli);
        return -1;
    }
    esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        /* 403 here is nearly always the unauthenticated rate limit. */
        if (status == 403 || status == 429) {
            strlcpy(err, "GitHub is rate-limiting this network — try later",
                    errlen);
        } else {
            snprintf(err, errlen, "GitHub returned HTTP %d", status);
        }
        esp_http_client_cleanup(cli);
        return -1;
    }

    scan_t s = { .needle = "\"tag_name\":\"", .out = out, .max = max };
    running_version(s.running, sizeof(s.running));

    char buf[512];
    int n;
    while ((n = esp_http_client_read(cli, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            scan_byte(&s, buf[i]);
        }
        if (s.n >= max) {
            break;      /* seen enough; no need to drain the rest */
        }
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (s.n == 0) {
        strlcpy(err, "no releases found", errlen);
        return -1;
    }
    ESP_LOGI(TAG, "found %u releases (running %s)", (unsigned)s.n, s.running);
    return (int)s.n;
}

/* ------------------------------------------------------------------ */
/* Installing                                                          */
/* ------------------------------------------------------------------ */

static struct {
    ota_gh_state_t state;
    int            pct;
    char           msg[96];
    char           version[16];
    SemaphoreHandle_t lock;
} G = { .state = OTA_GH_IDLE, .pct = -1 };

static void set_fail(const char *why)
{
    xSemaphoreTake(G.lock, portMAX_DELAY);
    G.state = OTA_GH_FAILED;
    strlcpy(G.msg, why, sizeof(G.msg));
    xSemaphoreGive(G.lock);
    ESP_LOGE(TAG, "update failed: %s", why);
}

static void ota_task(void *arg)
{
    char url[220];
    snprintf(url, sizeof(url),
             "https://github.com/%s/%s/releases/download/v%s/%s-%s.bin",
             GH_OWNER, GH_REPO, G.version, GH_REPO, G.version);
    ESP_LOGI(TAG, "downloading %s", url);

    esp_http_client_config_t hc = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t oc = { .http_config = &hc };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&oc, &h);
    if (err != ESP_OK || !h) {
        set_fail("could not start the download");
        vTaskDelete(NULL);
        return;
    }

    int total = esp_https_ota_get_image_size(h);
    while ((err = esp_https_ota_perform(h)) ==
           ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(h);
        xSemaphoreTake(G.lock, portMAX_DELAY);
        G.pct = total > 0 ? (int)((int64_t)done * 100 / total) : -1;
        xSemaphoreGive(G.lock);
    }

    if (err != ESP_OK) {
        esp_https_ota_abort(h);
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            set_fail("that file is not a valid image for this device");
        } else {
            char m[96];
            snprintf(m, sizeof(m), "download failed (%s)",
                     esp_err_to_name(err));
            set_fail(m);
        }
        vTaskDelete(NULL);
        return;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        set_fail("the download ended early");
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        set_fail(err == ESP_ERR_OTA_VALIDATE_FAILED
                     ? "the downloaded image failed validation"
                     : "could not finish writing the image");
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(G.lock, portMAX_DELAY);
    G.state = OTA_GH_DONE;
    G.pct = 100;
    strlcpy(G.msg, "installed - rebooting", sizeof(G.msg));
    xSemaphoreGive(G.lock);
    ESP_LOGI(TAG, "update installed, rebooting");

    vTaskDelay(pdMS_TO_TICKS(1200));   /* let the page see 100% first */
    esp_restart();
}

int ota_gh_start(const char *version)
{
    if (!G.lock) {
        G.lock = xSemaphoreCreateMutex();
        if (!G.lock) {
            return -1;
        }
    }
    xSemaphoreTake(G.lock, portMAX_DELAY);
    if (G.state == OTA_GH_RUNNING) {
        xSemaphoreGive(G.lock);
        return -1;
    }
    strlcpy(G.version, version, sizeof(G.version));
    G.state = OTA_GH_RUNNING;
    G.pct = -1;
    strlcpy(G.msg, "starting", sizeof(G.msg));
    xSemaphoreGive(G.lock);

    /* TLS plus the OTA writer wants a generous stack. */
    if (xTaskCreate(ota_task, "gh_ota", 8192, NULL, 5, NULL) != pdPASS) {
        set_fail("could not start the update task");
        return -1;
    }
    return 0;
}

ota_gh_state_t ota_gh_status(int *pct, char *msg, size_t msglen)
{
    if (!G.lock) {
        if (pct) *pct = -1;
        if (msg && msglen) msg[0] = '\0';
        return OTA_GH_IDLE;
    }
    xSemaphoreTake(G.lock, portMAX_DELAY);
    ota_gh_state_t s = G.state;
    if (pct) *pct = G.pct;
    if (msg && msglen) strlcpy(msg, G.msg, msglen);
    xSemaphoreGive(G.lock);
    return s;
}
