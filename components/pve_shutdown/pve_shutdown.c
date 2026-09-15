/*
 * The runtime half of pve_shutdown: a pinned-certificate HTTPS client for
 * the Proxmox API, the shutdown sequence, and the glue that feeds
 * observations into the engine. The decision itself is in pve_engine.c.
 *
 * Why a hand-rolled client rather than esp_http_client: Proxmox's
 * certificate is self-signed, so the choice is between not verifying it
 * at all and verifying it against something the user pins. esp-tls can
 * only do the former (and only via a Kconfig that loosens every client
 * without a CA). Plain mbedTLS lets us complete the handshake, hash the
 * certificate we were shown, and refuse to send a byte of the request —
 * token included — unless that hash is the pinned one.
 */
#include "pve_shutdown.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "pve";

/* pve-ups retries a failed node shutdown three times: a 503 from a busy
 * pveproxy, or a request that just timed out, is not evidence that the
 * node is going down. */
#define NODE_ATTEMPTS   3
#define RETRY_GAP_MS    5000
#define IO_TIMEOUT_MS   15000
#define RESP_MAX        2048

static struct {
    pve_config_t      cfg;
    pve_engine_t      eng;
    pve_event_cb_t    cb;
    void             *cb_user;
    SemaphoreHandle_t lock;
    bool              started;
    bool              running;     /* a sequence task is in flight */
    char              last[128];
    struct { char text[64]; bool ok; } host_last[PVE_MAX_HOSTS];
} P;

static void note_host(int i, bool ok, const char *text)
{
    if (i < 0 || i >= PVE_MAX_HOSTS) return;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    P.host_last[i].ok = ok;
    strlcpy(P.host_last[i].text, text, sizeof(P.host_last[i].text));
    xSemaphoreGive(P.lock);
}

void pve_shutdown_note_test(int i, bool ok, const char *msg)
{
    if (!P.started) return;
    note_host(i, ok, ok ? "test ok" : msg);
}

/* ------------------------------------------------------------------ */
/* Fingerprints                                                        */
/* ------------------------------------------------------------------ */

bool pve_fingerprint_normalise(const char *in, char out[65])
{
    int n = 0;
    for (const char *p = in; *p; p++) {
        if (*p == ':' || *p == ' ') continue;
        if (!isxdigit((unsigned char)*p) || n >= 64) return false;
        out[n++] = (char)toupper((unsigned char)*p);
    }
    out[n] = '\0';
    return n == 64;
}

