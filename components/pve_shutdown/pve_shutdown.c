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
/* A guest listing carries ~250 bytes per guest; 8 KB covers ~30. */
#define PVE_GUEST_RESP_MAX 8192

/* One host's pending guest work, queued from pve_shutdown_observe() and
 * drained by the worker. Unbounded (realloc'd), unlike the old per-host
 * bitmask — there's no cap on how many guests can fire at once now. */
typedef struct {
    int  host;
    int  id;
    char reason[96];
} pending_guest_t;

/* Last test/shutdown result for one guest, kept by id — not by array
 * position, since the configured list can be edited (and reordered) out
 * from under it. */
typedef struct {
    int  id;
    char text[64];
    bool ok;
} guest_last_t;

static struct {
    pve_config_t      cfg;
    uint16_t          selftest_hours;   /* not part of cfg; see pve_shutdown.h */
    pve_guest_list_t  guests[PVE_MAX_HOSTS];   /* configured rules, owned here */
    pve_engine_t      eng;
    pve_event_cb_t    cb;
    void             *cb_user;
    SemaphoreHandle_t lock;
    bool              started;
    bool              running;         /* the worker task is alive         */
    unsigned          pending_hosts;   /* host bit: node needs shutdown    */
    pending_guest_t  *pending_guests;  /* realloc'd, any host, any length  */
    int               pending_guest_n;
    struct { char text[128]; bool ok; } host_last[PVE_MAX_HOSTS];
    int64_t           host_last_test_us[PVE_MAX_HOSTS]; /* 0 = never tested */
    guest_last_t     *guest_last[PVE_MAX_HOSTS];   /* realloc'd, by id */
    int               guest_last_n[PVE_MAX_HOSTS];
} P;

static void note_host(int i, bool ok, const char *text)
{
    if (i < 0 || i >= PVE_MAX_HOSTS) return;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    P.host_last[i].ok = ok;
    strlcpy(P.host_last[i].text, text, sizeof(P.host_last[i].text));
    xSemaphoreGive(P.lock);
}

/* Upserts by id: a guest firing for the first time grows the array, one
 * that's fired before just updates in place. */
static void note_guest(int i, int id, bool ok, const char *text)
{
    if (i < 0 || i >= PVE_MAX_HOSTS) return;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    guest_last_t *found = NULL;
    for (int k = 0; k < P.guest_last_n[i]; k++) {
        if (P.guest_last[i][k].id == id) {
            found = &P.guest_last[i][k];
            break;
        }
    }
    if (!found) {
        guest_last_t *grown = realloc(P.guest_last[i],
                                      (size_t)(P.guest_last_n[i] + 1) * sizeof(*grown));
        if (!grown) {
            xSemaphoreGive(P.lock);
            return;
        }
        P.guest_last[i] = grown;
        found = &P.guest_last[i][P.guest_last_n[i]];
        found->id = id;
        P.guest_last_n[i]++;
    }
    found->ok = ok;
    strlcpy(found->text, text, sizeof(found->text));
    xSemaphoreGive(P.lock);
}

/* Drops any guest_last entry whose id is no longer in host i's configured
 * list — otherwise a device that's had many different guests configured
 * over time would keep every one of them forever. Call with P.lock held. */
static void prune_guest_last(int i)
{
    int keep = 0;
    for (int k = 0; k < P.guest_last_n[i]; k++) {
        bool still_configured = false;
        for (int g = 0; g < P.guests[i].n; g++) {
            if (P.guests[i].items[g].id == P.guest_last[i][k].id) {
                still_configured = true;
                break;
            }
        }
        if (still_configured) {
            P.guest_last[i][keep++] = P.guest_last[i][k];
        }
    }
    P.guest_last_n[i] = keep;
}

