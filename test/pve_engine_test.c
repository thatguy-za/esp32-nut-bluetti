/* Drives the real pve_engine.c through outages minute by minute. This is
 * the code that decides to power off a server, so every rule in
 * pve_shutdown.h's safety model gets a case here: fail-safe on unknown,
 * countdown on our own clock, per-host thresholds off one shared edge,
 * per-host fire-once latches released together once the mains is back,
 * the charge trigger needing a live reading, and t = 0 being a valid start.
 * No I/O is involved — the runtime half (TLS, the API, the sequence) is not
 * covered and needs a real host. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "pve_shutdown.h"

static int fails;
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

#define MIN(n) ((int64_t)(n) * 60 * 1000000LL)
#define H0 (1u << 0)
#define H1 (1u << 1)

/* One enabled host: 30 min / 10 %. */
static pve_config_t base(void)
{
    pve_config_t c;
    memset(&c, 0, sizeof c);
    c.enabled = true;
    c.armed = true;
    c.mains_back_min = 5;
    c.hosts[0].enabled = true;
    strcpy(c.hosts[0].node, "pve1");
    c.hosts[0].on_battery_min = 30;
    c.hosts[0].charge_pct = 10;
    return c;
}

/* No guest configured for any host, for the host-only scenarios below —
 * pve_eval() now takes each host's guest list as a separate argument since
 * there's no per-host cap to embed it against. */
static pve_guest_list_t g_guests[PVE_MAX_HOSTS];

/* Replaces host `host`'s configured guest list and keeps the engine's
 * per-guest state matched to it, the way the runtime does after a save. */
static void set_guests(pve_engine_t *e, int host, const pve_guest_t *arr, int n)
{
    free(g_guests[host].items);
    g_guests[host].items = NULL;
    g_guests[host].n = 0;
    if (n > 0) {
        g_guests[host].items = malloc((size_t)n * sizeof(pve_guest_t));
        memcpy(g_guests[host].items, arr, (size_t)n * sizeof(pve_guest_t));
        g_guests[host].n = n;
    }
    pve_engine_sync_guests(&e->guests[host], &g_guests[host]);
}

/* Frees everything set_guests() has allocated so far and zeroes the
 * engine — the guest-scenario equivalent of `memset(&e, 0, sizeof e)`. */
static void reset_engine(pve_engine_t *e)
{
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        free(e->guests[i].items);
        free(g_guests[i].items);
        g_guests[i] = (pve_guest_list_t){ 0 };
    }
    memset(e, 0, sizeof *e);
}

static bool guest_fired(const pve_engine_t *e, int host, int id)
{
    for (int g = 0; g < e->guests[host].n; g++) {
        if (e->guests[host].items[g].id == id) return e->guests[host].items[g].fired;
    }
    return false;
}
static bool guest_fired_now(const pve_engine_t *e, int host, int id)
{
    for (int g = 0; g < e->guests[host].n; g++) {
        if (e->guests[host].items[g].id == id) return e->guests[host].items[g].fired_now;
    }
    return false;
}
static const char *guest_reason(const pve_engine_t *e, int host, int id)
{
    for (int g = 0; g < e->guests[host].n; g++) {
        if (e->guests[host].items[g].id == id) return e->guests[host].items[g].reason;
    }
    return "";
}

static unsigned feed(const pve_config_t *c, pve_engine_t *e, pve_power_t pw,
                     int soc, int64_t t)
{
    pve_obs_t o = { .power = pw, .soc_pct = soc };
    return pve_eval(c, g_guests, e, &o, t).hosts;
}
#define BATT PVE_PWR_BATTERY
#define LINE PVE_PWR_LINE
#define NONE PVE_PWR_UNKNOWN

