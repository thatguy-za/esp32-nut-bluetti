/*
 * Captive-portal provisioning (setup mode) and a small admin server
 * (normal mode) for the EcoFlow NUT bridge.
 */

#include "provisioning.h"
#include "dns_server.h"
#include "log_ring.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#include "wifi_mgr.h"
#include "ecoflow_ble.h"

static const char *TAG = "provisioning";

extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[]   asm("_binary_portal_html_end");
extern const char admin_html_start[]  asm("_binary_admin_html_start");
extern const char admin_html_end[]    asm("_binary_admin_html_end");

#define AP_IP "192.168.4.1"

/* ---- shared state ------------------------------------------------- */

typedef enum { ST_IDLE, ST_CONNECTING, ST_CONNECTED, ST_FAILED } prov_state_t;

static struct {
    httpd_handle_t     httpd;
    app_config_t      *cfg;          /* live config to update on success */
    app_config_t       pending;      /* candidate from the form          */
    volatile prov_state_t state;
    char               detail[96];
    EventGroupHandle_t  done;        /* BIT0 set when provisioning done   */

    /* transient EcoFlow login (never persisted) */
    char               ef_email[80];
    char               ef_pass[80];
    char               ef_region[16];

    /* BLE scan results, filled from the BLE host task */
    SemaphoreHandle_t   ble_lock;
    ecoflow_scan_entry_t ble[24];
    int                 ble_n;
} P;

#define DONE_BIT BIT0

/* ---- tiny helpers ----------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* URL-decode src into dst (dst may equal src). */
static void url_decode(char *dst, const char *src, size_t dstsz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dstsz; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && hexval(src[i + 1]) >= 0 &&
                   hexval(src[i + 2]) >= 0) {
            dst[o++] = (char)(hexval(src[i + 1]) * 16 + hexval(src[i + 2]));
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/* Pull one field out of an application/x-www-form-urlencoded body. */
static bool form_get(const char *body, const char *key, char *out, size_t outsz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *eq = strchr(p, '=');
        if (eq && (!amp || eq < amp) && (size_t)(eq - p) == klen &&
            strncmp(p, key, klen) == 0) {
            const char *vstart = eq + 1;
            size_t vlen = amp ? (size_t)(amp - vstart) : strlen(vstart);
            char tmp[256];
            if (vlen >= sizeof(tmp)) vlen = sizeof(tmp) - 1;
            memcpy(tmp, vstart, vlen);
            tmp[vlen] = '\0';
            url_decode(out, tmp, outsz);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    if (outsz) out[0] = '\0';
    return false;
}

static esp_err_t send_json(httpd_req_t *r, const char *json)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, json);
}

/* ---- BLE scan glue -------------------------------------------------- */

static void ble_scan_cb(const ecoflow_scan_entry_t *e, void *user)
{
    xSemaphoreTake(P.ble_lock, portMAX_DELAY);
    if (P.ble_n < (int)(sizeof(P.ble) / sizeof(P.ble[0]))) {
        P.ble[P.ble_n++] = *e;
    }
    xSemaphoreGive(P.ble_lock);
}

static esp_err_t h_ble_scan(httpd_req_t *r)
{
    /* A bare GET (re)starts a scan; "?poll=1" only reports progress. */
    char q[16] = "";
    httpd_req_get_url_query_str(r, q, sizeof(q));
    bool poll_only = strstr(q, "poll=1") != NULL;

    if (!poll_only && !ecoflow_ble_scanning()) {
        xSemaphoreTake(P.ble_lock, portMAX_DELAY);
        P.ble_n = 0;
        xSemaphoreGive(P.ble_lock);
        ecoflow_ble_scan(7000, ble_scan_cb, NULL);
    }

    char *buf = malloc(2048);
    if (!buf) return httpd_resp_send_500(r);
    int o = snprintf(buf, 2048, "{\"scanning\":%s,\"devices\":[",
                     ecoflow_ble_scanning() ? "true" : "false");

    xSemaphoreTake(P.ble_lock, portMAX_DELAY);
    for (int i = 0; i < P.ble_n && o < 1900; i++) {
        o += snprintf(buf + o, 2048 - o,
                      "%s{\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"ecoflow\":%s}",
                      i ? "," : "", P.ble[i].addr, P.ble[i].name,
                      P.ble[i].rssi, P.ble[i].looks_like_ecoflow ? "true" : "false");
    }
    xSemaphoreGive(P.ble_lock);

    snprintf(buf + o, 2048 - o, "]}");
    esp_err_t rc = send_json(r, buf);
    free(buf);
    return rc;
}