void pve_shutdown_note_test(int i, bool ok, const char *msg)
{
    if (!P.started) return;
    note_host(i, ok, msg);
    if (i < 0 || i >= PVE_MAX_HOSTS) return;
    xSemaphoreTake(P.lock, portMAX_DELAY);
    P.host_last_test_us[i] = esp_timer_get_time();
    xSemaphoreGive(P.lock);
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

    /* Read to EOF, headers and all. Room for the caller's body plus the
     * headers; a caller that wants no body gets just enough for the status
     * line. */
    size_t cap = (resp && resp_sz ? resp_sz : 0) + 1024;
    char *buf = malloc(cap);
    if (!buf) { snprintf(err, err_sz, "out of memory"); goto out; }
    size_t got = 0;
    for (;;) {
        rc = mbedtls_ssl_read(&ssl, (unsigned char *)buf + got, cap - 1 - got);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0) break;
        if (rc < 0) {
            if (got) break;                 /* have a reply; a rude close is fine */
            snprintf(err, err_sz, "read failed (-0x%04x)", -rc);
            free(buf);
            goto out;
        }
        got += rc;
        if (got >= cap - 1) break;
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

static void emit(pve_event_kind_t kind, const char *text)
{
    ESP_LOGW(TAG, "%s", text);
    if (P.cb) {
        P.cb(kind, text, P.cb_user);
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

/* Handle one host's pending work: any guests due — whether their own
 * trigger fired them or the node's swept them up — then, if the node
 * itself is due, the node. `batch` holds every pending guest for this
 * host in this pass (any length; `.host` is unused here, all entries
 * already belong to host `i`). */
static void run_host(int i, const pve_config_t *cfg, bool node_pending,
                     const pending_guest_t *batch, int batch_n,
                     const char *node_reason)
{
    const pve_host_t *h = &cfg->hosts[i];
    char text[240];
    if (!h->enabled || !h->url[0]) {
        return;
    }
    if (!cfg->armed) {
        for (int k = 0; k < batch_n; k++) {
            snprintf(text, sizeof(text), "DRY RUN \xE2\x80\x94 would shut down guest %d on %s (%s)",
                     batch[k].id, h->node, batch[k].reason);
            emit(PVE_EVENT_GUEST, text);
            note_guest(i, batch[k].id, true, "dry run: would shut down");
        }
        if (node_pending) {
            snprintf(text, sizeof(text), "DRY RUN \xE2\x80\x94 would shut down %s (%s)",
                     h->node, node_reason);
            emit(PVE_EVENT_HOST, text);
            note_host(i, true, "dry run: would shut down");
        }
        return;
    }
    if (!h->fingerprint[0]) {
        if (node_pending) {
            snprintf(text, sizeof(text), "\xE2\x9D\x8C Proxmox %s NOT shut down: "
                     "certificate not trusted yet (use Test connection on the Proxmox tab)", h->node);
            emit(PVE_EVENT_HOST, text);
            note_host(i, false, "not trusted \xE2\x80\x94 skipped");
        }
        for (int k = 0; k < batch_n; k++) {
            note_guest(i, batch[k].id, false, "not trusted \xE2\x80\x94 skipped");
        }
        return;
    }

    bool any_guest_shut = false;
    for (int k = 0; k < batch_n; k++) {
        int id = batch[k].id;
        char why[96] = "";
        if (shutdown_guest(h, id, why, sizeof(why))) {
            snprintf(text, sizeof(text), "\xF0\x9F\x94\xBB Shutting down guest %d on %s (%s)",
                     id, h->node, batch[k].reason);
            note_guest(i, id, true, "shutdown sent");
            any_guest_shut = true;
        } else {
            snprintf(text, sizeof(text), "\xE2\x9D\x8C Guest %d on %s shutdown FAILED \xE2\x80\x94 %s",
                     id, h->node, why);
            note_guest(i, id, false, why);
            ESP_LOGE(TAG, "%s: %s", h->node, why);
        }
        emit(PVE_EVENT_GUEST, text);
    }

    if (!node_pending) {
        return;
    }
    /* Only guests just stopped *in this pass* are worth a wait for — one
     * that fired minutes ago on its own trigger has had all the time it
     * needs already. */
    if (any_guest_shut && h->guest_wait_s) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)h->guest_wait_s * 1000));
    }
    char why[96] = "";
    bool good = !h->shutdown_node || shutdown_node(h, why, sizeof(why));
    if (good) {
        snprintf(text, sizeof(text), "\xF0\x9F\x94\xBB Shutting down Proxmox %s (%s)",
                 h->node, node_reason);
        note_host(i, true, "shutdown sent");
    } else {
        snprintf(text, sizeof(text), "\xE2\x9D\x8C Proxmox %s shutdown FAILED \xE2\x80\x94 %s",
                 h->node, why);
        note_host(i, false, why);
    }
    emit(PVE_EVENT_HOST, text);
}

