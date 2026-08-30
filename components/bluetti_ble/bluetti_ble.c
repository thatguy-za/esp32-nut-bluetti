/*
 * NimBLE central for BLUETTI power stations.
 *
 * Connects to the unit's ff00 service, runs the "2a2a" key exchange (see
 * bt_session.c) and then polls Modbus holding registers, decoding them per
 * the model table in bt_regs.c. The unit names itself in register 110, so
 * the map is chosen from what it reports rather than from configuration.
 *
 * Probe mode is kept as a diagnostic: it enumerates every service and
 * characteristic and hex-dumps notifications without decoding, which is
 * how to confirm the protocol on a model this has not been tried against.
 */

#include "bluetti_ble.h"
#include "bt_session.h"
#include "bt_regs.h"

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

/* Loose fallback prefixes, for BLUETTI kit that is not in the supported
 * model table — it should still appear in the picker rather than looking
 * invisible. An unmatched name is not fatal; it just misses the ★. */
static const char *BT_PREFIXES[] = {
    "BLUETTI", "Bluetti", "AC", "EB", "EP", "AP", "EL", "AORA", "PR", "RV",
};

/* BLUETTI's GATT service: 16-bit ff00, with ff01 notify / ff02 write. */
static const ble_uuid16_t SVC_UUID    = BLE_UUID16_INIT(0xff00);
static const ble_uuid16_t NOTIFY_UUID = BLE_UUID16_INIT(0xff01);
static const ble_uuid16_t WRITE_UUID  = BLE_UUID16_INIT(0xff02);

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
    const bt_device_t   *device;      /* identified model, NULL until known */

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

    /* Characteristic handles for the BLUETTI service. */
    uint16_t             write_handle;
    uint16_t             notify_handle;
    uint16_t             notify_cccd;

    bt_session_t        *session;
    int                  poll_block;    /* index into BT_EL10_BLOCKS */

    /* One Modbus request at a time: the reference never has two in flight,
     * and a second read while the first is still coming back confuses the
     * reassembly. */
    bool                 req_in_flight;
    int64_t              req_sent_us;

    bluetti_state_t      state;
    SemaphoreHandle_t    lock;
    esp_timer_handle_t   scan_timer;
    esp_timer_handle_t   poll_timer;
    esp_timer_handle_t   kex_timer;     /* one-shot: plain-Modbus fallback */
} b;