/* ---- Wi-Fi scan --------------------------------------------------- */

static esp_err_t h_wifi_scan(httpd_req_t *r)
{
    wifi_scan_entry_t aps[20];
    int n = wifi_mgr_scan(aps, 20);
    if (n < 0) n = 0;

    char *buf = malloc(2048);
    if (!buf) return httpd_resp_send_500(r);
    int o = snprintf(buf, 2048, "{\"aps\":[");
    for (int i = 0; i < n && o < 1900; i++) {
        o += snprintf(buf + o, 2048 - o,
                      "%s{\"ssid\":\"%s\",\"rssi\":%d,\"lock\":%s}",
                      i ? "," : "", aps[i].ssid, aps[i].rssi,
                      aps[i].authmode == 0 ? "false" : "true");
    }
    snprintf(buf + o, 2048 - o, "]}");
    esp_err_t rc = send_json(r, buf);
    free(buf);
    return rc;
}

/* ---- provision --------------------------------------------------- */

static void connect_task(void *arg)
{
    P.state = ST_CONNECTING;
    esp_err_t rc = wifi_mgr_sta_connect(P.pending.wifi_ssid,
                                        P.pending.wifi_pass, 25000);
    if (rc != ESP_OK) {
        strlcpy(P.detail,
                rc == ESP_ERR_TIMEOUT ? "Wi-Fi timed out"
                                      : "Wi-Fi association failed",
                sizeof(P.detail));
        P.state = ST_FAILED;
        ESP_LOGW(TAG, "provision connect failed: %s", esp_err_to_name(rc));
        vTaskDelete(NULL);
        return;
    }

    /* Resolve the EcoFlow user id from a login if one wasn't pasted. */
    if (P.pending.ef_user_id[0] == '\0' && P.ef_email[0] && P.ef_pass[0]) {
        char err[80] = "";
        int lr = ecoflow_resolve_user_id(P.ef_email, P.ef_pass, P.ef_region,
                                         P.pending.ef_user_id,
                                         sizeof(P.pending.ef_user_id),
                                         err, sizeof(err));
        memset(P.ef_pass, 0, sizeof(P.ef_pass));
        if (lr != 0) {
            snprintf(P.detail, sizeof(P.detail), "EcoFlow login: %s", err);
            P.state = ST_FAILED;
            vTaskDelete(NULL);
            return;
        }
    }
    memset(P.ef_pass, 0, sizeof(P.ef_pass));

    *P.cfg = P.pending;
    P.cfg->provisioned = true;
    app_config_save(P.cfg);
    P.state = ST_CONNECTED;
    ESP_LOGI(TAG, "provisioned; STA connected; user_id %s",
             P.cfg->ef_user_id[0] ? "set" : "MISSING");
    xEventGroupSetBits(P.done, DONE_BIT);
    vTaskDelete(NULL);
}