/* Drains P.pending_hosts / P.pending_guests until both are empty, then
 * exits. Work that arrives while another host is being handled is picked
 * up on the next pass; a second task is never started while this one
 * lives. */
static void worker_task(void *arg)
{
    for (;;) {
        pve_config_t cfg;
        char reason[sizeof(P.eng.reason[0])] = "";
        pending_guest_t *batch = NULL;
        int batch_n = 0;
        int i = -1;
        bool node_pending = false;

        xSemaphoreTake(P.lock, portMAX_DELAY);
        for (int k = 0; k < PVE_MAX_HOSTS; k++) {
            if (P.pending_hosts & (1u << k)) {
                i = k;
                break;
            }
        }
        if (i < 0 && P.pending_guest_n > 0) {
            i = P.pending_guests[0].host;
        }
        if (i < 0) {
            P.running = false;
            xSemaphoreGive(P.lock);
            break;
        }
        node_pending = (P.pending_hosts & (1u << i)) != 0;
        P.pending_hosts &= ~(1u << i);
        if (node_pending) {
            strlcpy(reason, P.eng.reason[i], sizeof(reason));
        }
        /* Pull every pending guest belonging to host i into `batch`,
         * compacting the rest of the queue in place. A failed realloc just
         * drops that one guest from this pass — it stays latched (fired)
         * in the engine, so nothing re-queues it, but it also never gets
         * acted on; logged loudly since that's a real gap, not just a
         * skipped log line. */
        int keep = 0;
        for (int k = 0; k < P.pending_guest_n; k++) {
            if (P.pending_guests[k].host == i) {
                pending_guest_t *grown = realloc(batch, (size_t)(batch_n + 1) * sizeof(*grown));
                if (grown) {
                    batch = grown;
                    batch[batch_n++] = P.pending_guests[k];
                } else {
                    ESP_LOGE(TAG, "out of memory: guest %d on host %d dropped from this pass",
                             P.pending_guests[k].id, i);
                }
            } else {
                P.pending_guests[keep++] = P.pending_guests[k];
            }
        }
        P.pending_guest_n = keep;
        cfg = P.cfg;
        xSemaphoreGive(P.lock);

        run_host(i, &cfg, node_pending, batch, batch_n, reason);
        free(batch);
    }
    vTaskDelete(NULL);
}

/* How often the self-test task wakes up to check whether any host is due —
 * coarser than the interval itself (hours), just fine-grained enough that a
 * fresh save of a shorter interval, or the mains coming back, is noticed
 * reasonably promptly. */
#define SELFTEST_TICK_MS (15 * 60 * 1000)

/* Re-runs the same check as a manual "Test connection" against every
 * pinned, enabled host on P.selftest_hours — so a rotated certificate or
 * a revoked token surfaces on its own instead of waiting for a real outage
 * to find it. Skipped entirely while riding out an outage: every host
 * costs several seconds on the network, and the countdown has to stay
 * responsive (same reasoning pve-ups documents for its own self-test). */
