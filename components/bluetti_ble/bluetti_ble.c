/*
 * NimBLE central for BLUETTI power stations.
 *
 * Probe-first: rather than guessing at a protocol that has not been
 * confirmed on an Elite 10, this connects and reports what the device
 * actually exposes. Point it at your unit with probe mode on and read
 * the admin page's Logs tab.
 *
 * What to look for in that output:
 *
 *   - A Nordic-UART-style pair (6e400002 write / 6e400003 notify) with
 *     notifications that look like Modbus RTU responses — a leading
 *     slave id, function code 0x03, a byte count, then register data
 *     ending in a CRC-16/Modbus. That is the documented older-BLUETTI
 *     protocol and a decoder is straightforward from there.
 *
 *   - Notifications beginning 2A 2A, or a burst of traffic followed by
 *     the device dropping the link when we do not answer. That is the
 *     newer encrypted handshake. No public implementation derives its
 *     keys; the projects that support it lift a per-device pin, key and
 *     token out of an Android Bluetooth HCI capture.
 */

#include "bluetti_ble.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "bluetti_ble";

/* Advertised-name prefixes BLUETTI units are known to use. The Elite
 * series has not been confirmed; the probe reports whatever it sees, so
 * an unmatched name is not fatal — it just misses the ★ marker. */
static const char *BT_PREFIXES[] = {
    "BLUETTI", "Bluetti", "AC", "EB", "EP", "AP", "EL", "AORA", "PR", "RV",
};

typedef enum { MODE_IDLE, MODE_SCAN, MODE_CONNECT } ble_mode_t;

static struct {
    bluetti_ble_config_t cfg;
    bluetti_state_cb_t   state_cb;
    void                *state_cb_user;

    bluetti_scan_cb_t    scan_cb;
    void                *scan_cb_user;
    uint8_t              seen[24][6];
    int                  seen_n;

    ble_addr_t           target_addr;
    bool                 have_target_addr;
    char                 name_prefix[24];

    ble_mode_t           mode;
    bool                 resume_connect;
    bool                 synced;
    SemaphoreHandle_t    sync_sem;

    uint8_t              own_addr_type;
    uint16_t             conn_handle;
    bool                 connected;

    /* Characteristics found during discovery, for the probe report. */
    uint16_t             notify_handles[8];
    int                  notify_n;

    bluetti_state_t      state;
    SemaphoreHandle_t    lock;
    esp_timer_handle_t   scan_timer;
} b;

static void start_connect_scan(void);
static void start_disc(bool report_all);
static int  gap_event(struct ble_gap_event *event, void *arg);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void addr_to_str(const uint8_t val[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             val[5], val[4], val[3], val[2], val[1], val[0]);
}

static bool parse_addr(const char *str, ble_addr_t *out)
{
    unsigned v[6];
    if (!str || sscanf(str, "%x:%x:%x:%x:%x:%x",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out->val[i] = (uint8_t)v[5 - i];   /* NimBLE stores LSB first */
    }
    out->type = BLE_ADDR_PUBLIC;
    return true;
}

static bool name_looks_bluetti(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(BT_PREFIXES) / sizeof(BT_PREFIXES[0]); i++) {
        size_t pl = strlen(BT_PREFIXES[i]);
        if (len >= pl && strncmp(name, BT_PREFIXES[i], pl) == 0) {
            return true;
        }
    }
    return false;
}

static bool adv_name_matches_target(const struct ble_hs_adv_fields *f)
{
    if (b.name_prefix[0] == '\0' || f->name_len == 0) {
        return false;
    }
    size_t pl = strlen(b.name_prefix);
    return f->name_len >= pl &&
           strncmp((const char *)f->name, b.name_prefix, pl) == 0;
}

static bool seen_before(const uint8_t val[6])
{
    for (int i = 0; i < b.seen_n; i++) {
        if (memcmp(b.seen[i], val, 6) == 0) {
            return true;
        }
    }
    if (b.seen_n < (int)(sizeof(b.seen) / sizeof(b.seen[0]))) {
        memcpy(b.seen[b.seen_n++], val, 6);
    }
    return false;
}

