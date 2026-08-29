/*
 * Captive-portal provisioning (setup mode) and a small admin server
 * (normal mode) for the BLUETTI NUT bridge.
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
#include "mbedtls/base64.h"

#include "wifi_mgr.h"
#include "bluetti_ble.h"
#include "notify.h"

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

    /* BLE scan results, filled from the BLE host task */
    SemaphoreHandle_t   ble_lock;
    bluetti_scan_entry_t ble[24];
    int                 ble_n;
} P;

#define DONE_BIT BIT0

static void reboot_after_delay(void *arg);

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

/* Strict dotted-quad check: four 0..255 parts, no spaces, no leading '+'.
 * esp_ip4addr_aton() is lenient (accepts "1.2.3" and hex), which would let
 * a typo through and strand the device on an unreachable address. */
static bool valid_ipv4(const char *s)
{
    if (!s || !*s) {
        return false;
    }
    int parts = 0;
    for (;;) {
        if (*s < '0' || *s > '9') {
            return false;                    /* each part needs a digit */
        }
        int val = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s++ - '0');
            if (++digits > 3 || val > 255) {
                return false;
            }
        }
        parts++;
        if (*s == '\0') {
            break;
        }
        if (*s != '.' || parts == 4) {
            return false;
        }
        s++;
    }
    return parts == 4;
}

static esp_err_t send_json(httpd_req_t *r, const char *json)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, json);
}

/* ---- admin authentication (HTTP Basic) ---------------------------- *
 * There is no TLS here, so this keeps casual users off the admin page
 * rather than defending against someone sniffing the LAN. The password
 * itself is stored only as a salted SHA-256.                          */

static bool auth_ok(httpd_req_t *r)
{
    if (!P.cfg || !P.cfg->auth_set) {
        return true;                       /* not configured yet */
    }
    size_t len = httpd_req_get_hdr_value_len(r, "Authorization");
    if (len == 0 || len > 400) {
        return false;
    }
    char hdr[416];
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (strncasecmp(hdr, "Basic ", 6) != 0) {
        return false;
    }

    unsigned char dec[160];
    size_t dlen = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dlen,
                              (const unsigned char *)hdr + 6,
                              strlen(hdr + 6)) != 0) {
        return false;
    }
    dec[dlen] = '\0';

    char *colon = strchr((char *)dec, ':');
    if (!colon) {
        return false;
    }
    *colon = '\0';
    bool ok = strcmp((char *)dec, P.cfg->auth_user) == 0 &&
              app_config_check_password(P.cfg, colon + 1);
    memset(dec, 0, sizeof(dec));           /* don't leave it on the stack */
    return ok;
}

static esp_err_t send_401(httpd_req_t *r)
{
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_hdr(r, "WWW-Authenticate",
                       "Basic realm=\"BLUETTI NUT Bridge\", charset=\"UTF-8\"");
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_sendstr(r, "Authentication required");
}

/* Wrap a handler so it 401s unless the request carries valid credentials. */
#define REQUIRE_AUTH(req) do {            \
        if (!auth_ok(req)) {              \
            return send_401(req);         \
        }                                 \
    } while (0)

/* ---- BLE scan glue -------------------------------------------------- */

static void ble_scan_cb(const bluetti_scan_entry_t *e, void *user)
{
    xSemaphoreTake(P.ble_lock, portMAX_DELAY);
    if (P.ble_n < (int)(sizeof(P.ble) / sizeof(P.ble[0]))) {
        P.ble[P.ble_n++] = *e;
    }
    xSemaphoreGive(P.ble_lock);
}