static void start_connect_scan(void);
static void poll_timer_cb(void *arg);
static void kex_timer_cb(void *arg);
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
    /* A supported model is an exact answer; the loose prefixes below still
     * surface other BLUETTI kit so it appears in the picker rather than
     * looking like the bridge cannot see it. */
    char buf[33];
    if (len < sizeof(buf)) {
        memcpy(buf, name, len);
        buf[len] = '\0';
        if (bt_device_lookup(buf)) {
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(BT_PREFIXES) / sizeof(BT_PREFIXES[0]); i++) {
        size_t pl = strlen(BT_PREFIXES[i]);
        if (len >= pl && strncmp(name, BT_PREFIXES[i], pl) == 0) {
            return true;
        }
    }
    return false;
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
/* Session plumbing                                                     */
/* ------------------------------------------------------------------ */

/* Session -> radio. Write-without-response: the device does not ack. */
static int session_tx(const uint8_t *data, size_t len, void *user)
{
    if (!b.connected || b.write_handle == 0) {
        return -1;
    }
    int rc = ble_gattc_write_no_rsp_flat(b.conn_handle, b.write_handle,
                                         data, len);
    if (rc != 0) {
        ESP_LOGW(TAG, "write failed: %d", rc);
        return -1;
    }
    return 0;
}

/* Session -> decoder, once a Modbus response passes CRC. */
static void session_regs(uint16_t addr, const uint8_t *data, size_t len,
                         void *user)
{
    bluetti_state_t copy;
    int matched;

    xSemaphoreTake(b.lock, portMAX_DELAY);
    matched = bt_regs_apply(b.device, addr, data, len, &b.state);
    /* The unit names itself in register 110, so the map can be picked from
     * what it reports rather than from what we were told to look for. */
    if (!b.device && b.state.model[0]) {
        b.device = bt_device_lookup(b.state.model);
        if (b.device) {
            ESP_LOGI(TAG, "identified as %s", b.device->name);
        } else {
            ESP_LOGW(TAG, "unknown model '%s' - charge and power only",
                     b.state.model);
            b.device = &BT_DEVICE_GENERIC;
        }
        /* Re-run now that the optional fields are known to be readable. */
        bt_regs_apply(b.device, addr, data, len, &b.state);
    }
    b.state.soc_low_pct = b.cfg.low_battery_pct;
    copy = b.state;
    xSemaphoreGive(b.lock);

    b.req_in_flight = false;      /* response arrived; next block may go */

    if (matched > 0 && b.state_cb) {
        b.state_cb(&copy, b.state_cb_user);
    }
}

/* Walk the register blocks one per tick, so a slow device never has more
 * than one outstanding request. */
#define REQ_TIMEOUT_US (6 * 1000 * 1000LL)

static void poll_timer_cb(void *arg)
{
    if (!b.connected || !bt_session_ready(b.session)) {
        return;
    }
    /* Hold off while a request is still outstanding, unless it has been
     * long enough that the response is not coming. */
    if (b.req_in_flight) {
        if (esp_timer_get_time() - b.req_sent_us < REQ_TIMEOUT_US) {
            return;
        }
        ESP_LOGW(TAG, "no response to the last read; moving on");
        b.req_in_flight = false;
    }
    /* Walk to the next block this model actually has fields in, so a unit
     * without AC metering does not spend two round trips a cycle on
     * registers it will never answer usefully. */
    for (size_t tries = 0; tries < BT_EL10_BLOCK_COUNT; tries++) {
        const bt_reg_block_t *blk = &BT_EL10_BLOCKS[b.poll_block];
        b.poll_block = (b.poll_block + 1) % (int)BT_EL10_BLOCK_COUNT;
        if (!bt_regs_block_wanted(b.device, blk->addr)) {
            continue;
        }
        if (bt_session_read_regs(b.session, blk->addr, blk->count) != 0) {
            ESP_LOGW(TAG, "register read %u failed", blk->addr);
            return;
        }
        b.req_in_flight = true;
        b.req_sent_us = esp_timer_get_time();
        return;
    }
}

/*
 * Armed when notifications go live. If the device has not opened the key
 * exchange by the time this fires, it is not going to — switch the session
 * to unencrypted Modbus, the way the reference library falls back after
 * its encrypted attempt times out.
 */
static void kex_timer_cb(void *arg)
{
    if (b.connected && bt_session_state(b.session) == BT_SESS_IDLE) {
        bt_session_use_plain(b.session);
    }
}

/* ---- discovery: find ff01/ff02 and subscribe ---- */

static int on_sub(uint16_t conn, const struct ble_gatt_error *err,
                  struct ble_gatt_attr *attr, void *arg)
{
    if (err->status != 0) {
        ESP_LOGE(TAG, "subscribe failed: %d", err->status);
        return 0;
    }
    ESP_LOGI(TAG, "subscribed; awaiting the device challenge");
    /* The device opens the key exchange on its own once notifications are
     * live, so there is nothing to send here. Start the poll timer: it
     * no-ops until the session reports ready. */
    if (b.poll_timer) {
        esp_timer_stop(b.poll_timer);
        uint32_t period = b.cfg.poll_interval_ms ? b.cfg.poll_interval_ms : 5000;
        /* Spread the blocks across the interval. */
        esp_timer_start_periodic(b.poll_timer,
            (uint64_t)(period / BT_EL10_BLOCK_COUNT + 1) * 1000);
    }
    /* ...but not every unit encrypts. If no challenge lands in this window,
     * fall back to plain Modbus rather than waiting forever. */
    if (b.kex_timer && !b.cfg.probe) {
        esp_timer_stop(b.kex_timer);
        esp_timer_start_once(b.kex_timer, 12 * 1000 * 1000ULL);
    }
    return 0;
}

static int on_disc_notify_chr(uint16_t conn, const struct ble_gatt_error *err,
                              const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        b.notify_handle = chr->val_handle;
        b.notify_cccd = chr->val_handle + 1;   /* CCCD convention */
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (b.notify_cccd == 0 || b.write_handle == 0) {
            ESP_LOGE(TAG, "ff01/ff02 not found — is this a BLUETTI? "
                          "Try probe mode.");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        uint8_t enable[2] = { 0x01, 0x00 };
        if (ble_gattc_write_flat(conn, b.notify_cccd, enable, sizeof(enable),
                                 on_sub, NULL) != 0) {
            ESP_LOGE(TAG, "could not enable notifications");
        }
    }
    return 0;
}

static int on_disc_write_chr(uint16_t conn, const struct ble_gatt_error *err,
                             const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        b.write_handle = chr->val_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        ble_gattc_disc_chrs_by_uuid(conn, 1, 0xffff, &NOTIFY_UUID.u,
                                    on_disc_notify_chr, NULL);
    }
    return 0;
}