static esp_err_t h_provision(httpd_req_t *r)
{
    int len = r->content_len;
    if (len <= 0 || len > 2048) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body");
    }
    char *body = malloc(len + 1);
    if (!body) return httpd_resp_send_500(r);
    int got = 0;
    while (got < len) {
        int k = httpd_req_recv(r, body + got, len - got);
        if (k <= 0) { free(body); return httpd_resp_send_500(r); }
        got += k;
    }
    body[len] = '\0';

    P.pending = *P.cfg;
    char v[128];
    form_get(body, "ssid", P.pending.wifi_ssid, sizeof(P.pending.wifi_ssid));
    form_get(body, "pass", P.pending.wifi_pass, sizeof(P.pending.wifi_pass));

    if (form_get(body, "ble_addr", v, sizeof(v)) && v[0]) {
        strlcpy(P.pending.ble_addr, v, sizeof(P.pending.ble_addr));
    }
    if (form_get(body, "ble_name", v, sizeof(v)) && v[0]) {
        strlcpy(P.pending.ble_name, v, sizeof(P.pending.ble_name));
    }
    if (form_get(body, "ef_user_id", v, sizeof(v)) && v[0]) {
        strlcpy(P.pending.ef_user_id, v, sizeof(P.pending.ef_user_id));
    }
    form_get(body, "ef_email", P.ef_email, sizeof(P.ef_email));
    form_get(body, "ef_pass", P.ef_pass, sizeof(P.ef_pass));
    if (!form_get(body, "ef_region", P.ef_region, sizeof(P.ef_region)) ||
        !P.ef_region[0]) {
        strlcpy(P.ef_region, "api", sizeof(P.ef_region));
    }
    if (form_get(body, "ups_name", v, sizeof(v)) && v[0]) {
        strlcpy(P.pending.ups_name, v, sizeof(P.pending.ups_name));
    }
    if (form_get(body, "nut_port", v, sizeof(v)) && atoi(v) > 0) {
        P.pending.nut_port = (uint16_t)atoi(v);
    }
    if (form_get(body, "low_pct", v, sizeof(v)) && atoi(v) > 0) {
        P.pending.low_pct = (uint8_t)atoi(v);
    }
    free(body);

    if (P.pending.wifi_ssid[0] == '\0') {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "ssid required");
    }

    P.state = ST_CONNECTING;
    P.detail[0] = '\0';
    xTaskCreate(connect_task, "prov_connect", 4096, NULL, 5, NULL);
    return send_json(r, "{\"ok\":true}");
}

static esp_err_t h_status(httpd_req_t *r)
{
    const char *s = P.state == ST_CONNECTED  ? "connected"
                  : P.state == ST_CONNECTING ? "connecting"
                  : P.state == ST_FAILED     ? "failed" : "idle";
    char ip[16];
    wifi_mgr_sta_ip(ip, sizeof(ip));
    char out[192];
    snprintf(out, sizeof(out),
             "{\"state\":\"%s\",\"ip\":\"%s\",\"detail\":\"%s\"}",
             s, ip, P.detail);
    return send_json(r, out);
}

/* ---- portal page + captive redirect ------------------------------ */

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, portal_html_start,
                           portal_html_end - portal_html_start - 1);
}

static esp_err_t h_redirect(httpd_req_t *r)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://" AP_IP "/");
    return httpd_resp_send(r, NULL, 0);
}

static esp_err_t captive_404(httpd_req_t *r, httpd_err_code_t err)
{
    return h_redirect(r);
}

/* ---- server plumbing ------------------------------------------------ */

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m,
                esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = m, .handler = fn };
    httpd_register_uri_handler(s, &u);
}

static esp_err_t h_admin_root(httpd_req_t *r);
static esp_err_t h_admin_status(httpd_req_t *r);
static esp_err_t h_admin_logs(httpd_req_t *r);
static esp_err_t h_factory_reset(httpd_req_t *r);
#if CONFIG_ENABLE_WEB_OTA
static esp_err_t h_ota(httpd_req_t *r);
#endif

