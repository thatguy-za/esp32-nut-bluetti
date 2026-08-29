#pragma once

#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Setup mode. Brings up an open SoftAP + captive portal, serves the
 * config page, and blocks until the user submits Wi-Fi credentials that
 * successfully associate. On return:
 *   - *cfg holds the confirmed settings (and has been saved to NVS with
 *     provisioned = true),
 *   - the SoftAP / portal / DNS server are torn down,
 *   - the STA link is already up.
 * Requires wifi_mgr_init() and bluetti_ble_host_init() beforehand.
 */
esp_err_t provisioning_run(app_config_t *cfg);

/*
 * Normal mode. Starts a small always-on HTTP server (port 80) with a
 * status page and a "forget config & reboot" control, so the device can
 * be re-provisioned without the BOOT button. Non-blocking.
 */
esp_err_t provisioning_admin_start(const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
