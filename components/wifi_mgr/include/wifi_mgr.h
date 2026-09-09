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

/* Bring up a SoftAP alongside STA (mode APSTA). A NULL/empty `pass` makes
 * it an open network; otherwise WPA2-PSK (8..63 chars). */
esp_err_t wifi_mgr_ap_start(const char *ssid, const char *pass);

/* Drop the SoftAP, return to STA-only. */
esp_err_t wifi_mgr_ap_stop(void);

/*
 * Fallback AP: while in station mode, raise a SoftAP if the station link
 * has been down for `after_ms`, and drop it again once the station is
 * back. It exists so a headless bridge whose network has gone (router
 * dead, SSID renamed, password changed) can still be reached — join the
 * AP and the admin page is on 192.168.4.1.
 *
 * Off unless enabled. A NULL/empty ssid means the board's default name;
 * a NULL/empty pass means an open network. Safe to call before or after
 * the station is up, and again later to change the settings.
 */
void wifi_mgr_set_fallback_ap(bool enabled, const char *ssid,
                              const char *pass, uint32_t after_ms);

/* True while the SoftAP currently up was raised by the fallback (as
 * opposed to configured AP mode). */
bool wifi_mgr_fallback_ap_active(void);

/* Default SoftAP SSID for this board: "esp-nut-bluetti-XXXX" (MAC tail). */
void wifi_mgr_default_ap_ssid(char *buf, size_t len);

/* AP-mode address (the SoftAP's own IP), or "0.0.0.0" when down. */
void wifi_mgr_ap_ip(char *buf, size_t len);
bool wifi_mgr_ap_active(void);

/* Blocking active scan. Fills out[] (up to max), returns count or -1. */
int wifi_mgr_scan(wifi_scan_entry_t *out, int max);

/* Station addressing. All IPv4. With `use_static` false the interface
 * takes a DHCP lease; the hostname is still sent as the DHCP client name.
 * Apply before wifi_mgr_sta_connect(). Any of ip/mask/gw/dns may be NULL
 * or empty, in which case a sane default is used (mask 255.255.255.0,
 * dns = gateway). */
typedef struct {
    const char *hostname;
    bool        use_static;
    const char *ip;
    const char *mask;
    const char *gw;
    const char *dns;
} wifi_mgr_ipv4_t;

esp_err_t wifi_mgr_set_ipv4(const wifi_mgr_ipv4_t *c);

/* Set STA credentials and connect; block up to timeout_ms for an IP.
 * ESP_OK on success, ESP_ERR_TIMEOUT / ESP_FAIL otherwise. */
esp_err_t wifi_mgr_sta_connect(const char *ssid, const char *pass,
                               uint32_t timeout_ms);

/* Current station gateway / DNS, for the status page. */
void wifi_mgr_sta_gw(char *buf, size_t len);
void wifi_mgr_sta_dns(char *buf, size_t len);

/* True only while the station holds a DHCP/static lease right now: set on
 * GOT_IP, cleared on disconnect. */
bool wifi_mgr_sta_connected(void);

/* Dotted-quad of the current STA IP, or "0.0.0.0". */
void wifi_mgr_sta_ip(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