static void selftest_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_TICK_MS));

        xSemaphoreTake(P.lock, portMAX_DELAY);
        pve_config_t cfg = P.cfg;
        uint16_t selftest_hours = P.selftest_hours;
        bool on_battery = P.eng.on_battery;
        int64_t now = esp_timer_get_time();
        xSemaphoreGive(P.lock);
        if (!cfg.enabled || selftest_hours == 0 || on_battery) {
            continue;
        }

        int64_t interval_us = (int64_t)selftest_hours * 3600LL * 1000000LL;
        for (int i = 0; i < PVE_MAX_HOSTS; i++) {
            const pve_host_t *hc = &cfg.hosts[i];
            if (!hc->enabled || !hc->url[0] || !hc->fingerprint[0]) {
                continue;
            }

            xSemaphoreTake(P.lock, portMAX_DELAY);
            int64_t last = P.host_last_test_us[i];
            on_battery = P.eng.on_battery;   /* an outage may have just started */
            xSemaphoreGive(P.lock);
            if (on_battery) break;
            if (last != 0 && now - last < interval_us) continue;

            pve_host_t h = *hc;
            pve_guest_list_t guests = { 0 };
            xSemaphoreTake(P.lock, portMAX_DELAY);
            if (P.guests[i].n > 0) {
                guests.items = malloc((size_t)P.guests[i].n * sizeof(*guests.items));
                if (guests.items) {
                    memcpy(guests.items, P.guests[i].items,
                           (size_t)P.guests[i].n * sizeof(*guests.items));
                    guests.n = P.guests[i].n;
                }
            }
            xSemaphoreGive(P.lock);

            char msg[200], fp[96];
            int t = pve_shutdown_test(&h, &guests, msg, sizeof(msg), fp);
            free(guests.items);
            ESP_LOGI(TAG, "%s: self-test \xE2\x80\x94 %s", hc->node, msg);
            pve_shutdown_note_test(i, t == 0, msg);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

/* Frees every host's guest list in `guests` — used when a caller hands
 * pve_shutdown_start/reconfigure a list this module won't end up keeping
 * (already started; not started yet). */
static void free_guest_lists(pve_guest_list_t guests[PVE_MAX_HOSTS])
{
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        free(guests[i].items);
    }
}

int pve_shutdown_start(const pve_config_t *cfg, uint16_t selftest_hours,
                       pve_guest_list_t guests[PVE_MAX_HOSTS],
                       pve_event_cb_t cb, void *user)
{
    if (P.started) {
        free_guest_lists(guests);
        return 0;
    }
    P.lock = xSemaphoreCreateMutex();
    if (!P.lock) {
        free_guest_lists(guests);
        return -1;
    }
    P.cfg = *cfg;
    P.selftest_hours = selftest_hours;
    P.cb = cb;
    P.cb_user = user;
    memset(&P.eng, 0, sizeof(P.eng));
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        P.guests[i] = guests[i];   /* take ownership of .items */
        pve_engine_sync_guests(&P.eng.guests[i], &P.guests[i]);
    }
    P.started = true;
    if (cfg->enabled) {
        for (int i = 0; i < PVE_MAX_HOSTS; i++) {
            const pve_host_t *h = &cfg->hosts[i];
            if (!h->enabled) continue;
            ESP_LOGI(TAG, "%s: %s \xE2\x80\x94 on battery %u min / charge <= %u%%%s (%d guest rule%s)",
                     h->node, cfg->armed ? "ARMED" : "dry run",
                     (unsigned)h->on_battery_min, (unsigned)h->charge_pct,
                     h->fingerprint[0] ? "" : " (NOT PINNED)",
                     P.guests[i].n, P.guests[i].n == 1 ? "" : "s");
        }
    }
    /* TLS in-task, same as the shutdown worker; runs for the life of the
     * device, waking up briefly every SELFTEST_TICK_MS to check whether any
     * host is due. */
    if (xTaskCreate(selftest_task, "pve_selftest", 10240, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the Proxmox self-test task");
    }
    return 0;
}

