/*
 * NimBLE central for EcoFlow power stations.
 *
 *   ecoflow_ble_host_init()   - start the BLE host, do nothing else
 *   ecoflow_ble_scan()        - list nearby devices (used by provisioning)
 *   ecoflow_ble_start()       - connect to the configured unit and stream
 *                               telemetry: scan -> connect -> discover ->
 *                               subscribe -> (notifications) -> [reconnect]
 */

#include "ecoflow_ble.h"
#include "ef_session.h"
#include "ef_crypto.h"

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

static const char *TAG = "ecoflow_ble";

/* Nordic UART clone UUIDs, 128-bit little-endian byte order. */
static const ble_uuid128_t UART_SVC_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UART_TX_UUID = BLE_UUID128_INIT(  /* notify dev->esp */
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UART_RX_UUID = BLE_UUID128_INIT(  /* write  esp->dev */
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

/* Advertised-name prefixes EcoFlow units are known to use (best-effort;
 * only drives the "looks like an EcoFlow" hint in the scan list). */
static const char *EF_PREFIXES[] = { "EF-", "EcoFlow", "DELTA", "RIVER" };

typedef enum { MODE_IDLE, MODE_SCAN, MODE_CONNECT } ble_mode_t;

static struct {
    ecoflow_ble_config_t cfg;
    ecoflow_state_cb_t   state_cb;
    void                *state_cb_user;

    ecoflow_scan_cb_t    scan_cb;
    void                *scan_cb_user;
    uint8_t              seen[24][6];
    int                  seen_n;

    ble_addr_t           target_addr;
    bool                 have_target_addr;
    char                 name_prefix[24];

    ble_mode_t           mode;
    bool                 synced;
    SemaphoreHandle_t    sync_sem;

    uint8_t              own_addr_type;
    uint16_t             conn_handle;
    uint16_t             tx_val_handle;
    uint16_t             rx_val_handle;
    uint16_t             tx_cccd_handle;

    bool                 connected;
    bool                 subscribed;

    char                 target_sn[20];
    int                  target_encrypt;

    ef_session_t        *session;

    ecoflow_state_t      state;
    SemaphoreHandle_t    lock;
    esp_timer_handle_t   tick_timer;    /* 1 Hz session tick */
    esp_timer_handle_t   scan_timer;
} b;

static void start_connect_scan(void);
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

static bool name_has_ef_prefix(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(EF_PREFIXES) / sizeof(EF_PREFIXES[0]); i++) {
        size_t pl = strlen(EF_PREFIXES[i]);
        if (len >= pl && strncmp(name, EF_PREFIXES[i], pl) == 0) {
            return true;
        }
    }
    return false;
}

/* EcoFlow BLE advert manufacturer data (company 0xB5B5):
 *   [0..1] company id  [2] proto ver  [3..18] serial (16)  [24] cap flags
 * where encrypt_type = (cap_flags >> 3) & 0x07.  Returns false if absent. */
static bool parse_mfg(const struct ble_hs_adv_fields *f, char sn_out[20],
                      int *encrypt_out)
{
    if (f->mfg_data_len < 19) {
        return false;
    }
    if (!(f->mfg_data[0] == 0xB5 && f->mfg_data[1] == 0xB5)) {
        return false;
    }
    memcpy(sn_out, &f->mfg_data[3], 16);
    sn_out[16] = '\0';
    for (int i = 0; i < 16; i++) {
        if (sn_out[i] < 0x20 || sn_out[i] > 0x7E) {   /* not printable ASCII */
            return false;
        }
    }
    *encrypt_out = (f->mfg_data_len > 24)
                       ? (int)((f->mfg_data[24] >> 3) & 0x07)
                       : 7;
    return true;
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

static void publish_state(void)
{
    ecoflow_state_t copy;
    xSemaphoreTake(b.lock, portMAX_DELAY);
    copy = b.state;
    xSemaphoreGive(b.lock);
    if (b.state_cb) {
        b.state_cb(&copy, b.state_cb_user);
    }
}

/* ef_session -> app: a fresh telemetry snapshot was decoded. */
static void session_state_cb(const ecoflow_state_t *st, void *user)
{
    (void)user;
    xSemaphoreTake(b.lock, portMAX_DELAY);
    b.state = *st;
    if (b.state.soc_low_pct < b.cfg.low_battery_pct) {
        b.state.soc_low_pct = b.cfg.low_battery_pct;
    }
    xSemaphoreGive(b.lock);
    publish_state();
}

/* NimBLE allows only one GATT write procedure in flight, but the session
 * can emit several frames back-to-back (e.g. the time-sync trio). Queue
 * them and drain from the write-completion callback. All access is on the
 * NimBLE host task, so no locking. */
#define TXQ_DEPTH 10
#define TXQ_SLOT  256

static struct {
    uint8_t buf[TXQ_DEPTH][TXQ_SLOT];
    size_t  len[TXQ_DEPTH];
    int     head, count;
    bool    in_flight;
} txq;

static int on_session_write_done(uint16_t conn, const struct ble_gatt_error *err,
                                 struct ble_gatt_attr *attr, void *arg);

static void txq_pump(void)
{
    if (txq.in_flight || txq.count == 0 || !b.connected || b.rx_val_handle == 0) {
        return;
    }
    int rc = ble_gattc_write_flat(b.conn_handle, b.rx_val_handle,
                                  txq.buf[txq.head], txq.len[txq.head],
                                  on_session_write_done, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "gatt write start failed: %d, dropping frame", rc);
        txq.head = (txq.head + 1) % TXQ_DEPTH;
        txq.count--;
        return;
    }
    txq.in_flight = true;
}

static int on_session_write_done(uint16_t conn, const struct ble_gatt_error *err,
                                 struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err && err->status != 0) {
        ESP_LOGD(TAG, "gatt write status %d", err->status);
    }
    if (txq.count > 0) {
        txq.head = (txq.head + 1) % TXQ_DEPTH;
        txq.count--;
    }
    txq.in_flight = false;
    txq_pump();
    return 0;
}

