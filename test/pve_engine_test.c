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

static unsigned feed(const pve_config_t *c, pve_engine_t *e, pve_power_t pw,
                     int soc, int64_t t)
{
    pve_obs_t o = { .power = pw, .soc_pct = soc };
    return pve_eval(c, e, &o, t).hosts;
}
/* Full result, for tests that need the guest bits too. */
static pve_fire_t feedf(const pve_config_t *c, pve_engine_t *e, pve_power_t pw,
                        int soc, int64_t t)
{
    pve_obs_t o = { .power = pw, .soc_pct = soc };
    return pve_eval(c, e, &o, t);
}
#define G0 (1u << 0)
#define G1 (1u << 1)
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
     * gone.
     * ================================================================== */

    /* ---- a guest's own trigger fires it before the host ---- */
    c = base();   /* host 0: 30 min / 10% */
    c.hosts[0].guests[0] = (pve_guest_t){ .id = 101, .enabled = true, .on_battery_min = 10 };
    memset(&e, 0, sizeof e);
    pve_fire_t f = feedf(&c, &e, BATT, 90, MIN(0));
    OKF(f.hosts == 0 && f.guests[0] == 0, "t=0: nothing yet");
    f = feedf(&c, &e, BATT, 85, MIN(10));
    OKF(f.hosts == 0 && f.guests[0] == G0, "t=10: the guest fires on its own trigger, the host does not");
    OKF(strstr(e.guest_reason[0][0], "10 min") != NULL, "guest reason: '%s'", e.guest_reason[0][0]);
    OKF(e.guest_fired[0][0] && !e.fired[0], "guest latched, host not");
    f = feedf(&c, &e, BATT, 70, MIN(30));
    OKF(f.hosts == H0 && f.guests[0] == 0, "t=30: the host fires; the guest, already gone, is not repeated");

    /* ---- a guest with neither trigger set only ever moves with the node ---- */
    c = base();
    c.hosts[0].guests[0] = (pve_guest_t){ .id = 102, .enabled = true };  /* both triggers off */
    memset(&e, 0, sizeof e);
    feedf(&c, &e, BATT, 95, MIN(0));
    OKF(feedf(&c, &e, BATT, 90, MIN(29)).guests[0] == 0, "t=29: no trigger of its own, quiet");
    f = feedf(&c, &e, BATT, 89, MIN(30));
    OKF(f.hosts == H0 && f.guests[0] == G0, "t=30: swept up the moment the node fires");
    OKF(strstr(e.guest_reason[0][0], "with the node") != NULL, "reason says so: '%s'", e.guest_reason[0][0]);

    /* ---- two guests, two independent countdowns off the same edge ---- */
    c = base();
    c.hosts[0].guests[0] = (pve_guest_t){ .id = 101, .enabled = true, .on_battery_min = 5 };
    c.hosts[0].guests[1] = (pve_guest_t){ .id = 102, .enabled = true, .charge_pct = 50 };
    memset(&e, 0, sizeof e);
    OKF(pve_guest_countdown_s(&c, &e, 0, 0, MIN(0)) == -1, "no countdown before the edge");
    feedf(&c, &e, BATT, 90, MIN(0));
    OKF(pve_guest_countdown_s(&c, &e, 0, 0, MIN(0)) == 5 * 60, "guest 0: 5:00 to go");
    OKF(pve_guest_countdown_s(&c, &e, 0, 1, MIN(0)) == -1, "guest 1 has no timer (charge only)");
    f = feedf(&c, &e, BATT, 60, MIN(5));
    OKF(f.guests[0] == G0, "guest 0 fires on its timer");
    f = feedf(&c, &e, BATT, 50, MIN(6));
    OKF(f.guests[0] == G1, "guest 1 fires on charge <= 50%%");
    OKF(strstr(e.guest_reason[0][1], "50%") != NULL, "reason: '%s'", e.guest_reason[0][1]);

    /* ---- latches release together with the host's, on the shared mains-back edge ---- */
    c = base();
    c.hosts[0].guests[0] = (pve_guest_t){ .id = 101, .enabled = true, .on_battery_min = 5 };
    memset(&e, 0, sizeof e);
    feedf(&c, &e, BATT, 90, MIN(0));
    feedf(&c, &e, BATT, 80, MIN(5));                 /* guest fires */
    OKF(e.guest_fired[0][0], "latched");
    feedf(&c, &e, LINE, 80, MIN(6));                 /* mains back */
    OKF(feedf(&c, &e, LINE, 85, MIN(10)).guests[0] == 0 && e.guest_fired[0][0],
        "still latched 4 min into mains-back");
    feedf(&c, &e, LINE, 90, MIN(11));
    OKF(!e.guest_fired[0][0], "released at 5 min mains-back, with the host's shared edge");

    /* ---- disabled slot / empty id: never fires, never swept ---- */
    c = base();
    c.hosts[0].guests[0] = (pve_guest_t){ .id = 101, .enabled = false, .on_battery_min = 1 };
    c.hosts[0].guests[1] = (pve_guest_t){ .id = 0, .enabled = true, .on_battery_min = 1 };
    memset(&e, 0, sizeof e);
    for (int m = 0; m < 30; m++) feedf(&c, &e, BATT, 50, MIN(m));
    OKF(!e.guest_fired[0][0] && !e.guest_fired[0][1], "disabled and id-0 slots: never fire on their own");
    f = feedf(&c, &e, BATT, 50, MIN(30));
    OKF((f.guests[0] & (G0 | G1)) == 0, "...and not swept up when the host fires either");

    /* ---- a disabled host's guests are inert too ---- */
    c = base(); c.hosts[0].enabled = false;
    c.hosts[0].guests[0] = (pve_guest_t){ .id = 101, .enabled = true, .on_battery_min = 1 };
    memset(&e, 0, sizeof e);
    for (int m = 0; m < 30; m++) feedf(&c, &e, BATT, 50, MIN(m));
    OKF(!e.guest_fired[0][0], "host disabled: its guests never fire either");

    /* ---- out-of-range guest index ---- */
    OKF(pve_guest_countdown_s(&c, &e, 0, -1, 0) == -1 &&
        pve_guest_countdown_s(&c, &e, 0, PVE_MAX_GUESTS, 0) == -1,
        "countdown for a bad guest slot is -1, not a read past the array");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