static esp_err_t h_ble_scan(httpd_req_t *r)
{
    REQUIRE_AUTH(r);   /* no-op during setup: no password set yet */
    /* A bare GET (re)starts a scan; "?poll=1" only reports progress. */
    char q[16] = "";
    httpd_req_get_url_query_str(r, q, sizeof(q));
    bool poll_only = strstr(q, "poll=1") != NULL;

    if (!poll_only && !bluetti_ble_scanning()) {
        xSemaphoreTake(P.ble_lock, portMAX_DELAY);
        P.ble_n = 0;
        xSemaphoreGive(P.ble_lock);
        bluetti_ble_scan(7000, ble_scan_cb, NULL);
    }

    char *buf = malloc(2048);
    if (!buf) return httpd_resp_send_500(r);
    int o = snprintf(buf, 2048, "{\"scanning\":%s,\"devices\":[",
                     bluetti_ble_scanning() ? "true" : "false");

    xSemaphoreTake(P.ble_lock, portMAX_DELAY);
    for (int i = 0; i < P.ble_n && o < 1900; i++) {
        o += snprintf(buf + o, 2048 - o,
                      "%s{\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"bluetti\":%s}",
                      i ? "," : "", P.ble[i].addr, P.ble[i].name,
                      P.ble[i].rssi, P.ble[i].looks_like_bluetti ? "true" : "false");
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
    REQUIRE_AUTH(r);   /* no-op during setup: no password set yet */
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

    *P.cfg = P.pending;
    P.cfg->provisioned = true;
    app_config_save(P.cfg);
    P.state = ST_CONNECTED;
    ESP_LOGI(TAG, "provisioned; station connected");
    xEventGroupSetBits(P.done, DONE_BIT);
    vTaskDelete(NULL);
}

/* Setup mode only handles Wi-Fi: join a network, or become an AP. The
 * BLUETTI unit and NUT are configured afterwards from the admin page. */
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
    char mode[8] = "";
    form_get(body, "wifi_mode", mode, sizeof(mode));
    bool ap_mode = strcmp(mode, "ap") == 0;

    /* Step 2 of the portal: the admin login for the page you'll land on. */
    char auth_user[33] = "", auth_pass[128] = "";
    form_get(body, "auth_user", auth_user, sizeof(auth_user));
    form_get(body, "auth_pass", auth_pass, sizeof(auth_pass));
    if (auth_pass[0] == '\0') {
        memset(body, 0, len);
        free(body);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "an admin password is required");
    }
    if (auth_user[0] == '\0') {
        strlcpy(auth_user, "admin", sizeof(auth_user));
    }
    strlcpy(P.pending.auth_user, auth_user, sizeof(P.pending.auth_user));
    app_config_set_password(&P.pending, auth_pass);
    memset(auth_pass, 0, sizeof(auth_pass));

    if (ap_mode) {
        P.pending.wifi_mode = APP_WIFI_AP;
        form_get(body, "ap_ssid", P.pending.ap_ssid, sizeof(P.pending.ap_ssid));
        form_get(body, "ap_pass", P.pending.ap_pass, sizeof(P.pending.ap_pass));
        memset(body, 0, len);
        free(body);

        size_t plen = strlen(P.pending.ap_pass);
        if (plen > 0 && plen < 8) {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                       "AP password must be 8+ characters");
        }
        if (P.pending.ap_ssid[0] == '\0') {
            wifi_mgr_default_ap_ssid(P.pending.ap_ssid,
                                     sizeof(P.pending.ap_ssid));
        }

        *P.cfg = P.pending;
        P.cfg->provisioned = true;
        if (app_config_save(P.cfg) != ESP_OK) {
            return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "could not save config");
        }
        ESP_LOGW(TAG, "provisioned in AP mode as '%s'; rebooting",
                 P.cfg->ap_ssid);
        send_json(r, "{\"ok\":true}");
        xTaskCreate(reboot_after_delay, "reboot", 2048, NULL, 5, NULL);
        return ESP_OK;
    }

    P.pending.wifi_mode = APP_WIFI_STATION;
    form_get(body, "ssid", P.pending.wifi_ssid, sizeof(P.pending.wifi_ssid));
    form_get(body, "pass", P.pending.wifi_pass, sizeof(P.pending.wifi_pass));
    memset(body, 0, len);
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
static esp_err_t h_admin_config(httpd_req_t *r);
static esp_err_t h_admin_reconfigure(httpd_req_t *r);
static esp_err_t h_admin_credentials(httpd_req_t *r);
/* POST /api/notify-test — send a Telegram message with the posted (or
 * stored) settings so the user can verify before saving. Blocking. */