static void txq_reset(void)
{
    txq.head = txq.count = 0;
    txq.in_flight = false;
}

/* ef_session -> device: queue bytes for the RX characteristic. */
static int session_write(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (len == 0 || len > TXQ_SLOT) {
        return -1;
    }
    if (txq.count >= TXQ_DEPTH) {
        ESP_LOGW(TAG, "tx queue full, dropping frame");
        return -1;
    }
    int slot = (txq.head + txq.count) % TXQ_DEPTH;
    memcpy(txq.buf[slot], data, len);
    txq.len[slot] = len;
    txq.count++;
    txq_pump();
    return 0;
}

static void begin_session(void)
{
    if (!b.session) {
        b.session = ef_session_create();
    }
    if (!b.session) {
        ESP_LOGE(TAG, "no memory for session");
        return;
    }
    ESP_LOGI(TAG, "starting EcoFlow V2 handshake (sn=%s, enc=%d)",
             b.target_sn, b.target_encrypt);
    ef_session_begin(b.session, b.target_sn, b.cfg.user_id, b.target_encrypt,
                     b.cfg.low_battery_pct,
                     session_write, NULL, session_state_cb, NULL);
    if (b.tick_timer) {
        esp_timer_start_periodic(b.tick_timer, 1000 * 1000);
    }
}

/* ------------------------------------------------------------------ */
/* GATT discovery                                                      */
/* ------------------------------------------------------------------ */

static int on_subscribe(uint16_t conn, const struct ble_gatt_error *err,
                        struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0) {
        ESP_LOGI(TAG, "subscribed to telemetry notifications");
        b.subscribed = true;
        begin_session();
    } else {
        ESP_LOGE(TAG, "subscribe failed: %d", err->status);
    }
    return 0;
}

static int on_disc_tx_chr(uint16_t conn, const struct ble_gatt_error *err,
                          const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        b.tx_val_handle = chr->val_handle;
        b.tx_cccd_handle = chr->val_handle + 1;  /* CCCD convention */
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (b.tx_cccd_handle == 0) {
            ESP_LOGE(TAG, "TX characteristic not found");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        uint8_t enable[2] = { 0x01, 0x00 };
        int rc = ble_gattc_write_flat(conn, b.tx_cccd_handle,
                                      enable, sizeof(enable), on_subscribe, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "write CCCD failed: %d", rc);
        }
    }
    return 0;
}

static int on_disc_rx_chr(uint16_t conn, const struct ble_gatt_error *err,
                          const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        b.rx_val_handle = chr->val_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        int rc = ble_gattc_disc_chrs_by_uuid(conn, 1, 0xffff, &UART_TX_UUID.u,
                                             on_disc_tx_chr, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "disc TX chr failed: %d", rc);
        }
    }
    return 0;
}