/* Render a UUID for logging; NimBLE's own formatter needs a buffer. */
static const char *uuid_str(const ble_uuid_t *u, char *buf, size_t len)
{
    ble_uuid_to_str(u, buf);
    (void)len;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Probe: enumerate everything and subscribe to whatever notifies       */
/* ------------------------------------------------------------------ */

static int on_probe_dsc(uint16_t conn, const struct ble_gatt_error *err,
                        uint16_t chr_val_handle,
                        const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0 && dsc) {
        char u[BLE_UUID_STR_LEN];
        ESP_LOGI(TAG, "      descriptor %s (handle %d)",
                 uuid_str(&dsc->uuid.u, u, sizeof(u)), dsc->handle);
        /* 0x2902 is the CCCD: writing 0x0001 turns notifications on. */
        if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
            uint8_t enable[2] = { 0x01, 0x00 };
            if (ble_gattc_write_flat(conn, dsc->handle, enable,
                                     sizeof(enable), NULL, NULL) == 0) {
                ESP_LOGI(TAG, "      -> subscribed");
            }
        }
    }
    return 0;
}

static int on_probe_chr(uint16_t conn, const struct ble_gatt_error *err,
                        const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        char u[BLE_UUID_STR_LEN];
        const uint8_t p = chr->properties;
        ESP_LOGI(TAG, "  char %s val_handle=%d props=%s%s%s%s%s",
                 uuid_str(&chr->uuid.u, u, sizeof(u)), chr->val_handle,
                 (p & BLE_GATT_CHR_PROP_READ)   ? "R" : "",
                 (p & BLE_GATT_CHR_PROP_WRITE)  ? "W" : "",
                 (p & BLE_GATT_CHR_PROP_WRITE_NO_RSP) ? "w" : "",
                 (p & BLE_GATT_CHR_PROP_NOTIFY) ? "N" : "",
                 (p & BLE_GATT_CHR_PROP_INDICATE) ? "I" : "");

        if (p & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) {
            if (b.notify_n < (int)(sizeof(b.notify_handles) /
                                   sizeof(b.notify_handles[0]))) {
                b.notify_handles[b.notify_n++] = chr->val_handle;
            }
            /* Find its CCCD so we can turn notifications on. */
            ble_gattc_disc_all_dscs(conn, chr->val_handle,
                                    chr->val_handle + 2, on_probe_dsc, NULL);
        }
    } else if (err->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "  (end of characteristics)");
    }
    return 0;
}

static int on_probe_svc(uint16_t conn, const struct ble_gatt_error *err,
                        const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) {
        char u[BLE_UUID_STR_LEN];
        ESP_LOGI(TAG, "service %s handles %d..%d",
                 uuid_str(&svc->uuid.u, u, sizeof(u)),
                 svc->start_handle, svc->end_handle);
        ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle,
                                on_probe_chr, NULL);
    } else if (err->status == BLE_HS_EDONE) {
        ESP_LOGW(TAG, "probe: GATT enumeration done. Watch for notifications "
                      "below; nothing is decoded yet.");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* GAP events                                                          */
/* ------------------------------------------------------------------ */

static void handle_scan_disc(struct ble_gap_event *event)
{
    if (seen_before(event->disc.addr.val)) {
        return;
    }
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, event->disc.data,
                                event->disc.length_data) != 0) {
        return;
    }
    bluetti_scan_entry_t e = { .rssi = event->disc.rssi };
    addr_to_str(event->disc.addr.val, e.addr);
    if (f.name_len) {
        size_t n = f.name_len < sizeof(e.name) - 1 ? f.name_len
                                                   : sizeof(e.name) - 1;
        memcpy(e.name, f.name, n);
        e.name[n] = '\0';
        e.looks_like_bluetti = name_looks_bluetti((const char *)f.name,
                                                  f.name_len);
    }
    /* Manufacturer data distinguishes protocol variants on some models,
     * so log it while probing. */
    if (b.cfg.probe && f.mfg_data_len > 0) {
        ESP_LOGI(TAG, "%s mfg data (%d bytes):", e.addr, f.mfg_data_len);
        ESP_LOG_BUFFER_HEX(TAG, f.mfg_data, f.mfg_data_len);
    }
    if (b.scan_cb) {
        b.scan_cb(&e, b.scan_cb_user);
    }
}

