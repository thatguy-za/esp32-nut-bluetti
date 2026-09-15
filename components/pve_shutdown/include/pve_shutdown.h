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
 * one Proxmox shows under System > Certificates. Trust-on-first-use, like
 * an SSH host key: the first Test against a host trusts whatever
 * certificate it presents and remembers it; every connection after that —
 * Test included — is refused if the certificate ever changes. There is
 * nothing to compare the first sighting against, so the fingerprint is
 * never shown or typed in; "Forget trusted certificate" clears it so the
 * next Test trusts again. The token never crosses a connection whose
 * certificate doesn't match. The client is plain mbedTLS (not esp-tls), so
 * no global setting is loosened for anything else.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PVE_MAX_HOSTS 4

/* One Proxmox host, with its own triggers: each host runs its own
 * countdown against the same outage and fires independently, so a NAS can
 * go at ten minutes and the hypervisor at thirty. */
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
    /* Triggers. Either one that is set and met fires this host. */
    uint16_t on_battery_min; /* on battery this long; 0 = off              */
    uint8_t  charge_pct;     /* charge at or below this, on battery; 0 = off */
} pve_host_t;

/* Stored inside app_config_t. Its layout changed at CFG_VERSION 9, when
 * the triggers moved into the hosts; the loader resets an older block. */
typedef struct {
    bool     enabled;        /* the feature at all                         */
    bool     armed;          /* false = dry-run: log + notify, touch nothing */
    uint16_t mains_back_min; /* mains back this long before re-arming      */
    pve_host_t hosts[PVE_MAX_HOSTS];
} pve_config_t;

/* ---- the decision, kept pure so the host suite can drive it -------- */

/* Read straight off the Bluetti telemetry — nothing here depends on the
 * NUT server or its thresholds. UNKNOWN covers no link, no data yet, and a
 * sweep that has not read the mains registers: never a power reading. */
typedef enum { PVE_PWR_UNKNOWN = 0, PVE_PWR_LINE, PVE_PWR_BATTERY } pve_power_t;

typedef struct {
    pve_power_t power;
    int         soc_pct;     /* -1 = unknown                               */
} pve_obs_t;

typedef struct {
    bool    on_battery;           /* a countdown is running...             */
    int64_t on_battery_since_us;  /* ...since this edge (our clock)        */
    bool    on_mains;             /* known to be on the mains...           */
    int64_t mains_since_us;       /* ...since this edge                    */
    bool    fired[PVE_MAX_HOSTS]; /* per host: latched until mains is back */
    char    reason[PVE_MAX_HOSTS][64];  /* why each fired                  */
} pve_engine_t;

/* Fold one observation into the engine. Returns a bitmask of the hosts
 * whose trigger is met on this observation — each host appears in it
 * exactly once per outage, and never while its latch is set. The caller
 * decides armed vs dry-run. */
unsigned pve_eval(const pve_config_t *cfg, pve_engine_t *st,
                  const pve_obs_t *obs, int64_t now_us);

/* Seconds until host `i`'s on-battery trigger fires, or -1 when no
 * countdown is running, that trigger is off, or the host is latched. */
int pve_countdown_s(const pve_config_t *cfg, const pve_engine_t *st, int i,
                    int64_t now_us);


/* ---- runtime ------------------------------------------------------ */

#ifndef PVE_HOST_ONLY
/* Something happened worth telling the user (fired, dry-run, a failed
 * host). Text is ready to send; the callback decides where. */
typedef void (*pve_event_cb_t)(const char *text, void *user);

int  pve_shutdown_start(const pve_config_t *cfg, pve_event_cb_t cb, void *user);
void pve_shutdown_reconfigure(const pve_config_t *cfg);

/* Feed the engine from the Bluetti state. `known` is false whenever the
 * reading cannot be trusted — no link, nothing decoded, or the first sweep
 * still running — and then `on_mains`/`soc_pct` are ignored. Call on every
 * decoded update, and from the staleness path with known = false. */
void pve_shutdown_observe(bool known, bool on_mains, int soc_pct);

/* Snapshot for the status page: the engine, and one line per host. */
typedef struct {
    char node[32];
    bool enabled;
    bool pinned;                 /* a fingerprint is stored               */
    bool fired;                  /* latched                                */
    int  countdown_s;            /* -1 = none running                      */
    char last[64];               /* "" until something happens; then the
                                    last test or shutdown result          */
    bool last_ok;
} pve_host_status_t;

typedef struct {
    bool enabled, armed;
    int  on_battery_s;           /* -1 = not on battery */
    pve_host_status_t hosts[PVE_MAX_HOSTS];
} pve_status_t;
void pve_shutdown_status(pve_status_t *out);

/* Blocking connection test against one host. With no certificate trusted
 * yet, trusts whatever this connection presents (TOFU) and continues in
 * the same call — `host->fingerprint` comes back filled in, and `seen_fp`
 * carries the same value (colon-separated) so the caller can persist it.
 * Either way: GET /version, then confirm Sys.PowerMgmt on /nodes (or
 * /nodes/<node>) and VM.PowerMgmt for each listed guest; 0 = all good,
 * -1 = a problem (including a certificate that no longer matches the one
 * trusted before). `msg` is filled either way; `seen_fp` is only filled
 * when this call is the one that established trust. */
int  pve_shutdown_test(pve_host_t *host, char *msg, size_t msg_sz,
                       char seen_fp[96]);

/* List the host's VMs and containers as a JSON array of
 * {id,name,type:"qemu"|"lxc",status}, for the picker. Needs the token to
 * carry VM.Audit, and the certificate to be pinned. Returns the count, or
 * -1 with `err` filled. Blocking. */
int  pve_shutdown_list_guests(const pve_host_t *host, char *json, size_t json_sz,
                              char *err, size_t err_sz);

/* Record a test outcome against host slot `i` so the status page shows it. */
void pve_shutdown_note_test(int i, bool ok, const char *msg);

/* "AB:CD:..." or "abcd..." -> 64 upper-case hex, or false if malformed. */
bool pve_fingerprint_normalise(const char *in, char out[65]);
#endif

#ifdef __cplusplus
}
#endif
