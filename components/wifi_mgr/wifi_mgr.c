#include "wifi_mgr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "wifi_mgr";

#define BIT_CONNECTED  BIT0
#define BIT_FAILED     BIT1
/* Attempts before wifi_mgr_sta_connect() reports failure to its caller.
 * Reconnection itself never gives up — see reconnect_later(). */
#define MAX_RETRY      8
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000

static EventGroupHandle_t s_events;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static bool               s_ap_up;
static bool               s_inited;
static bool               s_want_connect;
static int                s_retries;
static uint32_t           s_backoff_ms = BACKOFF_MIN_MS;
static esp_timer_handle_t s_retry_timer;
static char               s_ip[16] = "0.0.0.0";

/* Retry forever, with backoff. Giving up permanently would strand a
 * headless device after any transient outage — a rebooted router, or the
 * link drop that the AP->STA mode switch causes at the end of setup. */
static void retry_timer_cb(void *arg)
{
    if (s_want_connect) {
        esp_wifi_connect();
    }
}

static void reconnect_later(void)
{
    if (!s_retry_timer) {
        const esp_timer_create_args_t a = { .callback = retry_timer_cb,
                                            .name = "wifi_retry" };
        if (esp_timer_create(&a, &s_retry_timer) != ESP_OK) {
            esp_wifi_connect();          /* no timer: at least try once */
            return;
        }
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000);
    if (s_backoff_ms < BACKOFF_MAX_MS) {
        s_backoff_ms *= 2;
        if (s_backoff_ms > BACKOFF_MAX_MS) {
            s_backoff_ms = BACKOFF_MAX_MS;
        }
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_connect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        strcpy(s_ip, "0.0.0.0");
        if (!s_want_connect) {
            return;
        }
        s_retries++;
        if (s_retries == MAX_RETRY) {
            /* Unblock an in-flight wifi_mgr_sta_connect() so setup can
             * report the failure, but keep trying in the background. */
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
        ESP_LOGW(TAG, "STA disconnected (attempt %d), retrying in %u ms",
                 s_retries, (unsigned)s_backoff_ms);
        reconnect_later();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        ESP_LOGI(TAG, "STA got IP %s", s_ip);
        s_retries = 0;
        s_backoff_ms = BACKOFF_MIN_MS;
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
        }
        xEventGroupClearBits(s_events, BIT_FAILED);
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t wifi_mgr_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_inited = true;
    return ESP_OK;
}

void wifi_mgr_default_ap_ssid(char *buf, size_t len)
{
    uint8_t mac[6] = { 0 };
    /* eFuse, not esp_wifi_get_mac(): callers may run before Wi-Fi starts,
     * and this must match app_config_default_name() exactly. */
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "esp-nut-ecoflow-%02X%02X", mac[4], mac[5]);
}

esp_err_t wifi_mgr_ap_start(const char *ssid, const char *pass)
{
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    char fallback[33];
    if (!ssid || !ssid[0]) {
        wifi_mgr_default_ap_ssid(fallback, sizeof(fallback));
        ssid = fallback;
    }
    /* WPA2-PSK needs >= 8 chars; anything shorter is treated as "open". */
    bool secured = pass && strlen(pass) >= 8;

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = secured ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (secured) {
        strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    /* Hand out our own address as the DNS server so the captive DNS
     * responder actually sees the clients' queries. */
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("192.168.4.1");
    esp_netif_dhcps_stop(s_ap_netif);
    uint8_t offer = 0x02; /* OFFER_DNS */
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(s_ap_netif);

    s_ap_up = true;
    ESP_LOGI(TAG, "SoftAP '%s' up (%s), http://192.168.4.1/",
             ssid, secured ? "WPA2" : "open");
    return ESP_OK;
}

esp_err_t wifi_mgr_ap_stop(void)
{
    /* Dropping APSTA back to STA reinitialises the interfaces and takes
     * the station link with it, so reconnect deliberately rather than
     * relying on the disconnect handler to notice. */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_up = false;
    if (s_want_connect) {
        s_retries = 0;
        s_backoff_ms = BACKOFF_MIN_MS;
        xEventGroupClearBits(s_events, BIT_FAILED);
        esp_wifi_connect();
    }
    ESP_LOGI(TAG, "SoftAP stopped");
    return err;
}

bool wifi_mgr_ap_active(void)
{
    return s_ap_up;
}

void wifi_mgr_ap_ip(char *buf, size_t len)
{
    if (!s_ap_up || !s_ap_netif) {
        strlcpy(buf, "0.0.0.0", len);
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, buf, len);
    } else {
        strlcpy(buf, "192.168.4.1", len);
    }
}

