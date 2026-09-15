/*
 * The decision half of pve_shutdown: pure functions over a small state
 * struct, no I/O, no RTOS — so the host suite can drive an outage through
 * it minute by minute. See pve_shutdown.h for the rules being encoded.
 *
 * One outage, several hosts: the on-battery edge is shared, but each host
 * has its own thresholds and its own fire-once latch, so a host at ten
 * minutes and one at thirty are two countdowns off the same clock.
 */
#include "pve_shutdown.h"

#include <string.h>
#include <stdio.h>

#define US_PER_S   1000000LL
#define US_PER_MIN (60 * US_PER_S)

unsigned pve_eval(const pve_config_t *cfg, pve_engine_t *st,
                  const pve_obs_t *obs, int64_t now_us)
{
    if (!cfg->enabled) {
        /* Off: forget everything, so enabling it mid-outage starts clean. */
        st->on_battery = false;
        st->on_mains = false;
        return 0;
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

    bool mains_back = cfg->mains_back_min > 0 && st->on_mains &&
        now_us - st->mains_since_us >= (int64_t)cfg->mains_back_min * US_PER_MIN;

    unsigned fire = 0;
    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        const pve_host_t *h = &cfg->hosts[i];
        if (st->fired[i]) {
            if (mains_back) {
                st->fired[i] = false;
                st->reason[i][0] = '\0';
            }
            continue;
        }
        if (!h->enabled) {
            continue;
        }
        /* The on-battery timer needs no live reading; the charge test does
         * — a figure from before the link dropped says nothing about now. */
        if (h->on_battery_min > 0 && st->on_battery &&
            now_us - st->on_battery_since_us >= (int64_t)h->on_battery_min * US_PER_MIN) {
            snprintf(st->reason[i], sizeof(st->reason[i]), "on battery for %u min",
                     (unsigned)h->on_battery_min);
        } else if (obs->power == PVE_PWR_BATTERY && h->charge_pct > 0 &&
                   obs->soc_pct >= 0 && obs->soc_pct <= (int)h->charge_pct) {
            snprintf(st->reason[i], sizeof(st->reason[i]), "charge %d%% (limit %u%%)",
                     obs->soc_pct, (unsigned)h->charge_pct);
        } else {
            continue;
        }
        st->fired[i] = true;
        fire |= 1u << i;
    }
    return fire;
}

int pve_countdown_s(const pve_config_t *cfg, const pve_engine_t *st, int i,
                    int64_t now_us)
{
    if (i < 0 || i >= PVE_MAX_HOSTS) {
        return -1;
    }
    const pve_host_t *h = &cfg->hosts[i];
    if (!cfg->enabled || !h->enabled || h->on_battery_min == 0 ||
        !st->on_battery || st->fired[i]) {
        return -1;
    }
    int64_t due = st->on_battery_since_us + (int64_t)h->on_battery_min * US_PER_MIN;
    int64_t left = due - now_us;
    return left > 0 ? (int)(left / US_PER_S) : 0;
}