static esp_err_t start_httpd(bool captive)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = 80;
    c.lru_purge_enable = true;
    c.max_uri_handlers = 16;
    c.stack_size = 8192;
    if (captive) {
        c.uri_match_fn = httpd_uri_match_wildcard;
    } else {
        c.recv_wait_timeout = 20;   /* tolerate a slow firmware upload */
        c.send_wait_timeout = 20;
    }
    esp_err_t rc = httpd_start(&P.httpd, &c);
    if (rc != ESP_OK) {
        return rc;
    }

    if (captive) {
        reg(P.httpd, "/", HTTP_GET, h_root);
        reg(P.httpd, "/api/wifi-scan", HTTP_GET, h_wifi_scan);
        reg(P.httpd, "/api/ble-scan", HTTP_GET, h_ble_scan);
        reg(P.httpd, "/api/provision", HTTP_POST, h_provision);
        reg(P.httpd, "/api/status", HTTP_GET, h_status);
        /* OS connectivity-check endpoints -> bounce to the portal */
        reg(P.httpd, "/generate_204", HTTP_GET, h_redirect);
        reg(P.httpd, "/gen_204", HTTP_GET, h_redirect);
        reg(P.httpd, "/hotspot-detect.html", HTTP_GET, h_redirect);
        reg(P.httpd, "/library/test/success.html", HTTP_GET, h_redirect);
        reg(P.httpd, "/ncsi.txt", HTTP_GET, h_redirect);
        reg(P.httpd, "/connecttest.txt", HTTP_GET, h_redirect);
        reg(P.httpd, "/*", HTTP_GET, h_redirect);
        httpd_register_err_handler(P.httpd, HTTPD_404_NOT_FOUND, captive_404);
    } else {
        reg(P.httpd, "/", HTTP_GET, h_admin_root);
        reg(P.httpd, "/api/status", HTTP_GET, h_admin_status);
        reg(P.httpd, "/api/logs", HTTP_GET, h_admin_logs);
        reg(P.httpd, "/api/factory-reset", HTTP_POST, h_factory_reset);
#if CONFIG_ENABLE_WEB_OTA
        reg(P.httpd, "/api/ota", HTTP_POST, h_ota);
#endif
    }
    return ESP_OK;
}

/* ---- public: setup mode ------------------------------------------- */

static void make_ap_ssid(char *out, size_t n)
{
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);   /* STA is always up by now */
    snprintf(out, n, "ecoflow-setup-%02X%02X", mac[4], mac[5]);
}

esp_err_t provisioning_run(app_config_t *cfg)
{
    P.cfg = cfg;
    P.state = ST_IDLE;
    P.done = xEventGroupCreate();
    P.ble_lock = xSemaphoreCreateMutex();
    if (!P.done || !P.ble_lock) {
        return ESP_ERR_NO_MEM;
    }

    char ssid[32];
    make_ap_ssid(ssid, sizeof(ssid));
    ESP_ERROR_CHECK(wifi_mgr_ap_start(ssid));
    dns_server_start(AP_IP);

    esp_err_t rc = start_httpd(true);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(rc));
        return rc;
    }

    ESP_LOGI(TAG, "setup portal ready: join Wi-Fi '%s', browse to http://%s/",
             ssid, AP_IP);

    xEventGroupWaitBits(P.done, DONE_BIT, pdTRUE, pdTRUE, portMAX_DELAY);

    /* Give the browser a moment to fetch the final status, then tear down. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    ecoflow_ble_scan_stop();
    httpd_stop(P.httpd);
    P.httpd = NULL;
    dns_server_stop();
    wifi_mgr_ap_stop();
    ESP_LOGI(TAG, "setup complete, SoftAP down");
    return ESP_OK;
}

/* ---- public: normal-mode admin server ---------------------------- */

static esp_err_t h_admin_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, admin_html_start,
                           admin_html_end - admin_html_start - 1);
}