int main(void)
{
    pve_config_t c = base();
    pve_engine_t e;

    /* ---- t = 0 is a valid moment, not "unset" ---- */
    memset(&e, 0, sizeof e);
    OKF(feed(&c, &e, BATT, 90, 0) == 0 && e.on_battery,
        "an outage that begins at timestamp 0 still starts the countdown");
    OKF(feed(&c, &e, BATT, 90, MIN(30)) == H0, "...and fires at 30 min");

    /* ---- the on-battery countdown, on our clock ---- */
    memset(&e, 0, sizeof e);
    OKF(feed(&c, &e, LINE, 100, MIN(0)) == 0, "on line: nothing");
    OKF(pve_countdown_s(&c, &e, 0, MIN(0)) == -1, "no countdown on line");
    OKF(feed(&c, &e, BATT, 95, MIN(1)) == 0, "t=1: mains drops, countdown starts");
    OKF(pve_countdown_s(&c, &e, 0, MIN(1)) == 30 * 60, "30:00 to go at the edge");
    OKF(pve_countdown_s(&c, &e, 0, MIN(21)) == 10 * 60, "10:00 to go, twenty minutes in");
    OKF(feed(&c, &e, BATT, 60, MIN(30)) == 0, "t=30: 29 min on battery, not yet");
    OKF(feed(&c, &e, BATT, 58, MIN(31)) == H0, "t=31: 30 min on battery -> FIRE host 1");
    OKF(strstr(e.reason[0], "30 min") != NULL, "reason names the trigger: '%s'", e.reason[0]);
    OKF(e.fired[0], "latched");
    OKF(feed(&c, &e, BATT, 50, MIN(32)) == 0, "still on battery: fires only once");
    OKF(pve_countdown_s(&c, &e, 0, MIN(32)) == -1, "no countdown shown while latched");

    /* ---- the latch: mains back, but not for long enough ---- */
    OKF(feed(&c, &e, LINE, 50, MIN(40)) == 0, "t=40: mains back");
    OKF(e.fired[0], "still latched the moment it returns");
    OKF(feed(&c, &e, LINE, 55, MIN(44)) == 0 && e.fired[0], "t=44: back 4 min, still latched");
    OKF(feed(&c, &e, LINE, 56, MIN(45)) == 0 && !e.fired[0], "t=45: back 5 min -> released");
    OKF(feed(&c, &e, BATT, 56, MIN(46)) == 0 && feed(&c, &e, BATT, 56, MIN(76)) == H0,
        "a second outage after release fires again (fresh 30 min)");

    /* ---- a flap inside the latch must not fire twice ---- */
    memset(&e, 0, sizeof e);
    feed(&c, &e, BATT, 90, MIN(0));
    OKF(feed(&c, &e, BATT, 70, MIN(30)) == H0, "fires");
    feed(&c, &e, LINE, 70, MIN(31));           /* back for a minute */
    feed(&c, &e, BATT, 70, MIN(32));           /* gone again */
    OKF(e.fired[0] && feed(&c, &e, BATT, 40, MIN(70)) == 0,
        "a 1-min mains blip inside the latch does not re-fire, even 38 min later");

    /* ---- mains returning resets the countdown ---- */
    memset(&e, 0, sizeof e);
    feed(&c, &e, BATT, 90, MIN(0));
    feed(&c, &e, LINE, 90, MIN(20));
    OKF(pve_countdown_s(&c, &e, 0, MIN(20)) == -1, "mains back at 20 min: countdown gone");
    feed(&c, &e, BATT, 90, MIN(25));
    OKF(feed(&c, &e, BATT, 90, MIN(50)) == 0, "new outage: 25 min in, not 50");
    OKF(feed(&c, &e, BATT, 90, MIN(55)) == H0, "fires 30 min after the *second* edge");

    /* ---- fail-safe: unknown never starts anything ---- */
    memset(&e, 0, sizeof e);
    OKF(feed(&c, &e, NONE, -1, MIN(0)) == 0 && !e.on_battery, "no reading from boot: nothing");
    for (int m = 1; m < 120; m++) feed(&c, &e, NONE, -1, MIN(m));
    OKF(!e.fired[0], "two hours with no reading: still nothing (fail-safe)");

    /* ---- but a running countdown survives losing the link ---- */
    memset(&e, 0, sizeof e);
    feed(&c, &e, BATT, 80, MIN(0));
    feed(&c, &e, NONE, -1, MIN(5));            /* link dies mid-outage */
    OKF(e.on_battery && e.on_battery_since_us == MIN(0), "countdown kept through the loss");
    OKF(feed(&c, &e, NONE, -1, MIN(29)) == 0, "29 min, blind: not yet");
    OKF(feed(&c, &e, NONE, -1, MIN(30)) == H0, "30 min, blind: fires on the timer");
    OKF(feed(&c, &e, NONE, -1, MIN(60)) == 0 && e.fired[0],
        "blind for 30 min after firing: unknown is not 'mains back'");

    /* ---- the charge trigger needs a live reading ---- */
    memset(&e, 0, sizeof e);
    feed(&c, &e, BATT, 50, MIN(0));
    OKF(feed(&c, &e, BATT, 11, MIN(1)) == 0, "11%%: above the 10%% limit");
    OKF(feed(&c, &e, BATT, 10, MIN(2)) == H0, "10%%: at the limit -> FIRE");
    OKF(strstr(e.reason[0], "10%") != NULL, "reason: '%s'", e.reason[0]);
    memset(&e, 0, sizeof e);
    feed(&c, &e, BATT, 50, MIN(0));
    OKF(feed(&c, &e, NONE, 5, MIN(1)) == 0, "a charge figure with no link behind it does not fire");
    memset(&e, 0, sizeof e);
    OKF(feed(&c, &e, LINE, 5, MIN(0)) == 0, "5%% charge on the mains is not a trigger");

    /* ---- per-host thresholds off one shared edge ---- */
    c = base();
    c.hosts[1].enabled = true;
    strcpy(c.hosts[1].node, "nas");
    c.hosts[1].on_battery_min = 10;
    c.hosts[1].charge_pct = 0;
    memset(&e, 0, sizeof e);
    feed(&c, &e, BATT, 90, MIN(0));
    OKF(pve_countdown_s(&c, &e, 0, MIN(0)) == 30 * 60 && pve_countdown_s(&c, &e, 1, MIN(0)) == 10 * 60,
        "two hosts, two countdowns from the same edge (30:00 and 10:00)");
    OKF(feed(&c, &e, BATT, 85, MIN(10)) == H1, "t=10: only the 10-min host fires");
    OKF(e.fired[1] && !e.fired[0], "host 2 latched, host 1 still counting");
    OKF(pve_countdown_s(&c, &e, 0, MIN(10)) == 20 * 60, "host 1 has 20:00 left");
    OKF(feed(&c, &e, BATT, 70, MIN(30)) == H0, "t=30: the 30-min host fires, host 2 does not re-fire");
    OKF(feed(&c, &e, BATT, 9, MIN(31)) == 0, "9%%: host 1 already fired; host 2 has no charge trigger");
    feed(&c, &e, LINE, 9, MIN(40));
    OKF(feed(&c, &e, LINE, 20, MIN(45)) == 0 && !e.fired[0] && !e.fired[1],
        "both latches release together once the mains has been back 5 min");

    /* ---- a host with only a charge trigger never counts down ---- */
    memset(&e, 0, sizeof e);
    c = base(); c.hosts[0].on_battery_min = 0;
    feed(&c, &e, BATT, 50, MIN(0));
    OKF(pve_countdown_s(&c, &e, 0, MIN(0)) == -1, "timer off: no countdown");
    OKF(feed(&c, &e, BATT, 50, MIN(500)) == 0, "timer off: 500 min on battery, nothing");
    OKF(feed(&c, &e, BATT, 9, MIN(501)) == H0, "...but charge still fires");

    /* ---- a disabled host is ignored even if its thresholds are met ---- */
    c = base(); c.hosts[0].enabled = false;
    memset(&e, 0, sizeof e);
    for (int m = 0; m < 100; m++) feed(&c, &e, BATT, 1, MIN(m));
    OKF(!e.fired[0], "disabled host: never fires");
    OKF(pve_countdown_s(&c, &e, 0, MIN(50)) == -1, "disabled host: no countdown reported");

    /* ---- feature off ---- */
    c = base(); c.enabled = false;
    memset(&e, 0, sizeof e);
    for (int m = 0; m < 100; m++) feed(&c, &e, BATT, 1, MIN(m));
    OKF(!e.fired[0] && !e.on_battery, "feature disabled: never counts, never fires");

    /* ---- no re-arm configured: fires once per boot ---- */
    c = base(); c.mains_back_min = 0;
    memset(&e, 0, sizeof e);
    feed(&c, &e, BATT, 50, MIN(0));
    feed(&c, &e, BATT, 50, MIN(30));
    for (int m = 31; m < 500; m++) feed(&c, &e, LINE, 100, MIN(m));
    OKF(e.fired[0], "mains_back_min = 0: latch never releases by itself");

    /* ---- out-of-range host index ---- */
    OKF(pve_countdown_s(&c, &e, -1, 0) == -1 && pve_countdown_s(&c, &e, PVE_MAX_HOSTS, 0) == -1,
        "countdown for a bad host index is -1, not a read past the array");

    /* ==================================================================
     * Guests: each an independent countdown off the same shared edge,
     * with the node's own firing as a catch-all for anyone not already
     * gone. No per-host cap any more — a guest is "configured" by being
     * present in the list at all, so there's no separate enabled flag and
     * no reserved "empty slot" id.
     * ================================================================== */

    /* ---- a guest's own trigger fires it before the host ---- */
    c = base();   /* host 0: 30 min / 10% */
    reset_engine(&e);
    set_guests(&e, 0, (pve_guest_t[]){ { .id = 101, .on_battery_min = 10 } }, 1);
    OKF(feed(&c, &e, BATT, 90, MIN(0)) == 0 && !guest_fired_now(&e, 0, 101), "t=0: nothing yet");
    OKF(feed(&c, &e, BATT, 85, MIN(10)) == 0 && guest_fired_now(&e, 0, 101),
        "t=10: the guest fires on its own trigger, the host does not");
    OKF(strstr(guest_reason(&e, 0, 101), "10 min") != NULL, "guest reason: '%s'", guest_reason(&e, 0, 101));
    OKF(guest_fired(&e, 0, 101) && !e.fired[0], "guest latched, host not");
    OKF(feed(&c, &e, BATT, 70, MIN(30)) == H0 && !guest_fired_now(&e, 0, 101),
        "t=30: the host fires; the guest, already gone, is not repeated");

    /* ---- a guest with neither trigger set only ever moves with the node ---- */
    c = base();
    reset_engine(&e);
    set_guests(&e, 0, (pve_guest_t[]){ { .id = 102 } }, 1);   /* both triggers off */
    feed(&c, &e, BATT, 95, MIN(0));
    OKF(feed(&c, &e, BATT, 90, MIN(29)) == 0 && !guest_fired_now(&e, 0, 102),
        "t=29: no trigger of its own, quiet");
    OKF(feed(&c, &e, BATT, 89, MIN(30)) == H0 && guest_fired_now(&e, 0, 102),
        "t=30: swept up the moment the node fires");
    OKF(strstr(guest_reason(&e, 0, 102), "with the node") != NULL,
        "reason says so: '%s'", guest_reason(&e, 0, 102));

    /* ---- two guests, two independent countdowns off the same edge ---- */
    c = base();
    reset_engine(&e);
    set_guests(&e, 0, (pve_guest_t[]){
        { .id = 101, .on_battery_min = 5 },
        { .id = 102, .charge_pct = 50 },
    }, 2);
    OKF(pve_guest_countdown_s(&c, &e, &g_guests[0], 0, 101, MIN(0)) == -1, "no countdown before the edge");
    feed(&c, &e, BATT, 90, MIN(0));
    OKF(pve_guest_countdown_s(&c, &e, &g_guests[0], 0, 101, MIN(0)) == 5 * 60, "guest 101: 5:00 to go");
    OKF(pve_guest_countdown_s(&c, &e, &g_guests[0], 0, 102, MIN(0)) == -1, "guest 102 has no timer (charge only)");
    OKF(feed(&c, &e, BATT, 60, MIN(5)) == 0 && guest_fired_now(&e, 0, 101) && !guest_fired_now(&e, 0, 102),
        "guest 101 fires on its timer");
    OKF(feed(&c, &e, BATT, 50, MIN(6)) == 0 && guest_fired_now(&e, 0, 102),
        "guest 102 fires on charge <= 50%%");
    OKF(strstr(guest_reason(&e, 0, 102), "50%") != NULL, "reason: '%s'", guest_reason(&e, 0, 102));

    /* ---- latches release together with the host's, on the shared mains-back edge ---- */
    c = base();
    reset_engine(&e);
    set_guests(&e, 0, (pve_guest_t[]){ { .id = 101, .on_battery_min = 5 } }, 1);
    feed(&c, &e, BATT, 90, MIN(0));
    feed(&c, &e, BATT, 80, MIN(5));                  /* guest fires */
    OKF(guest_fired(&e, 0, 101), "latched");
    feed(&c, &e, LINE, 80, MIN(6));                  /* mains back */
    OKF(feed(&c, &e, LINE, 85, MIN(10)) == 0 && !guest_fired_now(&e, 0, 101) && guest_fired(&e, 0, 101),
        "still latched 4 min into mains-back");
    feed(&c, &e, LINE, 90, MIN(11));
    OKF(!guest_fired(&e, 0, 101), "released at 5 min mains-back, with the host's shared edge");

    /* ---- a host with only a charge trigger never counts down ---- */
    c = base();
    reset_engine(&e);
    set_guests(&e, 0, (pve_guest_t[]){ { .id = 101 } }, 1);
    for (int m = 0; m < 30; m++) feed(&c, &e, BATT, 50, MIN(m));
    OKF(!guest_fired(&e, 0, 101), "a guest present but never triggered, own or swept: never fires on its own");
    OKF(feed(&c, &e, BATT, 50, MIN(30)) == H0 && guest_fired_now(&e, 0, 101),
        "...swept up once the host fires, same as any other configured guest");

    /* ---- a disabled host's guests are inert too ---- */
    c = base(); c.hosts[0].enabled = false;
    reset_engine(&e);
    set_guests(&e, 0, (pve_guest_t[]){ { .id = 101, .on_battery_min = 1 } }, 1);
    for (int m = 0; m < 30; m++) feed(&c, &e, BATT, 50, MIN(m));
    OKF(!guest_fired(&e, 0, 101), "host disabled: its guests never fire either");

    /* ---- unknown guest id: countdown query is -1, not a crash ---- */
    OKF(pve_guest_countdown_s(&c, &e, &g_guests[0], 0, 999, 0) == -1,
        "countdown for an id that isn't in the list is -1");
    OKF(pve_guest_countdown_s(&c, &e, &g_guests[0], -1, 101, 0) == -1 &&
        pve_guest_countdown_s(&c, &e, &g_guests[0], PVE_MAX_HOSTS, 101, 0) == -1,
        "countdown for a bad host index is -1, not a read past the array");

    /* ==================================================================
     * pve_engine_sync_guests: the dynamic-list bookkeeping itself, not
     * routed through an outage — this is what keeps a latch attached to
     * the right guest across an edit instead of a slot position.
     * ================================================================== */

    /* ---- a persisting id keeps its fired state and reason ---- */
    {
        pve_guest_engine_list_t st = { 0 };
        pve_guest_list_t cfg1 = { .items = (pve_guest_t[]){ { .id = 101 }, { .id = 102 } }, .n = 2 };
        pve_engine_sync_guests(&st, &cfg1);
        OKF(st.n == 2 && !st.items[0].fired && !st.items[1].fired,
            "a fresh list starts with nothing fired");
        st.items[0].fired = true;
        strcpy(st.items[0].reason, "on battery for 10 min");

        /* Guest 102 removed, 103 added, 101 (fired) persists. */
        pve_guest_list_t cfg2 = { .items = (pve_guest_t[]){ { .id = 101 }, { .id = 103 } }, .n = 2 };
        pve_engine_sync_guests(&st, &cfg2);
        OKF(st.n == 2, "resized to match the new list");
        bool found101 = false, found103 = false;
        for (int i = 0; i < st.n; i++) {
            if (st.items[i].id == 101) {
                found101 = true;
                OKF(st.items[i].fired && strcmp(st.items[i].reason, "on battery for 10 min") == 0,
                    "101 kept its fired state and reason across the edit");
            }
            if (st.items[i].id == 103) {
                found103 = true;
                OKF(!st.items[i].fired, "103, new to the list, starts unfired");
            }
        }
        OKF(found101 && found103, "both ids present after the sync");
        for (int i = 0; i < st.n; i++) {
            OKF(st.items[i].id != 102, "102, dropped from the list, is gone from the state too");
        }
        free(st.items);
    }

    /* ---- syncing to an empty list frees the state cleanly ---- */
    {
        pve_guest_engine_list_t st = { 0 };
        pve_guest_list_t cfg1 = { .items = (pve_guest_t[]){ { .id = 101 } }, .n = 1 };
        pve_engine_sync_guests(&st, &cfg1);
        pve_guest_list_t empty = { 0 };
        pve_engine_sync_guests(&st, &empty);
        OKF(st.n == 0 && st.items == NULL, "an empty configured list leaves no engine state behind");
    }

    reset_engine(&e);
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