int wifi_mgr_scan(wifi_scan_entry_t *out, int max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return -1;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        return 0;
    }
    wifi_ap_record_t *recs = calloc(num, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&num, recs);

    int n = 0;
    for (int i = 0; i < num && n < max; i++) {
        /* skip duplicate SSIDs (keep the strongest, which comes first) */
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].ssid, (char *)recs[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup || recs[i].ssid[0] == '\0') {
            continue;
        }
        strlcpy(out[n].ssid, (char *)recs[i].ssid, sizeof(out[n].ssid));
        out[n].rssi = recs[i].rssi;
        out[n].authmode = recs[i].authmode;
        n++;
    }
    free(recs);
    return n;
}

esp_err_t wifi_mgr_set_ipv4(const wifi_mgr_ipv4_t *c)
{
    if (!s_sta_netif || !c) {
        return ESP_ERR_INVALID_STATE;
    }
    if (c->hostname && c->hostname[0]) {
        esp_err_t he = esp_netif_set_hostname(s_sta_netif, c->hostname);
        if (he != ESP_OK) {
            ESP_LOGW(TAG, "hostname '%s' rejected: %s",
                     c->hostname, esp_err_to_name(he));
        }
    }

    if (!c->use_static) {
        /* Back to DHCP. Starting an already-started client is not an error. */
        esp_err_t e = esp_netif_dhcpc_start(s_sta_netif);
        if (e == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            e = ESP_OK;
        }
        ESP_LOGI(TAG, "station addressing: DHCP");
        return e;
    }

    if (!c->ip || !c->ip[0]) {
        ESP_LOGE(TAG, "static addressing needs an IP; staying on DHCP");
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_dhcpc_stop(s_sta_netif);      /* must be stopped to set a lease */

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(c->ip);
    ip.netmask.addr = esp_ip4addr_aton(
        (c->mask && c->mask[0]) ? c->mask : "255.255.255.0");
    if (c->gw && c->gw[0]) {
        ip.gw.addr = esp_ip4addr_aton(c->gw);
    }
    esp_err_t e = esp_netif_set_ip_info(s_sta_netif, &ip);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set_ip_info failed: %s", esp_err_to_name(e));
        esp_netif_dhcpc_start(s_sta_netif);         /* don't strand the device */
        return e;
    }

    /* No DHCP means no resolver either, so one has to be set explicitly.
     * Falling back to the gateway suits nearly every home network. */
    const char *dns = (c->dns && c->dns[0]) ? c->dns
                    : ((c->gw && c->gw[0]) ? c->gw : NULL);
    if (dns) {
        esp_netif_dns_info_t di = { 0 };
        di.ip.type = ESP_IPADDR_TYPE_V4;
        di.ip.u_addr.ip4.addr = esp_ip4addr_aton(dns);
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &di);
    }

    ESP_LOGI(TAG, "station addressing: static %s/%s gw %s dns %s",
             c->ip, (c->mask && c->mask[0]) ? c->mask : "255.255.255.0",
             (c->gw && c->gw[0]) ? c->gw : "(none)", dns ? dns : "(none)");
    return ESP_OK;
}

void wifi_mgr_sta_gw(char *buf, size_t len)
{
    esp_netif_ip_info_t ip;
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.gw, buf, len);
    } else {
        strlcpy(buf, "0.0.0.0", len);
    }
}

void wifi_mgr_sta_dns(char *buf, size_t len)
{
    esp_netif_dns_info_t di;
    if (s_sta_netif &&
        esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &di) == ESP_OK) {
        esp_ip4addr_ntoa(&di.ip.u_addr.ip4, buf, len);
    } else {
        strlcpy(buf, "0.0.0.0", len);
    }
}

esp_err_t wifi_mgr_sta_connect(const char *ssid, const char *pass,
                               uint32_t timeout_ms)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    wc.sta.threshold.authmode =
        (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    s_retries = 0;
    s_backoff_ms = BACKOFF_MIN_MS;
    s_want_connect = true;

    esp_wifi_disconnect();
    esp_err_t rc = esp_wifi_connect();
    if (rc != ESP_OK && rc != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(rc));
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & BIT_CONNECTED) {
        return ESP_OK;
    }
    if (bits & BIT_FAILED) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

bool wifi_mgr_sta_connected(void)
{
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

void wifi_mgr_sta_ip(char *buf, size_t len)
{
    strlcpy(buf, s_ip, len);
}
