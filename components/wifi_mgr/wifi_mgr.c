#include "wifi_mgr.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "wifi_mgr";

#define BIT_CONNECTED  BIT0
#define BIT_FAILED     BIT1
#define MAX_RETRY      8

static EventGroupHandle_t s_events;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static bool               s_inited;
static bool               s_want_connect;
static int                s_retries;
static char               s_ip[16] = "0.0.0.0";

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
        if (s_retries < MAX_RETRY) {
            s_retries++;
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_retries, MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        ESP_LOGI(TAG, "STA got IP %s", s_ip);
        s_retries = 0;
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

esp_err_t wifi_mgr_ap_start(const char *ssid)
{
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

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

    ESP_LOGI(TAG, "SoftAP '%s' up (open), http://192.168.4.1/", ssid);
    return ESP_OK;
}

esp_err_t wifi_mgr_ap_stop(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "SoftAP stopped");
    return err;
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