void pve_shutdown_reconfigure(const pve_config_t *cfg, uint16_t selftest_hours,
                              pve_guest_list_t guests[PVE_MAX_HOSTS])
{
    if (!P.started) {
        free_guest_lists(guests);
        return;
    }
    xSemaphoreTake(P.lock, portMAX_DELAY);
    P.cfg = *cfg;
    P.selftest_hours = selftest_hours;
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        free(P.guests[i].items);
        P.guests[i] = guests[i];   /* take ownership of .items */
        pve_engine_sync_guests(&P.eng.guests[i], &P.guests[i]);
        prune_guest_last(i);
    }
    xSemaphoreGive(P.lock);
}

void pve_shutdown_observe(bool known, bool on_mains, int soc_pct)
{
    if (!P.started) {
        return;
    }
    pve_obs_t obs = {
        .power   = !known ? PVE_PWR_UNKNOWN : on_mains ? PVE_PWR_LINE : PVE_PWR_BATTERY,
        .soc_pct = known ? soc_pct : -1,
    };

    xSemaphoreTake(P.lock, portMAX_DELAY);
    pve_fire_t fire = pve_eval(&P.cfg, P.guests, &P.eng, &obs, esp_timer_get_time());
    bool any = fire.hosts != 0;
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        if (fire.hosts & (1u << i)) {
            ESP_LOGW(TAG, "%s: trigger \xE2\x80\x94 %s (%s)", P.cfg.hosts[i].node,
                     P.eng.reason[i], P.cfg.armed ? "ARMED" : "dry run");
        }
        for (int g = 0; g < P.eng.guests[i].n; g++) {
            pve_guest_engine_t *gs = &P.eng.guests[i].items[g];
            if (!gs->fired_now) continue;
            any = true;
            pending_guest_t *grown = realloc(P.pending_guests,
                                             (size_t)(P.pending_guest_n + 1) * sizeof(*grown));
            if (!grown) {
                ESP_LOGE(TAG, "out of memory: guest %d trigger on %s dropped",
                         gs->id, P.cfg.hosts[i].node);
                continue;
            }
            P.pending_guests = grown;
            P.pending_guests[P.pending_guest_n].host = i;
            P.pending_guests[P.pending_guest_n].id = gs->id;
            strlcpy(P.pending_guests[P.pending_guest_n].reason, gs->reason,
                   sizeof(P.pending_guests[0].reason));
            P.pending_guest_n++;
            ESP_LOGW(TAG, "%s: guest %d trigger \xE2\x80\x94 %s (%s)",
                     P.cfg.hosts[i].node, gs->id, gs->reason, P.cfg.armed ? "ARMED" : "dry run");
        }
    }
    bool start = false;
    if (any) {
        P.pending_hosts |= fire.hosts;
        if (!P.running) {
            P.running = true;
            start = true;
        }
    }
    xSemaphoreGive(P.lock);

    if (!start) {
        return;
    }
    /* TLS in-task; the mbedTLS handshake wants a comfortable stack. */
    if (xTaskCreate(worker_task, "pve_seq", 10240, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the shutdown worker");
        xSemaphoreTake(P.lock, portMAX_DELAY);
        P.running = false;
        xSemaphoreGive(P.lock);
    }
}

void pve_shutdown_status(pve_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->on_battery_s = -1;
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        out->hosts[i].countdown_s = -1;
    }
    if (!P.started) {
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    out->enabled = P.cfg.enabled;
    out->armed = P.cfg.armed;
    if (P.eng.on_battery) {
        out->on_battery_s = (int)((now - P.eng.on_battery_since_us) / 1000000LL);
    }
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        const pve_host_t *h = &P.cfg.hosts[i];
        pve_host_status_t *o = &out->hosts[i];
        strlcpy(o->node, h->node, sizeof(o->node));
        o->enabled = h->enabled && h->url[0];
        o->pinned = h->fingerprint[0] != '\0';
        o->fired = P.eng.fired[i];
        o->countdown_s = pve_countdown_s(&P.cfg, &P.eng, i, now);
        strlcpy(o->last, P.host_last[i].text, sizeof(o->last));
        o->last_ok = P.host_last[i].ok;
        o->last_checked_s = P.host_last_test_us[i] ?
            (int)((now - P.host_last_test_us[i]) / 1000000LL) : -1;
    }
    xSemaphoreGive(P.lock);
}