static void try_connect(const ble_addr_t *addr)
{
    ble_gap_disc_cancel();
    int rc = ble_gap_connect(b.own_addr_type, addr, 10000, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d, rescanning", rc);
        start_connect_scan();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_DISC:
        if (b.mode == MODE_SCAN) {
            handle_scan_disc(event);
        } else if (b.mode == MODE_CONNECT) {
            struct ble_hs_adv_fields f;
            if (ble_hs_adv_parse_fields(&f, event->disc.data,
                                        event->disc.length_data) != 0) {
                return 0;
            }
            bool match = b.have_target_addr
                ? memcmp(event->disc.addr.val, b.target_addr.val, 6) == 0
                : adv_name_matches_target(&f);
            if (match) {
                ESP_LOGI(TAG, "found target, connecting");
                try_connect(&event->disc.addr);
            }
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            b.conn_handle = event->connect.conn_handle;
            b.connected = true;
            b.notify_n = 0;
            ble_gattc_exchange_mtu(b.conn_handle, NULL, NULL);
            if (b.cfg.probe) {
                ESP_LOGW(TAG, "probe mode: enumerating GATT");
                ble_gattc_disc_all_svcs(b.conn_handle, on_probe_svc, NULL);
            } else {
                ESP_LOGW(TAG, "connected, but no protocol decoder is "
                              "implemented yet — enable probe mode");
            }
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            start_connect_scan();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason 0x%x)%s", event->disconnect.reason,
                 b.cfg.probe ? " — an early drop with no traffic often means "
                               "the device expected an encrypted handshake"
                             : "");
        b.connected = false;
        if (b.mode == MODE_SCAN) {
            start_disc(true);
        } else if (b.mode == MODE_CONNECT) {
            start_connect_scan();
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.conn_handle != b.conn_handle) {
            return 0;
        }
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t tmp[256];
        if (len > sizeof(tmp)) {
            len = sizeof(tmp);
        }
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, tmp, len, &len) != 0) {
            return 0;
        }
        ESP_LOGI(TAG, "notify handle=%d len=%u",
                 event->notify_rx.attr_handle, (unsigned)len);
        ESP_LOG_BUFFER_HEX(TAG, tmp, len);
        /* TODO: decode once the protocol is confirmed on hardware. */
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (b.mode == MODE_SCAN) {
            b.mode = MODE_IDLE;
            ESP_LOGI(TAG, "scan complete (%d devices)", b.seen_n);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Scan / connect                                                      */
/* ------------------------------------------------------------------ */

static void start_disc(bool report_all)
{
    struct ble_gap_disc_params dp = {
        .passive = 0,
        .filter_duplicates = report_all ? 0 : 1,
    };
    int rc = ble_gap_disc(b.own_addr_type, BLE_HS_FOREVER, &dp, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    }
}

static void start_connect_scan(void)
{
    if (b.mode != MODE_CONNECT) {
        return;
    }
    ESP_LOGI(TAG, "scanning for target BLUETTI");
    start_disc(false);
}

static void scan_timer_cb(void *arg)
{
    bluetti_ble_scan_stop();
}

/* ------------------------------------------------------------------ */
/* Host lifecycle                                                      */
/* ------------------------------------------------------------------ */

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble host reset, reason %d", reason);
    b.synced = false;
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &b.own_addr_type) != 0) {
        ESP_LOGE(TAG, "address setup failed");
        return;
    }
    b.synced = true;
    if (b.sync_sem) {
        xSemaphoreGive(b.sync_sem);
    }
    if (b.mode == MODE_CONNECT) {
        start_connect_scan();
    } else if (b.mode == MODE_SCAN) {
        start_disc(true);
    }
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int bluetti_ble_host_init(void)
{
    static bool inited;
    if (inited) {
        return b.synced ? 0 : -1;
    }

    b.lock = xSemaphoreCreateMutex();
    b.sync_sem = xSemaphoreCreateBinary();
    if (!b.lock || !b.sync_sem) {
        return -1;
    }
    b.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    b.mode = MODE_IDLE;

    esp_timer_create_args_t st = { .callback = scan_timer_cb, .name = "bt_scan" };
    esp_timer_create(&st, &b.scan_timer);

    /* nimble_port_init() returns void on IDF <5.2 and esp_err_t on newer;
     * call it without capturing so this builds either way. */
    nimble_port_init();

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gap_device_name_set("esp32-nut-bluetti");

    nimble_port_freertos_init(host_task);
    inited = true;

    if (xSemaphoreTake(b.sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "BLE host not synced after 5s");
        return -1;
    }
    return 0;
}