static void fp_pretty(const char hex[65], char out[96])
{
    int o = 0;
    for (int i = 0; i < 64 && hex[i]; i += 2) {
        if (o) out[o++] = ':';
        out[o++] = hex[i];
        out[o++] = hex[i + 1];
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* HTTPS with a pinned certificate                                     */
/* ------------------------------------------------------------------ */

/* "https://10.0.0.10:8006" -> host, port. Only https is accepted: the
 * whole point is the certificate. */
static bool split_url(const char *url, char *host, size_t hsz, char *port, size_t psz)
{
    const char *p = url;
    if (strncmp(p, "https://", 8) != 0) return false;
    p += 8;
    size_t n = strcspn(p, ":/");
    if (n == 0 || n >= hsz) return false;
    memcpy(host, p, n); host[n] = '\0';
    p += n;
    if (*p == ':') {
        p++;
        size_t m = strcspn(p, "/");
        if (m == 0 || m >= psz) return false;
        memcpy(port, p, m); port[m] = '\0';
    } else {
        strlcpy(port, "8006", psz);
    }
    return true;
}

typedef struct {
    int  status;          /* HTTP status; 0 = handshake only; <0 = error */
    char seen_fp[65];     /* what the server presented                   */
} req_result_t;

/*
 * One request. Handshake, pin check, then — only if the pin matches —
 * the request. With no fingerprint configured the function stops after
 * reporting what it saw (status 0), so a first Test never leaks the token
 * to whatever answered. `body` non-NULL makes it a POST.
 */
static req_result_t tls_request(const pve_host_t *h, const char *path,
                                const char *body, char *resp, size_t resp_sz,
                                char *err, size_t err_sz)
{
    req_result_t r = { .status = -1 };
    char host[64], port[8];
    if (!split_url(h->url, host, sizeof(host), port, sizeof(port))) {
        snprintf(err, err_sz, "URL must be https://host[:port]");
        return r;
    }

    mbedtls_net_context  net;
    mbedtls_ssl_context  ssl;
    mbedtls_ssl_config   conf;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    int rc;
    if ((rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                    (const unsigned char *)"pve", 3)) != 0) {
        snprintf(err, err_sz, "rng: -0x%04x", -rc);
        goto out;
    }
    if ((rc = mbedtls_net_connect(&net, host, port, MBEDTLS_NET_PROTO_TCP)) != 0) {
        snprintf(err, err_sz, "connect %s:%s failed (-0x%04x)", host, port, -rc);
        goto out;
    }
    if ((rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        snprintf(err, err_sz, "ssl config: -0x%04x", -rc);
        goto out;
    }
    /*
     * The chain cannot be validated — there is no CA, the cert is
     * self-signed — so mbedTLS is told not to try. Verification happens
     * below instead, against the pinned fingerprint, before anything is
     * sent. This is stricter than a CA check, not looser: it accepts one
     * exact certificate.
     */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_read_timeout(&conf, IO_TIMEOUT_MS);
    if ((rc = mbedtls_ssl_setup(&ssl, &conf)) != 0 ||
        (rc = mbedtls_ssl_set_hostname(&ssl, host)) != 0) {
        snprintf(err, err_sz, "ssl setup: -0x%04x", -rc);
        goto out;
    }
    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);
    while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            snprintf(err, err_sz, "TLS handshake failed (-0x%04x)", -rc);
            goto out;
        }
    }

    /* ---- the pin ---- */
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ssl);
    if (!peer) {
        snprintf(err, err_sz, "server sent no certificate");
        goto out;
    }
    unsigned char digest[32];
    mbedtls_sha256(peer->raw.p, peer->raw.len, digest, 0);
    for (int i = 0; i < 32; i++) {
        snprintf(&r.seen_fp[i * 2], 3, "%02X", digest[i]);
    }
    if (!h->fingerprint[0]) {
        r.status = 0;                       /* not pinned: report, send nothing */
        goto out;
    }
    if (strcmp(r.seen_fp, h->fingerprint) != 0) {
        snprintf(err, err_sz, "certificate fingerprint does not match the pinned one");
        goto out;
    }

    /* ---- the request. HTTP/1.0 so the reply is never chunked. ---- */
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "%s /api2/json%s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "Authorization: PVEAPIToken=%s=%s\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "Content-Length: %u\r\n"
                     "\r\n%s",
                     body ? "POST" : "GET", path, host, h->token_id, h->secret,
                     body ? "Content-Type: application/x-www-form-urlencoded\r\n" : "",
                     (unsigned)(body ? strlen(body) : 0), body ? body : "");
    if (n < 0 || n >= (int)sizeof(req)) {
        snprintf(err, err_sz, "request too long");
        goto out;
    }
    for (int off = 0; off < n; ) {
        rc = mbedtls_ssl_write(&ssl, (const unsigned char *)req + off, n - off);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc <= 0) { snprintf(err, err_sz, "write failed (-0x%04x)", -rc); goto out; }
        off += rc;
    }
    memset(req, 0, sizeof(req));            /* the token was in there */

    /* Read to EOF, headers and all. */
    char *buf = malloc(RESP_MAX);
    if (!buf) { snprintf(err, err_sz, "out of memory"); goto out; }
    size_t got = 0;
    for (;;) {
        rc = mbedtls_ssl_read(&ssl, (unsigned char *)buf + got, RESP_MAX - 1 - got);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0) break;
        if (rc < 0) {
            if (got) break;                 /* have a reply; a rude close is fine */
            snprintf(err, err_sz, "read failed (-0x%04x)", -rc);
            free(buf);
            goto out;
        }
        got += rc;
        if (got >= RESP_MAX - 1) break;
    }
    buf[got] = '\0';

    int status = 0;
    if (sscanf(buf, "HTTP/%*d.%*d %d", &status) != 1) {
        snprintf(err, err_sz, "not an HTTP reply");
        free(buf);
        goto out;
    }
    r.status = status;
    if (resp && resp_sz) {
        const char *b = strstr(buf, "\r\n\r\n");
        strlcpy(resp, b ? b + 4 : "", resp_sz);
    }
    free(buf);

out:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&net);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return r;
}

static const char *status_text(int st)
{
    switch (st) {
    case 401: return "401 authentication failed (token invalid?)";
    case 403: return "403 permission denied (privilege missing?)";
    case 500: return "500 (node name wrong?)";
    case 501: return "501 (a Backup Server? only PVE is supported)";
    default:  return NULL;
    }
}