static esp_err_t h_notify_test(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    int len = r->content_len;
    if (len < 0 || len > 1024) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body");
    }
    char *body = calloc(1, len + 1);
    if (!body) {
        return httpd_resp_send_500(r);
    }
    int got = 0;
    while (got < len) {
        int k = httpd_req_recv(r, body + got, len - got);
        if (k <= 0) { free(body); return httpd_resp_send_500(r); }
        got += k;
    }

    notify_config_t nc = { .enabled = true };
    char v[128];
    if (form_get(body, "tg_token", v, sizeof(v)) && v[0]) {
        strlcpy(nc.bot_token, v, sizeof(nc.bot_token));
    } else {
        strlcpy(nc.bot_token, P.cfg->tg_token, sizeof(nc.bot_token));
    }
    if (form_get(body, "tg_chat", v, sizeof(v)) && v[0]) {
        strlcpy(nc.chat_id, v, sizeof(nc.chat_id));
    } else {
        strlcpy(nc.chat_id, P.cfg->tg_chat, sizeof(nc.chat_id));
    }
    memset(body, 0, len);
    free(body);

    char err[128] = "";
    if (notify_send_test(&nc, P.cfg->ups_name, err, sizeof(err)) != 0) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   err[0] ? err : "send failed");
    }
    return send_json(r, "{\"ok\":true}");
}

static esp_err_t h_admin_reboot(httpd_req_t *r);
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
        /* Setup mode is Wi-Fi only; BLUETTI + NUT come later on the admin page. */
        reg(P.httpd, "/", HTTP_GET, h_root);
        reg(P.httpd, "/api/wifi-scan", HTTP_GET, h_wifi_scan);
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
        reg(P.httpd, "/api/config", HTTP_GET, h_admin_config);
        reg(P.httpd, "/api/reconfigure", HTTP_POST, h_admin_reconfigure);
        reg(P.httpd, "/api/credentials", HTTP_POST, h_admin_credentials);
        reg(P.httpd, "/api/notify-test", HTTP_POST, h_notify_test);
        reg(P.httpd, "/api/ble-scan", HTTP_GET, h_ble_scan);
        reg(P.httpd, "/api/wifi-scan", HTTP_GET, h_wifi_scan);
        reg(P.httpd, "/api/reboot", HTTP_POST, h_admin_reboot);
        reg(P.httpd, "/api/factory-reset", HTTP_POST, h_factory_reset);
#if CONFIG_ENABLE_WEB_OTA
        reg(P.httpd, "/api/ota", HTTP_POST, h_ota);
#endif
    }
    return ESP_OK;
}

/* ---- public: setup mode ------------------------------------------- */

esp_err_t provisioning_run(app_config_t *cfg)
{
    P.cfg = cfg;
    P.state = ST_IDLE;
    P.done = xEventGroupCreate();
    P.ble_lock = xSemaphoreCreateMutex();
    if (!P.done || !P.ble_lock) {
        return ESP_ERR_NO_MEM;
    }

    char ssid[33];
    wifi_mgr_default_ap_ssid(ssid, sizeof(ssid));
    ESP_ERROR_CHECK(wifi_mgr_ap_start(ssid, NULL));   /* open, for setup */
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
    bluetti_ble_scan_stop();
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
    REQUIRE_AUTH(r);
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, admin_html_start,
                           admin_html_end - admin_html_start - 1);
}

