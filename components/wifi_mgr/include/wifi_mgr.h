#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t authmode;   /* wifi_auth_mode_t */
} wifi_scan_entry_t;

/* One-time: esp_netif + default event loop + esp_wifi_init + esp_wifi_start
 * in STA mode. Safe to call once; idempotent. */
esp_err_t wifi_mgr_init(void);

/* Bring up an open SoftAP alongside STA (mode APSTA). */
esp_err_t wifi_mgr_ap_start(const char *ssid);

/* Drop the SoftAP, return to STA-only. */
esp_err_t wifi_mgr_ap_stop(void);

/* Blocking active scan. Fills out[] (up to max), returns count or -1. */
int wifi_mgr_scan(wifi_scan_entry_t *out, int max);

/* Set STA credentials and connect; block up to timeout_ms for an IP.
 * ESP_OK on success, ESP_ERR_TIMEOUT / ESP_FAIL otherwise. */
esp_err_t wifi_mgr_sta_connect(const char *ssid, const char *pass,
                               uint32_t timeout_ms);

bool wifi_mgr_sta_connected(void);

/* Dotted-quad of the current STA IP, or "0.0.0.0". */
void wifi_mgr_sta_ip(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
