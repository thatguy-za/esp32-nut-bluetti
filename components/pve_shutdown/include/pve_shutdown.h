#pragma once
/*
 * Proxmox VE shutdown on UPS events.
 *
 * Modelled on ffind-dev/pve-ups: shut hosts down through the Proxmox API
 * with a dedicated, revocable API token that carries only Sys.PowerMgmt
 * (and VM.PowerMgmt for any guests listed) — no agent on the host, no SSH.
 *
 *   POST /api2/json/nodes/{node}/status               command=shutdown
 *   POST /api2/json/nodes/{node}/qemu/{id}/status/shutdown
 *   POST /api2/json/nodes/{node}/lxc/{id}/status/shutdown
 *   Authorization: PVEAPIToken=<token_id>=<secret>
 *
 * And its safety model, which is the part worth copying:
 *   - fail-safe: losing the Bluetti is an alarm, never a shutdown. UNKNOWN
 *     power never starts a countdown — but one already running through a
 *     confirmed outage keeps running on our own clock, since a switch
 *     between us and the unit losing power is itself a symptom.
 *   - dry-run by default: fully configured, it logs what it would do and
 *     shuts down nothing until armed.
 *   - the outage is timed here, on this device's clock, from the OL->OB
 *     edge — not from anything the unit reports.
 *   - a failed POST is retried; a 503 from a busy pveproxy is not proof the
 *     node is going down.
 *   - latched after firing, released only once the mains has been back for
 *     a while, so a flapping supply cannot fire twice into a host that is
 *     mid-shutdown.
 *
 * Proxmox ships a self-signed certificate on 8006, so there is no CA to
 * verify against. Rather than skip verification (pve-ups's default), each
 * host is pinned by the SHA-256 fingerprint of its certificate — the same
 * one Proxmox shows under System > Certificates. Trust-on-first-use with
 * an explicit step: until a fingerprint is pinned, the Test button
 * completes the handshake, reports what it saw, and sends nothing. The
 * token never crosses an unpinned connection. The client is plain mbedTLS
 * (not esp-tls), so no global setting is loosened for anything else.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PVE_MAX_HOSTS 4

/* One Proxmox host, shut down in array order. */
typedef struct {
    bool     enabled;
    char     url[64];        /* https://10.0.0.10:8006 — one per node      */
    char     node[32];       /* node name, as `pvecm nodes` / the UI shows */
    char     token_id[64];   /* ups@pve!shutdown                           */
    char     secret[40];     /* the token secret (a UUID)                  */
    char     guests[64];     /* VM/CT ids to stop first, "101,102"; blank = none */
    uint16_t guest_wait_s;   /* after the last guest, before the node      */
    bool     shutdown_node;  /* false = guests only, the node stays up     */
    char     fingerprint[65];/* SHA-256 of the server cert, 64 hex, no
                                colons; blank = not pinned, nothing is sent */
} pve_host_t;

/* Triggers. Any one that is set and met fires the sequence. Stored inside
 * app_config_t, so append-only like everything else there. */
typedef struct {
    bool     enabled;        /* the feature at all                         */
    bool     armed;          /* false = dry-run: log + notify, touch nothing */
    uint16_t on_battery_min; /* on battery this long; 0 = off              */
    uint8_t  charge_pct;     /* charge at or below this, on battery; 0 = off */
    bool     on_low_battery; /* NUT's LB flag                              */
    uint16_t host_delay_s;   /* pause between hosts                        */
    uint16_t mains_back_min; /* mains back this long before re-arming      */
    pve_host_t hosts[PVE_MAX_HOSTS];
} pve_config_t;

/* ---- the decision, kept pure so the host suite can drive it -------- */

typedef enum { PVE_PWR_UNKNOWN = 0, PVE_PWR_LINE, PVE_PWR_BATTERY } pve_power_t;

typedef struct {
    pve_power_t power;
    bool        low_battery; /* LB present in ups.status                   */
    int         soc_pct;     /* -1 = unknown                               */
} pve_obs_t;

typedef struct {
    bool    on_battery;           /* a countdown is running...             */
    int64_t on_battery_since_us;  /* ...since this edge (our clock)        */
    bool    on_mains;             /* known to be on the mains...           */
    int64_t mains_since_us;       /* ...since this edge                    */
    bool    fired;                /* latched until the mains has been back */
    char    reason[80];           /* why it fired, for the log and alerts  */
} pve_engine_t;

/* Fold one observation into the engine. Returns true exactly once per
 * outage, the moment a trigger is met; the caller decides armed vs dry-run.
 * Never returns true while `fired` is latched. */
bool pve_eval(const pve_config_t *cfg, pve_engine_t *st,
              const pve_obs_t *obs, int64_t now_us);

/* Seconds until the on-battery trigger fires, or -1 when no countdown is
 * running / that trigger is off. For the status page. */
int pve_countdown_s(const pve_config_t *cfg, const pve_engine_t *st,
                    int64_t now_us);

/* Turn a NUT status string ("OL", "OB LB DISCHRG", "OL WAIT", "OFF") into an
 * observation. WAIT and OFF are UNKNOWN: not a power-state reading. */
void pve_obs_from_status(const char *status, int soc_pct, pve_obs_t *out);

/* ---- runtime ------------------------------------------------------ */

#ifndef PVE_HOST_ONLY
/* Something happened worth telling the user (fired, dry-run, a failed
 * host). Text is ready to send; the callback decides where. */
typedef void (*pve_event_cb_t)(const char *text, void *user);

int  pve_shutdown_start(const pve_config_t *cfg, pve_event_cb_t cb, void *user);
void pve_shutdown_reconfigure(const pve_config_t *cfg);

/* Feed the engine. Call with every status publish, and again from the
 * staleness path so a lost link is seen as UNKNOWN promptly. */
void pve_shutdown_observe(const char *ups_status, int soc_pct);

/* Snapshot for the status page: the engine, and one line per host. */
typedef struct {
    char node[32];
    bool enabled;
    bool pinned;                 /* a fingerprint is stored               */
    char last[64];               /* "" until something happens; then the
                                    last test or shutdown result          */
    bool last_ok;
} pve_host_status_t;

typedef struct {
    bool enabled, armed, fired;
    int  countdown_s;            /* -1 = none running */
    int  on_battery_s;           /* -1 = not on battery */
    char last[128];              /* last sequence result, "" = none yet */
    pve_host_status_t hosts[PVE_MAX_HOSTS];
} pve_status_t;
void pve_shutdown_status(pve_status_t *out);

/* Blocking connection test against one host. With no fingerprint pinned:
 * handshake only, returns 1 and puts the certificate's fingerprint in
 * `seen_fp` (colon-separated, for the user to pin) — the token is not
 * sent. With one pinned: GET /version, then confirm Sys.PowerMgmt on
 * /nodes (or /nodes/<node>) and VM.PowerMgmt for each listed guest;
 * 0 = all good, -1 = a problem. `msg` is filled either way. */
int  pve_shutdown_test(const pve_host_t *host, char *msg, size_t msg_sz,
                       char seen_fp[96]);

/* Record a test outcome against host slot `i` so the status page shows it. */
void pve_shutdown_note_test(int i, bool ok, const char *msg);

/* "AB:CD:..." or "abcd..." -> 64 upper-case hex, or false if malformed. */
bool pve_fingerprint_normalise(const char *in, char out[65]);
#endif

#ifdef __cplusplus
}
#endif