void pve_shutdown_guest_status(int i, pve_guest_status_t **out, int *n)
{
    *out = NULL;
    *n = 0;
    if (i < 0 || i >= PVE_MAX_HOSTS || !P.started) {
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(P.lock, portMAX_DELAY);
    int count = P.guests[i].n;
    pve_guest_status_t *arr = count > 0 ? malloc((size_t)count * sizeof(*arr)) : NULL;
    if (count > 0 && !arr) {
        xSemaphoreGive(P.lock);
        return;
    }
    for (int g = 0; g < count; g++) {
        int id = P.guests[i].items[g].id;
        pve_guest_status_t *go = &arr[g];
        go->id = id;
        go->fired = false;
        for (int k = 0; k < P.eng.guests[i].n; k++) {
            if (P.eng.guests[i].items[k].id == id) {
                go->fired = P.eng.guests[i].items[k].fired;
                break;
            }
        }
        go->countdown_s = pve_guest_countdown_s(&P.cfg, &P.eng, &P.guests[i], i, id, now);
        go->last[0] = '\0';
        go->last_ok = true;
        for (int k = 0; k < P.guest_last_n[i]; k++) {
            if (P.guest_last[i][k].id == id) {
                strlcpy(go->last, P.guest_last[i][k].text, sizeof(go->last));
                go->last_ok = P.guest_last[i][k].ok;
                break;
            }
        }
    }
    xSemaphoreGive(P.lock);
    *out = arr;
    *n = count;
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

int pve_shutdown_test(pve_host_t *h, const pve_guest_list_t *guests,
                      char *msg, size_t msg_sz, char seen_fp[96])
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
    bool trusted_now = false;

    req_result_t r = tls_request(h, "/version", NULL, resp, RESP_MAX,
                                 err, sizeof(err));
    if (r.status == 0) {
        /* Nothing trusted yet: this is the only moment there is anything to
         * compare against, so trust what was just shown — like SSH on a new
         * host key — and go straight on to the real request. Every
         * connection after this one is checked against exactly this value. */
        strlcpy(h->fingerprint, r.seen_fp, sizeof(h->fingerprint));
        fp_pretty(r.seen_fp, seen_fp);
        trusted_now = true;
        r = tls_request(h, "/version", NULL, resp, RESP_MAX, err, sizeof(err));
    }
    if (r.status != 200) {
        char d[80];
        describe(r.status, err, d, sizeof(d));
        snprintf(msg, msg_sz, "%sGET /version: %s",
                 trusted_now ? "Certificate trusted. " : "", d);
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
        ret = 1;          /* connected fine; this part just didn't confirm */
        goto done;
    }
    j = cJSON_Parse(resp);
    cJSON *d = j ? cJSON_GetObjectItem(j, "data") : NULL;
    if (!d) {
        if (j) cJSON_Delete(j);
        snprintf(msg, msg_sz, "PVE %s reachable, permissions reply unreadable "
                              "(too long? the token should carry one role)", ver);
        ret = 1;
        goto done;
    }

    char nodepath[48];
    snprintf(nodepath, sizeof(nodepath), "/nodes/%s", h->node);
    bool node_ok = !h->shutdown_node ||
                   has_priv(d, "/", "Sys.PowerMgmt") ||
                   has_priv(d, "/nodes", "Sys.PowerMgmt") ||
                   has_priv(d, nodepath, "Sys.PowerMgmt");
    int guests_missing = 0;
    for (int g = 0; g < guests->n; g++) {
        char vp[24];
        snprintf(vp, sizeof(vp), "/vms/%d", guests->items[g].id);
        if (!has_priv(d, "/", "VM.PowerMgmt") && !has_priv(d, "/vms", "VM.PowerMgmt") &&
            !has_priv(d, vp, "VM.PowerMgmt")) {
            guests_missing++;
        }
    }
    cJSON_Delete(j);

    if (node_ok && !guests_missing) {
        snprintf(msg, msg_sz, "PVE %s: Sys.PowerMgmt ok%s",
                 ver, guests->n ? ", VM.PowerMgmt ok for the guests" : "");
        ret = 0;
        goto done;
    }
    snprintf(msg, msg_sz, "%sPVE %s reachable, but %s%s%s",
             trusted_now ? "Certificate trusted. " : "", ver,
             node_ok ? "" : "Sys.PowerMgmt not confirmed on /nodes",
             (!node_ok && guests_missing) ? "; " : "",
             guests_missing ? "VM.PowerMgmt missing for some guests" : "");
    ret = 1;              /* connected fine; a privilege just isn't confirmed */
done:
    free(resp);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Guest listing                                                       */
/* ------------------------------------------------------------------ */

/* Append the guests of one kind to `out` as JSON array elements. */
static int list_kind(const pve_host_t *h, const char *kind, char *out,
                     size_t out_sz, size_t *o, bool *first, char *err, size_t esz)
{
    char path[64];
    snprintf(path, sizeof(path), "/nodes/%s/%s", h->node, kind);
    char *resp = malloc(PVE_GUEST_RESP_MAX);
    if (!resp) { snprintf(err, esz, "out of memory"); return -1; }
    req_result_t r = tls_request(h, path, NULL, resp, PVE_GUEST_RESP_MAX, err, esz);
    if (r.status != 200) {
        char d[80];
        describe(r.status, err, d, sizeof(d));
        snprintf(err, esz, "GET %s: %s", path, d);
        free(resp);
        return -1;
    }
    cJSON *j = cJSON_Parse(resp);
    free(resp);
    cJSON *data = j ? cJSON_GetObjectItem(j, "data") : NULL;
    if (!cJSON_IsArray(data)) {
        if (j) cJSON_Delete(j);
        snprintf(err, esz, "GET %s: reply unreadable (too many guests for this buffer?)", path);
        return -1;
    }
    int n = 0;
    cJSON *g;
    cJSON_ArrayForEach(g, data) {
        cJSON *id = cJSON_GetObjectItem(g, "vmid");
        cJSON *name = cJSON_GetObjectItem(g, "name");
        cJSON *st = cJSON_GetObjectItem(g, "status");
        if (!cJSON_IsNumber(id)) continue;
        /* Names are user-chosen; keep them out of the JSON's way. */
        char nm[40] = "";
        if (cJSON_IsString(name)) {
            int k = 0;
            for (const char *p = name->valuestring; *p && k < 38; p++) {
                nm[k++] = (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) ? '_' : *p;
            }
            nm[k] = '\0';
        }
        int w = snprintf(out + *o, out_sz - *o,
                         "%s{\"id\":%d,\"name\":\"%s\",\"type\":\"%s\",\"status\":\"%s\"}",
                         *first ? "" : ",", id->valueint, nm, kind,
                         cJSON_IsString(st) ? st->valuestring : "");
        if (w < 0 || (size_t)w >= out_sz - *o) break;
        *o += w;
        *first = false;
        n++;
    }
    cJSON_Delete(j);
    return n;
}

int pve_shutdown_list_guests(const pve_host_t *h, char *json, size_t json_sz,
                             char *err, size_t err_sz)
{
    if (!h->fingerprint[0]) {
        snprintf(err, err_sz, "pin the certificate first (Test)");
        return -1;
    }
    size_t o = 0;
    bool first = true;
    json[o++] = '[';
    int a = list_kind(h, "qemu", json, json_sz - 2, &o, &first, err, err_sz);
    if (a < 0) return -1;
    int b = list_kind(h, "lxc", json, json_sz - 2, &o, &first, err, err_sz);
    if (b < 0) return -1;
    json[o++] = ']';
    json[o] = '\0';
    return a + b;
}