static esp_err_t h_admin_status(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    bool ap = P.cfg->wifi_mode == APP_WIFI_AP;
    char ip[16];
    if (ap) {
        wifi_mgr_ap_ip(ip, sizeof(ip));
    } else {
        wifi_mgr_sta_ip(ip, sizeof(ip));
    }
    char gw[16] = "-", dns[16] = "-";
    if (!ap) {
        wifi_mgr_sta_gw(gw, sizeof(gw));
        wifi_mgr_sta_dns(dns, sizeof(dns));
    }
    bluetti_state_t st;
    bool have = bluetti_ble_get_state(&st);
    const esp_app_desc_t *app = esp_app_get_description();
#if CONFIG_ENABLE_WEB_OTA
    const bool ota = true;
#else
    const bool ota = false;
#endif
    char out[760];
    snprintf(out, sizeof(out),
             "{\"wifi_mode\":\"%s\",\"network\":\"%s\",\"ip\":\"%s\","
             "\"addressing\":\"%s\",\"gateway\":\"%s\",\"dns\":\"%s\","
             "\"hostname\":\"%s\","
             "\"fw_version\":\"%s\",\"fw_date\":\"%s %s\",\"ota\":%s,"
             "\"ups\":\"%s\",\"nut_port\":%u,"
             "\"ble_target\":\"%s\",\"ble_connected\":%s,"
             "\"telemetry_valid\":%s,\"battery_pct\":%d,"
             "\"ac_input\":%s,\"charging\":%s,\"model\":\"%s\","
             "\"configured\":%s}",
             ap ? "ap" : "station",
             ap ? P.cfg->ap_ssid : P.cfg->wifi_ssid, ip,
             ap ? "ap" : (P.cfg->use_static_ip ? "static" : "dhcp"), gw, dns,
             P.cfg->hostname,
             app->version, app->date, app->time, ota ? "true" : "false",
             P.cfg->ups_name, P.cfg->nut_port,
             P.cfg->ble_addr[0] ? P.cfg->ble_addr : P.cfg->ble_name,
             bluetti_ble_connected() ? "true" : "false",
             have && st.valid ? "true" : "false",
             have ? st.soc_pct : 0,
             have && st.ac_input_present ? "true" : "false",
             have && st.charging ? "true" : "false",
             have && st.model[0] ? st.model : "",
             (P.cfg->ble_addr[0] || P.cfg->ble_name[0]) ? "true" : "false");
    return send_json(r, out);
}

static esp_err_t h_admin_logs(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
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
    REQUIRE_AUTH(r);
    httpd_resp_sendstr(r, "{\"ok\":true}");
    ESP_LOGW(TAG, "factory reset requested via web");
    app_config_erase();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ---- live reconfiguration (BLUETTI / NUT / Wi-Fi) ---------------- */

static esp_err_t h_admin_config(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char def_ap[33];
    wifi_mgr_default_ap_ssid(def_ap, sizeof(def_ap));
    char out[1000];  /* ssid + ap_ssid + users + addressing + telegram */
    snprintf(out, sizeof(out),
             "{\"ble_addr\":\"%s\",\"ble_name\":\"%s\",\"ble_probe\":%s,"
             "\"ups_name\":\"%s\",\"nut_port\":%u,\"low_pct\":%u,\"poll_ms\":%u,"
             "\"nut_user\":\"%s\",\"nut_auth_set\":%s,"
             "\"ac_rating_w\":%u,\"runtime_low_s\":%u,"
             "\"wifi_mode\":\"%s\",\"wifi_ssid\":\"%s\",\"has_wifi_pass\":%s,"
             "\"ap_ssid\":\"%s\",\"has_ap_pass\":%s,\"default_ap_ssid\":\"%s\","
             "\"auth_user\":\"%s\",\"auth_set\":%s,"
             "\"hostname\":\"%s\",\"use_static_ip\":%s,\"static_ip\":\"%s\","
             "\"static_mask\":\"%s\",\"static_gw\":\"%s\",\"static_dns\":\"%s\","
             "\"tg_enabled\":%s,\"tg_chat\":\"%s\",\"has_tg_token\":%s,"
             "\"tg_on_power\":%s,\"tg_on_low_batt\":%s,\"tg_on_link\":%s}",
             P.cfg->ble_addr, P.cfg->ble_name,
             P.cfg->ble_probe ? "true" : "false",
             P.cfg->ups_name, P.cfg->nut_port, P.cfg->low_pct, P.cfg->poll_ms,
             P.cfg->nut_user, P.cfg->nut_auth_set ? "true" : "false",
             P.cfg->ac_rating_w, P.cfg->runtime_low_s,
             P.cfg->wifi_mode == APP_WIFI_AP ? "ap" : "station",
             P.cfg->wifi_ssid, P.cfg->wifi_pass[0] ? "true" : "false",
             P.cfg->ap_ssid[0] ? P.cfg->ap_ssid : def_ap,
             P.cfg->ap_pass[0] ? "true" : "false", def_ap,
             P.cfg->auth_user, P.cfg->auth_set ? "true" : "false",
             P.cfg->hostname, P.cfg->use_static_ip ? "true" : "false",
             P.cfg->static_ip, P.cfg->static_mask, P.cfg->static_gw,
             P.cfg->static_dns,
             P.cfg->tg_enabled ? "true" : "false", P.cfg->tg_chat,
             P.cfg->tg_token[0] ? "true" : "false",
             P.cfg->tg_on_power ? "true" : "false",
             P.cfg->tg_on_low_batt ? "true" : "false",
             P.cfg->tg_on_link ? "true" : "false");
    return send_json(r, out);
}

static void reboot_after_delay(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(900));
    esp_restart();
}

