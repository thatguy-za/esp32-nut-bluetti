/* Drives the real pve_engine.c through outages minute by minute. This is
 * the code that decides to power off a server, so every rule in
 * pve_shutdown.h's safety model gets a case here: fail-safe on unknown,
 * countdown on our own clock, latch until the mains has been back, the
 * charge/LB triggers needing a live reading, and the exact status strings
 * the rest of the firmware produces. No I/O is involved — the runtime half
 * (TLS, the API, the sequence) is not covered and needs a real host. */
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
#define SEC(n) ((int64_t)(n) * 1000000LL)

static pve_config_t base(void)
{
    pve_config_t c;
    memset(&c, 0, sizeof c);
    c.enabled = true;
    c.armed = true;
    c.on_battery_min = 30;
    c.charge_pct = 10;
    c.on_low_battery = true;
    c.mains_back_min = 5;
    return c;
}

/* Feed a status string at time t. */
static bool feed(const pve_config_t *c, pve_engine_t *e, const char *status,
                 int soc, int64_t t)
{
    pve_obs_t o;
    pve_obs_from_status(status, soc, &o);
    return pve_eval(c, e, &o, t);
}

int main(void)
{
    pve_config_t c = base();
    pve_engine_t e;

    /* ---- status parsing ---- */
    {
        pve_obs_t o;
        pve_obs_from_status("OL CHRG", 80, &o);
        OKF(o.power == PVE_PWR_LINE, "\"OL CHRG\" -> on line");
        pve_obs_from_status("OB DISCHRG", 60, &o);
        OKF(o.power == PVE_PWR_BATTERY && !o.low_battery, "\"OB DISCHRG\" -> on battery, not low");
        pve_obs_from_status("OB LB DISCHRG", 8, &o);
        OKF(o.power == PVE_PWR_BATTERY && o.low_battery, "\"OB LB DISCHRG\" -> on battery, low");
        pve_obs_from_status("OL WAIT", 0, &o);
        OKF(o.power == PVE_PWR_UNKNOWN, "\"OL WAIT\" is not a power reading");
        pve_obs_from_status("OFF", 0, &o);
        OKF(o.power == PVE_PWR_UNKNOWN, "\"OFF\" is not a power reading");
        pve_obs_from_status("OL LB", 5, &o);
        OKF(o.power == PVE_PWR_LINE && !o.low_battery,
            "LB on line is ignored (it is not a battery observation)");
        pve_obs_from_status(NULL, 50, &o);
        OKF(o.power == PVE_PWR_UNKNOWN, "NULL -> unknown");
    }

    /* ---- t = 0 is a valid moment, not "unset" ---- */
    memset(&e, 0, sizeof e);
    OKF(!feed(&c, &e, "OB DISCHRG", 90, 0) && e.on_battery,
        "an outage that begins at timestamp 0 still starts the countdown");
    OKF(feed(&c, &e, "OB DISCHRG", 90, MIN(30)), "...and fires at 30 min");

    /* ---- the on-battery countdown, on our clock ---- */
    memset(&e, 0, sizeof e);
    OKF(!feed(&c, &e, "OL", 100, MIN(0)), "on line: nothing");
    OKF(pve_countdown_s(&c, &e, MIN(0)) == -1, "no countdown on line");
    OKF(!feed(&c, &e, "OB DISCHRG", 95, MIN(1)), "t=1: mains drops, countdown starts");
    OKF(pve_countdown_s(&c, &e, MIN(1)) == 30 * 60, "30:00 to go at the edge");
    OKF(pve_countdown_s(&c, &e, MIN(21)) == 10 * 60, "10:00 to go, twenty minutes in");
    OKF(!feed(&c, &e, "OB DISCHRG", 60, MIN(30)), "t=30: 29 min on battery, not yet");
    OKF(feed(&c, &e, "OB DISCHRG", 58, MIN(31)), "t=31: 30 min on battery -> FIRE");
    OKF(strstr(e.reason, "30 min") != NULL, "reason names the trigger: '%s'", e.reason);
    OKF(e.fired, "latched");
    OKF(!feed(&c, &e, "OB DISCHRG", 50, MIN(32)), "still on battery: fires only once");
    OKF(pve_countdown_s(&c, &e, MIN(32)) == -1, "no countdown shown while latched");

    /* ---- the latch: mains back, but not for long enough ---- */
    OKF(!feed(&c, &e, "OL CHRG", 50, MIN(40)), "t=40: mains back");
    OKF(e.fired, "still latched the moment it returns");
    OKF(!feed(&c, &e, "OL CHRG", 55, MIN(44)), "t=44: back 4 min, still latched");
    OKF(e.fired, "latch holds through mains_back_min");
    OKF(!feed(&c, &e, "OL CHRG", 56, MIN(45)), "t=45: back 5 min");
    OKF(!e.fired, "released after mains_back_min");
    OKF(feed(&c, &e, "OB DISCHRG", 56, MIN(46)) == false &&
        feed(&c, &e, "OB DISCHRG", 56, MIN(76)) == true,
        "a second outage after release fires again (fresh 30 min)");

    /* ---- a flap inside the latch must not fire twice ---- */
    memset(&e, 0, sizeof e);
    feed(&c, &e, "OB DISCHRG", 90, MIN(0));
    OKF(feed(&c, &e, "OB DISCHRG", 70, MIN(30)), "fires");
    feed(&c, &e, "OL", 70, MIN(31));           /* back for a minute */
    feed(&c, &e, "OB DISCHRG", 70, MIN(32));   /* gone again */
    OKF(e.fired && !feed(&c, &e, "OB DISCHRG", 40, MIN(70)),
        "a 1-min mains blip inside the latch does not re-fire, even 38 min later");

    /* ---- mains returning resets the countdown ---- */
    memset(&e, 0, sizeof e);
    feed(&c, &e, "OB DISCHRG", 90, MIN(0));
    feed(&c, &e, "OL", 90, MIN(20));
    OKF(pve_countdown_s(&c, &e, MIN(20)) == -1, "mains back at 20 min: countdown gone");
    feed(&c, &e, "OB DISCHRG", 90, MIN(25));
    OKF(!feed(&c, &e, "OB DISCHRG", 90, MIN(50)), "new outage: 25 min in, not 50");
    OKF(feed(&c, &e, "OB DISCHRG", 90, MIN(55)), "fires 30 min after the *second* edge");

    /* ---- fail-safe: unknown never starts anything ---- */
    memset(&e, 0, sizeof e);
    OKF(!feed(&c, &e, "OFF", -1, MIN(0)), "link down from boot: nothing");
    OKF(!feed(&c, &e, "OL WAIT", -1, MIN(1)), "link up, no data: nothing");
    OKF(!e.on_battery, "no countdown was started blind");
    for (int m = 2; m < 120; m++) feed(&c, &e, "OFF", -1, MIN(m));
    OKF(!e.fired, "two hours of no contact: still nothing (fail-safe)");

    /* ---- but a running countdown survives losing the link ---- */
    memset(&e, 0, sizeof e);
    feed(&c, &e, "OB DISCHRG", 80, MIN(0));
    feed(&c, &e, "OFF", -1, MIN(5));            /* link dies mid-outage */
    OKF(e.on_battery && e.on_battery_since_us == MIN(0), "countdown kept through OFF");
    OKF(!feed(&c, &e, "OFF", -1, MIN(29)), "29 min, blind: not yet");
    OKF(feed(&c, &e, "OFF", -1, MIN(30)), "30 min, blind: fires on the timer");
    OKF(strstr(e.reason, "30 min") != NULL, "...on the on-battery trigger only");

    /* ---- and the latch does not release on unknown ---- */
    OKF(!feed(&c, &e, "OFF", -1, MIN(60)) && e.fired,
        "blind for 30 min after firing: unknown is not 'mains back'");

    /* ---- the charge and LB triggers need a live reading ---- */
    memset(&e, 0, sizeof e);
    feed(&c, &e, "OB DISCHRG", 50, MIN(0));
    OKF(!feed(&c, &e, "OB DISCHRG", 11, MIN(1)), "11%%: above the 10%% limit");
    OKF(feed(&c, &e, "OB DISCHRG", 10, MIN(2)), "10%%: at the limit -> FIRE");
    OKF(strstr(e.reason, "10%") != NULL, "reason: '%s'", e.reason);

    memset(&e, 0, sizeof e);
    feed(&c, &e, "OB DISCHRG", 50, MIN(0));
    feed(&c, &e, "OFF", 5, MIN(1));             /* stale 5% while blind */
    OKF(!e.fired, "a charge figure with no link behind it does not fire");

    memset(&e, 0, sizeof e);
    OKF(feed(&c, &e, "OB LB DISCHRG", 30, MIN(0)),
        "LB from the unit fires immediately (charge alone would not: 30%%)");
    OKF(strstr(e.reason, "low battery") != NULL, "reason: '%s'", e.reason);

    memset(&e, 0, sizeof e);
    OKF(!feed(&c, &e, "OL LB", 5, MIN(0)) && !feed(&c, &e, "OL", 5, MIN(1)),
        "5%% charge on the mains is not a trigger");

    /* ---- triggers switched off ---- */
    c = base(); c.charge_pct = 0; c.on_low_battery = false;
    memset(&e, 0, sizeof e);
    OKF(!feed(&c, &e, "OB LB DISCHRG", 1, MIN(0)),
        "1%% + LB with only the timer enabled: nothing yet");
    OKF(feed(&c, &e, "OB LB DISCHRG", 1, MIN(30)), "...until the timer");

    c = base(); c.on_battery_min = 0;
    memset(&e, 0, sizeof e);
    feed(&c, &e, "OB DISCHRG", 50, MIN(0));
    OKF(pve_countdown_s(&c, &e, MIN(0)) == -1, "timer off: no countdown");
    OKF(!feed(&c, &e, "OB DISCHRG", 50, MIN(500)), "timer off: 500 min on battery, nothing");
    OKF(feed(&c, &e, "OB DISCHRG", 9, MIN(501)), "...but charge still fires");

    /* ---- feature off ---- */
    c = base(); c.enabled = false;
    memset(&e, 0, sizeof e);
    for (int m = 0; m < 100; m++) feed(&c, &e, "OB LB DISCHRG", 1, MIN(m));
    OKF(!e.fired && !e.on_battery, "disabled: never counts, never fires");

    /* ---- no re-arm configured: fires once per boot ---- */
    c = base(); c.mains_back_min = 0;
    memset(&e, 0, sizeof e);
    feed(&c, &e, "OB DISCHRG", 50, MIN(0));
    feed(&c, &e, "OB DISCHRG", 50, MIN(30));
    for (int m = 31; m < 500; m++) feed(&c, &e, "OL", 100, MIN(m));
    OKF(e.fired, "mains_back_min = 0: latch never releases by itself");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