int bluetti_ble_scan(uint32_t duration_ms, bluetti_scan_cb_t cb, void *user)
{
    b.scan_cb = cb;
    b.scan_cb_user = user;
    b.seen_n = 0;

    b.resume_connect = (b.mode == MODE_CONNECT);
    b.mode = MODE_SCAN;

    esp_timer_stop(b.scan_timer);
    esp_timer_start_once(b.scan_timer, (uint64_t)duration_ms * 1000);

    if (b.connected) {
        ble_gap_terminate(b.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        /* start_disc() runs from the DISCONNECT handler */
    } else if (b.synced) {
        ble_gap_disc_cancel();
        start_disc(true);
    }
    return 0;
}

void bluetti_ble_scan_stop(void)
{
    esp_timer_stop(b.scan_timer);
    ble_gap_disc_cancel();
    if (b.mode != MODE_SCAN) {
        return;
    }
    if (b.resume_connect) {
        b.resume_connect = false;
        b.mode = MODE_CONNECT;
        start_connect_scan();
    } else {
        b.mode = MODE_IDLE;
    }
}

bool bluetti_ble_scanning(void)
{
    return b.mode == MODE_SCAN;
}

bool bluetti_ble_get_state(bluetti_state_t *out)
{
    if (!out || !b.lock) {
        return false;
    }
    xSemaphoreTake(b.lock, portMAX_DELAY);
    *out = b.state;
    bool ok = b.state.valid;
    xSemaphoreGive(b.lock);
    return ok;
}

bool bluetti_ble_connected(void)
{
    return b.connected;
}

int bluetti_ble_start(const bluetti_ble_config_t *config,
                      bluetti_state_cb_t cb, void *user)
{
    if (!config) {
        return -1;
    }
    if (bluetti_ble_host_init() != 0) {
        return -1;
    }
    bluetti_ble_scan_stop();

    b.cfg = *config;
    b.state_cb = cb;
    b.state_cb_user = user;

    xSemaphoreTake(b.lock, portMAX_DELAY);
    memset(&b.state, 0, sizeof(b.state));
    b.state.soc_low_pct = config->low_battery_pct;
    b.state.minutes_remaining = BLUETTI_UNKNOWN_I;
    b.state.ac_in_watts = BLUETTI_UNKNOWN_F;
    b.state.ac_out_watts = BLUETTI_UNKNOWN_F;
    xSemaphoreGive(b.lock);

    b.have_target_addr = false;
    b.name_prefix[0] = '\0';
    if (config->ble_address && config->ble_address[0] &&
        parse_addr(config->ble_address, &b.target_addr)) {
        b.have_target_addr = true;
        ESP_LOGI(TAG, "target address %s", config->ble_address);
    } else {
        strlcpy(b.name_prefix,
                config->ble_name_prefix && config->ble_name_prefix[0]
                    ? config->ble_name_prefix : "BLUETTI",
                sizeof(b.name_prefix));
        ESP_LOGI(TAG, "target name prefix '%s'", b.name_prefix);
    }

    if (config->probe) {
        ESP_LOGW(TAG, "PROBE MODE: GATT and notifications will be logged, "
                      "nothing will be decoded");
    }

    b.mode = MODE_CONNECT;
    start_connect_scan();
    return 0;
}