/* POST /api/credentials — change the admin login. Requires the current
 * password so a hijacked open session can't lock the owner out. */
static esp_err_t h_admin_credentials(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    int len = r->content_len;
    if (len <= 0 || len > 1024) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body");
    }
    char *body = malloc(len + 1);
    if (!body) {
        return httpd_resp_send_500(r);
    }
    int got = 0;
    while (got < len) {
        int k = httpd_req_recv(r, body + got, len - got);
        if (k <= 0) { free(body); return httpd_resp_send_500(r); }
        got += k;
    }
    body[len] = '\0';

    char user[33] = "", cur[128] = "", pass[128] = "";
    form_get(body, "auth_user", user, sizeof(user));
    form_get(body, "current_pass", cur, sizeof(cur));
    form_get(body, "auth_pass", pass, sizeof(pass));
    memset(body, 0, len);
    free(body);

    if (P.cfg->auth_set && !app_config_check_password(P.cfg, cur)) {
        memset(pass, 0, sizeof(pass));
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "current password is wrong");
    }
    if (pass[0] == '\0') {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                   "a password is required");
    }
    if (user[0] == '\0') {
        strlcpy(user, "admin", sizeof(user));
    }

    P.pending = *P.cfg;
    strlcpy(P.pending.auth_user, user, sizeof(P.pending.auth_user));
    app_config_set_password(&P.pending, pass);
    memset(pass, 0, sizeof(pass));
    memset(cur, 0, sizeof(cur));

    *P.cfg = P.pending;
    if (app_config_save(P.cfg) != ESP_OK) {
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "could not save config");
    }
    ESP_LOGW(TAG, "admin credentials changed (user '%s')", P.cfg->auth_user);
    return send_json(r, "{\"ok\":true}");
}