static esp_err_t h_admin_status(httpd_req_t *r)
{
    char ip[16];
    wifi_mgr_sta_ip(ip, sizeof(ip));
    ecoflow_state_t st;
    bool have = ecoflow_ble_get_state(&st);
    const esp_app_desc_t *app = esp_app_get_description();
#if CONFIG_ENABLE_WEB_OTA
    const bool ota = true;
#else
    const bool ota = false;
#endif
    char out[420];
    snprintf(out, sizeof(out),
             "{\"ip\":\"%s\",\"ups\":\"%s\",\"nut_port\":%u,"
             "\"fw_version\":\"%s\",\"fw_date\":\"%s %s\",\"ota\":%s,"
             "\"ble_target\":\"%s\",\"ble_connected\":%s,"
             "\"telemetry_valid\":%s,\"battery_pct\":%d,"
             "\"ac_input\":%s,\"charging\":%s,\"model\":\"%s\"}",
             ip, P.cfg->ups_name, P.cfg->nut_port,
             app->version, app->date, app->time, ota ? "true" : "false",
             P.cfg->ble_addr[0] ? P.cfg->ble_addr : P.cfg->ble_name,
             ecoflow_ble_connected() ? "true" : "false",
             have && st.valid ? "true" : "false",
             have ? st.soc_pct : 0,
             have && st.ac_input_present ? "true" : "false",
             have && st.charging ? "true" : "false",
             have && st.model[0] ? st.model : "");
    return send_json(r, out);
}

static esp_err_t h_admin_logs(httpd_req_t *r)
{
    char q[40] = "";
    httpd_req_get_url_query_str(r, q, sizeof(q));
    uint64_t from = 0;
    const char *p = strstr(q, "from=");
    if (p) {
        from = strtoull(p + 5, NULL, 10);
    }

    char *buf = malloc(4096);
    if (!buf) {
        return httpd_resp_send_500(r);
    }
    uint64_t next = from;
    size_t n = log_ring_read(from, buf, 4096, &next);

    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%llu", (unsigned long long)next);
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "X-Log-Next", hdr);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t rc = httpd_resp_send(r, buf, n);
    free(buf);
    return rc;
}

static esp_err_t h_factory_reset(httpd_req_t *r)
{
    httpd_resp_sendstr(r, "{\"ok\":true}");
    ESP_LOGW(TAG, "factory reset requested via web");
    app_config_erase();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

#if CONFIG_ENABLE_WEB_OTA
static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

/* POST /api/ota  — body is a raw app image; header X-Confirm: FLASH */
static esp_err_t h_ota(httpd_req_t *r)
{
    char confirm[16] = "";
    httpd_req_get_hdr_value_str(r, "X-Confirm", confirm, sizeof(confirm));
    if (strcmp(confirm, "FLASH") != 0) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "confirm required");
    }
    if (r->content_len < 4096 || r->content_len > 4 * 1024 * 1024) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "implausible size");
    }

    const esp_partition_t *tgt = esp_ota_get_next_update_partition(NULL);
    if (!tgt) {
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no OTA partition");
    }
    ESP_LOGW(TAG, "OTA: %d bytes -> partition '%s'", r->content_len, tgt->label);

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(tgt, r->content_len, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "ota_begin failed");
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(h);
        return httpd_resp_send_500(r);
    }
    int remaining = r->content_len;
    bool ok = true;
    int stalls = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(r, buf, remaining < 4096 ? remaining : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++stalls > 6) {          /* ~2 min of no data => give up */
                ok = false;
                break;
            }
            continue;
        }
        stalls = 0;
        if (n <= 0) {
            ok = false;
            break;
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) {
            ok = false;
            break;
        }
        remaining -= n;
    }
    free(buf);

    if (!ok || remaining != 0) {
        esp_ota_abort(h);
        ESP_LOGE(TAG, "OTA transfer failed (%d left)", remaining);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "transfer failed");
    }
    err = esp_ota_end(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   err == ESP_ERR_OTA_VALIDATE_FAILED
                                       ? "image invalid" : "ota_end failed");
    }
    err = esp_ota_set_boot_partition(tgt);
    if (err != ESP_OK) {
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "set_boot failed");
    }

    ESP_LOGW(TAG, "OTA written; rebooting into '%s'", tgt->label);
    send_json(r, "{\"ok\":true}");
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}
#endif /* CONFIG_ENABLE_WEB_OTA */

esp_err_t provisioning_admin_start(const app_config_t *cfg)
{
    P.cfg = (app_config_t *)cfg;
    esp_err_t rc = start_httpd(false);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "admin server on :80");
    }
    return rc;
}
