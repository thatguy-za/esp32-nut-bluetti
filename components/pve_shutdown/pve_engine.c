/*
 * The decision half of pve_shutdown: pure functions over a small state
 * struct, no I/O, no RTOS — so the host suite can drive an outage through
 * it minute by minute. See pve_shutdown.h for the rules being encoded.
 */
#include "pve_shutdown.h"

#include <string.h>
#include <stdio.h>

#define US_PER_S   1000000LL
#define US_PER_MIN (60 * US_PER_S)

void pve_obs_from_status(const char *status, int soc_pct, pve_obs_t *out)
{
    memset(out, 0, sizeof(*out));
    out->soc_pct = soc_pct;
    out->power = PVE_PWR_UNKNOWN;
    if (!status) {
        return;
    }
    /* Same reading as notify.c: WAIT is "link up, nothing decoded yet" and
     * OFF is "no link" — neither is a power-state observation. */
    if (strstr(status, "WAIT") || strncmp(status, "OFF", 3) == 0) {
        return;
    }
    if (strstr(status, "OB")) {
        out->power = PVE_PWR_BATTERY;
    } else if (strstr(status, "OL")) {
        out->power = PVE_PWR_LINE;
    }
    out->low_battery = out->power == PVE_PWR_BATTERY && strstr(status, "LB");
}

bool pve_eval(const pve_config_t *cfg, pve_engine_t *st,
              const pve_obs_t *obs, int64_t now_us)
{
    if (!cfg->enabled) {
        /* Off: forget everything, so enabling it mid-outage starts clean. */
        st->on_battery = false;
        st->on_mains = false;
        return false;
    }

    switch (obs->power) {
    case PVE_PWR_BATTERY:
        if (!st->on_battery) {
            st->on_battery = true;
            st->on_battery_since_us = now_us;   /* our clock, from the edge */
        }
        st->on_mains = false;
        break;
    case PVE_PWR_LINE:
        st->on_battery = false;
        if (!st->on_mains) {
            st->on_mains = true;
            st->mains_since_us = now_us;
        }
        break;
    case PVE_PWR_UNKNOWN:
    default:
        /* Fail-safe: unknown never starts a countdown. But one already
         * running keeps running — a confirmed outage does not become less
         * confirmed because the link died in it. Unknown is not mains,
         * either, so a latch does not release on it. */
        st->on_mains = false;
        break;
    }

    if (st->fired) {
        if (cfg->mains_back_min > 0 && st->on_mains &&
            now_us - st->mains_since_us >= (int64_t)cfg->mains_back_min * US_PER_MIN) {
            st->fired = false;
            st->reason[0] = '\0';
        }
        return false;
    }

    /* The on-battery timer needs no live reading; the other two do — a
     * charge figure from before the link dropped says nothing about now. */
    if (cfg->on_battery_min > 0 && st->on_battery &&
        now_us - st->on_battery_since_us >= (int64_t)cfg->on_battery_min * US_PER_MIN) {
        snprintf(st->reason, sizeof(st->reason), "on battery for %u min",
                 (unsigned)cfg->on_battery_min);
        st->fired = true;
        return true;
    }
    if (obs->power == PVE_PWR_BATTERY) {
        if (cfg->charge_pct > 0 && obs->soc_pct >= 0 &&
            obs->soc_pct <= (int)cfg->charge_pct) {
            snprintf(st->reason, sizeof(st->reason), "charge %d%% (limit %u%%)",
                     obs->soc_pct, (unsigned)cfg->charge_pct);
            st->fired = true;
            return true;
        }
        if (cfg->on_low_battery && obs->low_battery) {
            snprintf(st->reason, sizeof(st->reason), "unit reports low battery");
            st->fired = true;
            return true;
        }
    }
    return false;
}

int pve_countdown_s(const pve_config_t *cfg, const pve_engine_t *st,
                    int64_t now_us)
{
    if (!cfg->enabled || cfg->on_battery_min == 0 || !st->on_battery ||
        st->fired) {
        return -1;
    }
    int64_t due = st->on_battery_since_us + (int64_t)cfg->on_battery_min * US_PER_MIN;
    int64_t left = due - now_us;
    return left > 0 ? (int)(left / US_PER_S) : 0;
}