static int on_disc_svc(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) {
        ESP_LOGI(TAG, "UART service handles %d..%d",
                 svc->start_handle, svc->end_handle);
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        int rc = ble_gattc_disc_chrs_by_uuid(conn, 1, 0xffff, &UART_RX_UUID.u,
                                             on_disc_rx_chr, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "disc RX chr failed: %d", rc);
        }
    } else if (err->status != 0) {
        ESP_LOGE(TAG, "service discovery error: %d", err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Poll timer                                                          */
/* ------------------------------------------------------------------ */

static void tick_timer_cb(void *arg)
{
    if (!b.connected || !b.session) {
        return;
    }
    ef_session_tick(b.session);
    if (ef_session_state(b.session) == EF_SESS_ERROR) {
        ESP_LOGW(TAG, "session failed (%s); dropping link to retry",
                 ef_session_error(b.session));
        esp_timer_stop(b.tick_timer);
        ble_gap_terminate(b.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void scan_timer_cb(void *arg)
{
    ecoflow_ble_scan_stop();
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
    ecoflow_scan_entry_t e = { .rssi = event->disc.rssi };
    addr_to_str(event->disc.addr.val, e.addr);
    if (f.name_len) {
        size_t n = f.name_len < sizeof(e.name) - 1 ? f.name_len
                                                   : sizeof(e.name) - 1;
        memcpy(e.name, f.name, n);
        e.name[n] = '\0';
        e.looks_like_ecoflow = name_has_ef_prefix((const char *)f.name,
                                                  f.name_len);
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
                /* Capture serial + encrypt type from the advert; keep any
                 * value from a previous (scan-response) advert if this one
                 * lacks manufacturer data. */
                char sn[20];
                int enc;
                if (parse_mfg(&f, sn, &enc)) {
                    strlcpy(b.target_sn, sn, sizeof(b.target_sn));
                    b.target_encrypt = enc;
                }
                ESP_LOGI(TAG, "found target, connecting");
                try_connect(&event->disc.addr);
            }
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            b.conn_handle = event->connect.conn_handle;
            b.connected = true;
            b.subscribed = false;
            b.tx_val_handle = b.rx_val_handle = b.tx_cccd_handle = 0;
            txq_reset();
            if (b.session) {
                ef_session_reset(b.session);
            }
            ble_gattc_exchange_mtu(b.conn_handle, NULL, NULL);
            ESP_LOGI(TAG, "connected, discovering services");
            ble_gattc_disc_svc_by_uuid(b.conn_handle, &UART_SVC_UUID.u,
                                       on_disc_svc, NULL);
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            start_connect_scan();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
        b.connected = false;
        b.subscribed = false;
        txq_reset();
        if (b.tick_timer) {
            esp_timer_stop(b.tick_timer);
        }
        if (b.session) {
            ef_session_reset(b.session);
        }
        if (b.mode == MODE_CONNECT) {
            start_connect_scan();
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.conn_handle != b.conn_handle ||
            event->notify_rx.attr_handle != b.tx_val_handle) {
            return 0;
        }
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t tmp[512];
        if (len > sizeof(tmp)) {
            len = sizeof(tmp);
        }
        if (ble_hs_mbuf_to_flat(event->notify_rx.om, tmp, len, &len) != 0) {
            return 0;
        }
        if (b.session) {
            ef_session_feed(b.session, tmp, len);
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
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Scan / connect entry points                                         */
/* ------------------------------------------------------------------ */

static void start_disc(bool want_names_only)
{
    struct ble_gap_disc_params dp = {
        .passive = 0,
        .filter_duplicates = want_names_only ? 0 : 1,
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
    ESP_LOGI(TAG, "scanning for target EcoFlow");
    start_disc(false);
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

int ecoflow_ble_host_init(void)
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

    esp_timer_create_args_t pt = { .callback = tick_timer_cb, .name = "ef_tick" };
    esp_timer_create(&pt, &b.tick_timer);
    esp_timer_create_args_t st = { .callback = scan_timer_cb, .name = "ef_scan" };
    esp_timer_create(&st, &b.scan_timer);

    ef_crypto_init();

    /* nimble_port_init() returns void on IDF <5.2 and esp_err_t on newer;
     * call it without capturing so this builds either way. */
    nimble_port_init();

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gap_device_name_set("esp32-nut-ecoflow");

    nimble_port_freertos_init(host_task);
    inited = true;

    if (xSemaphoreTake(b.sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "BLE host not synced after 5s");
        return -1;
    }
    return 0;
}

int ecoflow_ble_scan(uint32_t duration_ms, ecoflow_scan_cb_t cb, void *user)
{
    if (b.mode == MODE_CONNECT) {
        return -1;
    }
    b.scan_cb = cb;
    b.scan_cb_user = user;
    b.seen_n = 0;
    b.mode = MODE_SCAN;

    if (b.synced) {
        start_disc(true);
    } /* else on_sync() will start it */

    esp_timer_stop(b.scan_timer);
    esp_timer_start_once(b.scan_timer, (uint64_t)duration_ms * 1000);
    return 0;
}

void ecoflow_ble_scan_stop(void)
{
    esp_timer_stop(b.scan_timer);
    ble_gap_disc_cancel();
    if (b.mode == MODE_SCAN) {
        b.mode = MODE_IDLE;
    }
}

bool ecoflow_ble_scanning(void)
{
    return b.mode == MODE_SCAN;
}

bool ecoflow_ble_get_state(ecoflow_state_t *out)
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

bool ecoflow_ble_connected(void)
{
    return b.connected;
}

int ecoflow_ble_start(const ecoflow_ble_config_t *config,
                      ecoflow_state_cb_t cb, void *user)
{
    if (!config) {
        return -1;
    }
    if (ecoflow_ble_host_init() != 0) {
        return -1;
    }

    ecoflow_ble_scan_stop();

    b.cfg = *config;
    b.state_cb = cb;
    b.state_cb_user = user;

    xSemaphoreTake(b.lock, portMAX_DELAY);
    memset(&b.state, 0, sizeof(b.state));
    b.state.soc_low_pct = config->low_battery_pct;
    b.state.minutes_remaining = ECOFLOW_UNKNOWN_I;
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
                    ? config->ble_name_prefix : "EF-",
                sizeof(b.name_prefix));
        ESP_LOGI(TAG, "target name prefix '%s'", b.name_prefix);
    }

    b.mode = MODE_CONNECT;
    start_connect_scan();
    return 0;
}