static esp_err_t h_admin_reconfigure(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    int len = r->content_len;
    if (len <= 0 || len > 1024) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body");
    }
    char *body = malloc(len + 1);
    if (!body) {
        return httpd_resp_send_500(r);
    }
    int got = 0;
    while (got < len) {
        int k = httpd_req_recv(r, body + got, len - got);
        if (k <= 0) { free(body); return httpd_resp_send_500(r); }
        got += k;
    }
    body[len] = '\0';

    P.pending = *P.cfg;
    char v[128], section[16] = "";
    form_get(body, "section", section, sizeof(section));

    /* ---- BLUETTI unit + account ---- */
    if (strcmp(section, "bluetti") == 0) {
        if (form_get(body, "ble_addr", v, sizeof(v))) {
            strlcpy(P.pending.ble_addr, v, sizeof(P.pending.ble_addr));
        }
        if (form_get(body, "ble_name", v, sizeof(v)) && v[0]) {
            strlcpy(P.pending.ble_name, v, sizeof(P.pending.ble_name));
        }
        P.pending.ble_probe =
            form_get(body, "ble_probe", v, sizeof(v)) && v[0] == '1';
        free(body);

        if (P.pending.ble_addr[0] == '\0' && P.pending.ble_name[0] == '\0') {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                       "pick a device or enter an address");
        }

    /* ---- NUT server ---- */
    } else if (strcmp(section, "nut") == 0) {
        if (form_get(body, "ups_name", v, sizeof(v)) && v[0]) {
            strlcpy(P.pending.ups_name, v, sizeof(P.pending.ups_name));
        }
        if (form_get(body, "nut_port", v, sizeof(v)) && atoi(v) > 0) {
            P.pending.nut_port = (uint16_t)atoi(v);
        }
        if (form_get(body, "low_pct", v, sizeof(v)) && atoi(v) > 0) {
            P.pending.low_pct = (uint8_t)atoi(v);
        }
        if (form_get(body, "ac_rating_w", v, sizeof(v)) && atoi(v) > 0) {
            P.pending.ac_rating_w = (uint16_t)atoi(v);
        }
        if (form_get(body, "runtime_low_s", v, sizeof(v)) && atoi(v) >= 0) {
            P.pending.runtime_low_s = (uint16_t)atoi(v);
        }
        if (form_get(body, "nut_user", v, sizeof(v)) && v[0]) {
            strlcpy(P.pending.nut_user, v, sizeof(P.pending.nut_user));
        }
        /* Blank keeps the stored password; the checkbox clears it. */
        if (form_get(body, "nut_pass", v, sizeof(v)) && v[0]) {
            app_config_set_nut_password(&P.pending, v);
        }
        if (form_get(body, "nut_noauth", v, sizeof(v)) && v[0] == '1') {
            app_config_set_nut_password(&P.pending, "");
        }
        memset(v, 0, sizeof(v));
        memset(body, 0, len);
        free(body);

    /* ---- Wi-Fi (station or AP) ---- */
    } else if (strcmp(section, "wifi") == 0) {
        char mode[8] = "";
        form_get(body, "wifi_mode", mode, sizeof(mode));
        bool ap = strcmp(mode, "ap") == 0;

        /* The hostname is a device setting, not a station one. */
        if (form_get(body, "hostname", v, sizeof(v)) && v[0]) {
            strlcpy(P.pending.hostname, v, sizeof(P.pending.hostname));
        }

        if (ap) {
            P.pending.wifi_mode = APP_WIFI_AP;
            form_get(body, "ap_ssid", P.pending.ap_ssid,
                     sizeof(P.pending.ap_ssid));
            /* Blank password field = keep the stored one. */
            if (form_get(body, "ap_pass", v, sizeof(v)) && v[0]) {
                strlcpy(P.pending.ap_pass, v, sizeof(P.pending.ap_pass));
            }
            if (form_get(body, "ap_open", v, sizeof(v)) && v[0] == '1') {
                P.pending.ap_pass[0] = '\0';
            }
            free(body);

            size_t plen = strlen(P.pending.ap_pass);
            if (plen > 0 && plen < 8) {
                return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                           "AP password must be 8+ characters");
            }
            if (P.pending.ap_ssid[0] == '\0') {
                wifi_mgr_default_ap_ssid(P.pending.ap_ssid,
                                         sizeof(P.pending.ap_ssid));
            }
        } else {
            P.pending.wifi_mode = APP_WIFI_STATION;
            form_get(body, "wifi_ssid", P.pending.wifi_ssid,
                     sizeof(P.pending.wifi_ssid));
            if (form_get(body, "wifi_pass", v, sizeof(v)) && v[0]) {
                strlcpy(P.pending.wifi_pass, v, sizeof(P.pending.wifi_pass));
            }
            if (form_get(body, "wifi_open", v, sizeof(v)) && v[0] == '1') {
                P.pending.wifi_pass[0] = '\0';
            }
            P.pending.use_static_ip =
                form_get(body, "use_static_ip", v, sizeof(v)) && v[0] == '1';
            if (P.pending.use_static_ip) {
                form_get(body, "static_ip", P.pending.static_ip,
                         sizeof(P.pending.static_ip));
                form_get(body, "static_mask", P.pending.static_mask,
                         sizeof(P.pending.static_mask));
                form_get(body, "static_gw", P.pending.static_gw,
                         sizeof(P.pending.static_gw));
                form_get(body, "static_dns", P.pending.static_dns,
                         sizeof(P.pending.static_dns));
            }
            free(body);

            if (P.pending.wifi_ssid[0] == '\0') {
                return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                           "pick a Wi-Fi network");
            }
            if (P.pending.use_static_ip) {
                if (!valid_ipv4(P.pending.static_ip)) {
                    return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                               "static IP is not a valid IPv4 address");
                }
                if (P.pending.static_mask[0] == '\0') {
                    strlcpy(P.pending.static_mask, "255.255.255.0",
                            sizeof(P.pending.static_mask));
                } else if (!valid_ipv4(P.pending.static_mask)) {
                    return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                               "subnet mask is not valid");
                }
                if (P.pending.static_gw[0] && !valid_ipv4(P.pending.static_gw)) {
                    return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                               "gateway is not a valid IPv4 address");
                }
                if (P.pending.static_dns[0] && !valid_ipv4(P.pending.static_dns)) {
                    return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                               "DNS server is not a valid IPv4 address");
                }
            }
        }
    /* ---- Telegram notifications ---- */
    } else if (strcmp(section, "notify") == 0) {
        P.pending.tg_enabled =
            form_get(body, "tg_enabled", v, sizeof(v)) && v[0] == '1';
        /* Blank token field means "keep the stored one". */
        if (form_get(body, "tg_token", v, sizeof(v)) && v[0]) {
            strlcpy(P.pending.tg_token, v, sizeof(P.pending.tg_token));
        }
        form_get(body, "tg_chat", P.pending.tg_chat, sizeof(P.pending.tg_chat));
        P.pending.tg_on_power =
            form_get(body, "tg_on_power", v, sizeof(v)) && v[0] == '1';
        P.pending.tg_on_low_batt =
            form_get(body, "tg_on_low_batt", v, sizeof(v)) && v[0] == '1';
        P.pending.tg_on_link =
            form_get(body, "tg_on_link", v, sizeof(v)) && v[0] == '1';
        memset(body, 0, len);
        free(body);

        if (P.pending.tg_enabled &&
            (P.pending.tg_token[0] == '\0' || P.pending.tg_chat[0] == '\0')) {
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                                       "a bot token and chat id are required");
        }

    } else {
        free(body);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "unknown section");
    }

    *P.cfg = P.pending;
    P.cfg->provisioned = true;
    if (app_config_save(P.cfg) != ESP_OK) {
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "could not save config");
    }
    ESP_LOGW(TAG, "'%s' settings changed via web; rebooting", section);
    send_json(r, "{\"ok\":true}");
    xTaskCreate(reboot_after_delay, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_admin_reboot(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    ESP_LOGW(TAG, "reboot requested via web");
    send_json(r, "{\"ok\":true}");
    xTaskCreate(reboot_after_delay, "reboot", 2048, NULL, 5, NULL);
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
    REQUIRE_AUTH(r);
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
    if (!P.ble_lock) {
        P.ble_lock = xSemaphoreCreateMutex();   /* used by /api/ble-scan */
    }
    esp_err_t rc = start_httpd(false);
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "admin server on :80");
    }
    return rc;
}