static void describe(int st, const char *err, char *out, size_t sz)
{
    if (st < 0)          snprintf(out, sz, "%s", err[0] ? err : "connection failed");
    else if (st == 0)    snprintf(out, sz, "certificate not pinned");
    else if (status_text(st)) snprintf(out, sz, "%s", status_text(st));
    else                 snprintf(out, sz, "HTTP %d", st);
}

/* Walk "101, 102,105" one id at a time. */
static const char *next_guest(const char *p, int *id)
{
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return NULL;
    *id = atoi(p);
    while (isdigit((unsigned char)*p)) p++;
    return p;
}

static void emit(const char *text)
{
    ESP_LOGW(TAG, "%s", text);
    if (P.cb) {
        P.cb(text, P.cb_user);
    }
}

/* ------------------------------------------------------------------ */
/* The sequence                                                        */
/* ------------------------------------------------------------------ */

/* A guest could be a VM or a container and we are not told which. Try
 * qemu, then lxc; whichever the id is, the other answers 500. */
static bool shutdown_guest(const pve_host_t *h, int id, char *why, size_t wsz)
{
    static const char *KIND[] = { "qemu", "lxc" };
    req_result_t r = { .status = -1 };
    char err[64] = "";
    for (size_t k = 0; k < 2; k++) {
        char path[96];
        snprintf(path, sizeof(path), "/nodes/%s/%s/%d/status/shutdown",
                 h->node, KIND[k], id);
        r = tls_request(h, path, "", NULL, 0, err, sizeof(err));
        if (r.status == 200) {
            return true;
        }
        if (r.status <= 0 || r.status == 401 || r.status == 403) {
            break;                  /* not a "wrong kind" answer */
        }
    }
    char d[80];
    describe(r.status, err, d, sizeof(d));
    snprintf(why, wsz, "guest %d: %s", id, d);
    return false;
}

/* pve-ups: the configured name first, then /nodes/localhost — the machine
 * behind the URL is the one being shut down, and a token scoped to
 * /nodes/<name> refuses one form while a misspelled name refuses the other. */
static bool shutdown_node(const pve_host_t *h, char *why, size_t wsz)
{
    const char *names[2] = { h->node, "localhost" };
    req_result_t r = { .status = -1 };
    char err[64] = "";
    for (int attempt = 0; attempt < NODE_ATTEMPTS; attempt++) {
        for (int n = 0; n < 2; n++) {
            if (n == 1 && strcmp(h->node, "localhost") == 0) continue;
            char path[64];
            snprintf(path, sizeof(path), "/nodes/%s/status", names[n]);
            r = tls_request(h, path, "command=shutdown", NULL, 0, err, sizeof(err));
            if (r.status == 200) {
                return true;
            }
            ESP_LOGW(TAG, "%s: /nodes/%s/status -> %d %s", h->node, names[n],
                     r.status, err);
            if (r.status <= 0) break;   /* no point trying the other name */
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_GAP_MS));
    }
    char d[80];
    describe(r.status, err, d, sizeof(d));
    snprintf(why, wsz, "node: %s", d);
    return false;
}

static void sequence_task(void *arg)
{
    pve_config_t cfg;
    char reason[sizeof(P.eng.reason)];
    xSemaphoreTake(P.lock, portMAX_DELAY);
    cfg = P.cfg;
    strlcpy(reason, P.eng.reason, sizeof(reason));
    xSemaphoreGive(P.lock);

    char text[240];
    int ok = 0, failed = 0;
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        const pve_host_t *h = &cfg.hosts[i];
        if (!h->enabled || !h->url[0]) {
            continue;
        }
        if (!cfg.armed) {
            snprintf(text, sizeof(text), "DRY RUN \xE2\x80\x94 would shut down %s%s%s (%s)",
                     h->node, h->guests[0] ? " after guests " : "",
                     h->guests[0] ? h->guests : "", reason);
            emit(text);
            note_host(i, true, "dry run: would shut down");
            continue;
        }
        if (!h->fingerprint[0]) {
            failed++;
            snprintf(text, sizeof(text), "\xE2\x9D\x8C Proxmox %s NOT shut down: "
                     "certificate not pinned (use Test on the Proxmox tab)", h->node);
            emit(text);
            note_host(i, false, "not pinned \xE2\x80\x94 skipped");
            continue;
        }

        char why[96] = "";
        bool good = true;
        int id;
        for (const char *p = h->guests; (p = next_guest(p, &id)) != NULL; ) {
            if (!shutdown_guest(h, id, why, sizeof(why))) {
                good = false;
                ESP_LOGE(TAG, "%s: %s", h->node, why);
            } else {
                ESP_LOGW(TAG, "%s: guest %d shutting down", h->node, id);
            }
        }
        if (h->guests[0] && h->guest_wait_s) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)h->guest_wait_s * 1000));
        }
        if (h->shutdown_node) {
            good = shutdown_node(h, why, sizeof(why)) && good;
        }
        if (good) {
            ok++;
            snprintf(text, sizeof(text), "\xF0\x9F\x94\xBB Shutting down Proxmox %s (%s)",
                     h->node, reason);
            note_host(i, true, "shutdown sent");
        } else {
            failed++;
            snprintf(text, sizeof(text), "\xE2\x9D\x8C Proxmox %s shutdown FAILED \xE2\x80\x94 %s",
                     h->node, why);
            note_host(i, false, why);
        }
        emit(text);
        if (cfg.host_delay_s) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)cfg.host_delay_s * 1000));
        }
    }

    xSemaphoreTake(P.lock, portMAX_DELAY);
    if (!cfg.armed) {
        snprintf(P.last, sizeof(P.last), "dry run: %s", reason);
    } else {
        snprintf(P.last, sizeof(P.last), "%d host%s ok, %d failed (%s)",
                 ok, ok == 1 ? "" : "s", failed, reason);
    }
    P.running = false;
    xSemaphoreGive(P.lock);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