static int on_disc_svc(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) {
        ESP_LOGI(TAG, "found BLUETTI service, handles %d..%d",
                 svc->start_handle, svc->end_handle);
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        ble_gattc_disc_chrs_by_uuid(conn, 1, 0xffff, &WRITE_UUID.u,
                                    on_disc_write_chr, NULL);
    } else if (err->status != 0) {
        ESP_LOGE(TAG, "service discovery error: %d", err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
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
            (void)f;
            if (memcmp(event->disc.addr.val, b.target_addr.val, 6) == 0) {
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
            b.write_handle = b.notify_handle = b.notify_cccd = 0;
            b.poll_block = 0;
            b.req_in_flight = false;
            bt_session_reset(b.session);
        b.device = NULL;   /* re-identify on the next connect */
            ble_gattc_exchange_mtu(b.conn_handle, NULL, NULL);
            if (b.cfg.probe) {
                ESP_LOGW(TAG, "probe mode: enumerating GATT, nothing decoded");
                ble_gattc_disc_all_svcs(b.conn_handle, on_probe_svc, NULL);
            } else {
                ESP_LOGI(TAG, "connected, discovering the BLUETTI service");
                ble_gattc_disc_svc_by_uuid(b.conn_handle, &SVC_UUID.u,
                                           on_disc_svc, NULL);
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
        if (b.poll_timer) {
            esp_timer_stop(b.poll_timer);
        }
        if (b.kex_timer) {
            esp_timer_stop(b.kex_timer);
        }
        bt_session_reset(b.session);
        b.device = NULL;   /* re-identify on the next connect */
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
        if (b.cfg.probe) {
            ESP_LOGI(TAG, "notify handle=%d len=%u",
                     event->notify_rx.attr_handle, (unsigned)len);
            ESP_LOG_BUFFER_HEX(TAG, tmp, len);
            return 0;
        }
        if (event->notify_rx.attr_handle == b.notify_handle && b.session) {
            bt_session_feed(b.session, tmp, len);
        }
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
    esp_timer_create_args_t pt = { .callback = poll_timer_cb, .name = "bt_poll" };
    esp_timer_create(&pt, &b.poll_timer);
    esp_timer_create_args_t kt = { .callback = kex_timer_cb, .name = "bt_kex" };
    esp_timer_create(&kt, &b.kex_timer);

    b.session = bt_session_new(session_tx, session_regs, NULL);
    if (!b.session) {
        ESP_LOGE(TAG, "session alloc failed");
        return -1;
    }

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
    b.device = NULL;
    b.state.soc_low_pct = config->low_battery_pct;
    b.state.minutes_remaining = BLUETTI_UNKNOWN_I;
    b.state.ac_in_watts = BLUETTI_UNKNOWN_F;
    b.state.ac_out_watts = BLUETTI_UNKNOWN_F;
    b.state.ac_in_volts = BLUETTI_UNKNOWN_F;
    b.state.ac_in_amps = BLUETTI_UNKNOWN_F;
    b.state.ac_out_volts = BLUETTI_UNKNOWN_F;
    b.state.ac_switch = BLUETTI_UNKNOWN_I;
    b.state.dc_switch = BLUETTI_UNKNOWN_I;
    xSemaphoreGive(b.lock);

    /* An address is the only way to name a unit, so a bad one is fatal
     * rather than something to fall back from. */
    b.have_target_addr = config->ble_address && config->ble_address[0] &&
                         parse_addr(config->ble_address, &b.target_addr);
    if (!b.have_target_addr) {
        ESP_LOGE(TAG, "no usable BLE address ('%s')",
                 config->ble_address ? config->ble_address : "");
        return -1;
    }
    ESP_LOGI(TAG, "target address %s", config->ble_address);

    if (config->probe) {
        ESP_LOGW(TAG, "PROBE MODE: GATT and notifications will be logged, "
                      "nothing will be decoded");
    } else {
        ESP_LOGI(TAG, "polling %u register blocks every %u ms",
                 (unsigned)BT_EL10_BLOCK_COUNT,
                 (unsigned)(config->poll_interval_ms ?: 5000));
    }

    b.mode = MODE_CONNECT;
    start_connect_scan();
    return 0;
}