int pve_shutdown_start(const pve_config_t *cfg, pve_event_cb_t cb, void *user)
{
    if (P.started) {
        return 0;
    }
    P.lock = xSemaphoreCreateMutex();
    if (!P.lock) {
        return -1;
    }
    P.cfg = *cfg;
    P.cb = cb;
    P.cb_user = user;
    memset(&P.eng, 0, sizeof(P.eng));
    P.started = true;
    if (cfg->enabled) {
        int hosts = 0;
        for (int i = 0; i < PVE_MAX_HOSTS; i++) hosts += cfg->hosts[i].enabled;
        ESP_LOGI(TAG, "shutdown %s: on battery %u min, charge <= %u%%%s, %d host(s)",
                 cfg->armed ? "ARMED" : "in DRY RUN",
                 (unsigned)cfg->on_battery_min, (unsigned)cfg->charge_pct,
                 cfg->on_low_battery ? ", LB" : "", hosts);
    }
    return 0;
}

void pve_shutdown_reconfigure(const pve_config_t *cfg)
{
    if (!P.started) {
        return;
    }
    xSemaphoreTake(P.lock, portMAX_DELAY);
    P.cfg = *cfg;
    xSemaphoreGive(P.lock);
}

void pve_shutdown_observe(const char *ups_status, int soc_pct)
{
    if (!P.started) {
        return;
    }
    pve_obs_t obs;
    pve_obs_from_status(ups_status, soc_pct, &obs);

    xSemaphoreTake(P.lock, portMAX_DELAY);
    bool fire = pve_eval(&P.cfg, &P.eng, &obs, esp_timer_get_time());
    bool start = fire && !P.running;
    if (start) {
        P.running = true;
    }
    xSemaphoreGive(P.lock);

    if (!start) {
        return;
    }
    ESP_LOGW(TAG, "trigger: %s (%s)", P.eng.reason,
             P.cfg.armed ? "ARMED" : "dry run");
    /* TLS in-task; the mbedTLS handshake wants a comfortable stack. */
    if (xTaskCreate(sequence_task, "pve_seq", 10240, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the shutdown sequence");
        xSemaphoreTake(P.lock, portMAX_DELAY);
        P.running = false;
        xSemaphoreGive(P.lock);
    }
}

void pve_shutdown_status(pve_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->countdown_s = -1;
    out->on_battery_s = -1;
    if (!P.started) {
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    out->enabled = P.cfg.enabled;
    out->armed = P.cfg.armed;
    out->fired = P.eng.fired;
    out->countdown_s = pve_countdown_s(&P.cfg, &P.eng, now);
    if (P.eng.on_battery) {
        out->on_battery_s = (int)((now - P.eng.on_battery_since_us) / 1000000LL);
    }
    strlcpy(out->last, P.last, sizeof(out->last));
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        const pve_host_t *h = &P.cfg.hosts[i];
        pve_host_status_t *o = &out->hosts[i];
        strlcpy(o->node, h->node, sizeof(o->node));
        o->enabled = h->enabled && h->url[0];
        o->pinned = h->fingerprint[0] != '\0';
        strlcpy(o->last, P.host_last[i].text, sizeof(o->last));
        o->last_ok = P.host_last[i].ok;
    }
    xSemaphoreGive(P.lock);
}

/* ------------------------------------------------------------------ */
/* Test                                                                */
/* ------------------------------------------------------------------ */

/* Does /access/permissions grant `priv` on `path`? The reply is
 * {"data":{"/nodes":{"Sys.PowerMgmt":1,...},...}}. */
static bool has_priv(cJSON *data, const char *path, const char *priv)
{
    cJSON *p = cJSON_GetObjectItem(data, path);
    if (!p) return false;
    cJSON *v = cJSON_GetObjectItem(p, priv);
    return v && ((cJSON_IsNumber(v) && v->valueint) || cJSON_IsTrue(v));
}

int pve_shutdown_test(const pve_host_t *h, char *msg, size_t msg_sz,
                      char seen_fp[96])
{
    char err[80] = "";
    seen_fp[0] = '\0';
    /* Runs on the web server's task: keep the reply off its stack. */
    char *resp = malloc(RESP_MAX);
    if (!resp) {
        snprintf(msg, msg_sz, "out of memory");
        return -1;
    }
    int ret = -1;

    req_result_t r = tls_request(h, "/version", NULL, resp, RESP_MAX,
                                 err, sizeof(err));
    if (r.status == 0) {
        fp_pretty(r.seen_fp, seen_fp);
        snprintf(msg, msg_sz, "Connected. Certificate fingerprint %s \xE2\x80\x94 "
                 "check it against Proxmox (System \xE2\x80\xBA Certificates), "
                 "pin it, and test again. Nothing was sent.", seen_fp);
        ret = 1;
        goto done;
    }
    if (r.status != 200) {
        char d[80];
        describe(r.status, err, d, sizeof(d));
        snprintf(msg, msg_sz, "GET /version: %s", d);
        ret = -1;
        goto done;
    }
    char ver[24] = "?";
    cJSON *j = cJSON_Parse(resp);
    if (j) {
        cJSON *d = cJSON_GetObjectItem(j, "data");
        cJSON *v = d ? cJSON_GetObjectItem(d, "version") : NULL;
        if (cJSON_IsString(v)) strlcpy(ver, v->valuestring, sizeof(ver));
        cJSON_Delete(j);
    }

    r = tls_request(h, "/access/permissions", NULL, resp, RESP_MAX,
                    err, sizeof(err));
    if (r.status != 200) {
        snprintf(msg, msg_sz, "PVE %s reachable, but /access/permissions: HTTP %d",
                 ver, r.status);
        ret = -1;
        goto done;
    }
    j = cJSON_Parse(resp);
    cJSON *d = j ? cJSON_GetObjectItem(j, "data") : NULL;
    if (!d) {
        if (j) cJSON_Delete(j);
        snprintf(msg, msg_sz, "PVE %s reachable, permissions reply unreadable "
                              "(too long? the token should carry one role)", ver);
        ret = -1;
        goto done;
    }

    char nodepath[48];
    snprintf(nodepath, sizeof(nodepath), "/nodes/%s", h->node);
    bool node_ok = !h->shutdown_node ||
                   has_priv(d, "/", "Sys.PowerMgmt") ||
                   has_priv(d, "/nodes", "Sys.PowerMgmt") ||
                   has_priv(d, nodepath, "Sys.PowerMgmt");
    int guests_missing = 0, guests = 0, id;
    for (const char *p = h->guests; (p = next_guest(p, &id)) != NULL; ) {
        char vp[24];
        snprintf(vp, sizeof(vp), "/vms/%d", id);
        guests++;
        if (!has_priv(d, "/", "VM.PowerMgmt") && !has_priv(d, "/vms", "VM.PowerMgmt") &&
            !has_priv(d, vp, "VM.PowerMgmt")) {
            guests_missing++;
        }
    }
    cJSON_Delete(j);

    if (node_ok && !guests_missing) {
        snprintf(msg, msg_sz, "PVE %s: certificate pinned, Sys.PowerMgmt ok%s", ver,
                 guests ? ", VM.PowerMgmt ok for the guests" : "");
        ret = 0;
        goto done;
    }
    snprintf(msg, msg_sz, "PVE %s reachable, but %s%s%s", ver,
             node_ok ? "" : "Sys.PowerMgmt not confirmed on /nodes",
             (!node_ok && guests_missing) ? "; " : "",
             guests_missing ? "VM.PowerMgmt missing for some guests" : "");
    ret = -1;
done:
    free(resp);
    return ret;
}
