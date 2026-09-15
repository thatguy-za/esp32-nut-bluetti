/*
 * The decision half of pve_shutdown: pure functions over a small state
 * struct, no I/O, no RTOS — so the host suite can drive an outage through
 * it minute by minute. See pve_shutdown.h for the rules being encoded.
 *
 * One outage, several hosts, several guests per host: the on-battery edge
 * is shared, but each host and each configured guest has its own
 * thresholds and its own fire-once latch, so a host at ten minutes and one
 * at thirty — or a disposable VM at five while its hypervisor waits — are
 * independent countdowns off the same clock. A guest with no trigger of
 * its own still goes down, swept up the moment its host's does.
 */
#include "pve_shutdown.h"

#include <string.h>
#include <stdio.h>

#define US_PER_S   1000000LL
#define US_PER_MIN (60 * US_PER_S)

/* Would this trigger (on_battery_min / charge_pct) fire right now? Shared
 * by a host and by a guest — the rule is identical, just applied to
 * whichever thresholds and reason buffer belong to the caller. */
static bool trigger_met(uint16_t on_battery_min, uint8_t charge_pct,
                        const pve_engine_t *st, const pve_obs_t *obs,
                        int64_t now_us, char *reason, size_t reason_sz)
{
    /* The on-battery timer needs no live reading; the charge test does —
     * a figure from before the link dropped says nothing about now. */
    if (on_battery_min > 0 && st->on_battery &&
        now_us - st->on_battery_since_us >= (int64_t)on_battery_min * US_PER_MIN) {
        snprintf(reason, reason_sz, "on battery for %u min", (unsigned)on_battery_min);
        return true;
    }
    if (obs->power == PVE_PWR_BATTERY && charge_pct > 0 &&
        obs->soc_pct >= 0 && obs->soc_pct <= (int)charge_pct) {
        snprintf(reason, reason_sz, "charge %d%% (limit %u%%)", obs->soc_pct,
                 (unsigned)charge_pct);
        return true;
    }
    return false;
}

pve_fire_t pve_eval(const pve_config_t *cfg, pve_engine_t *st,
                    const pve_obs_t *obs, int64_t now_us)
{
    pve_fire_t out = { 0 };
    if (!cfg->enabled) {
        /* Off: forget everything, so enabling it mid-outage starts clean. */
        st->on_battery = false;
        st->on_mains = false;
        return out;
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

    for (int i = 0; i < PVE_MAX_HOSTS; i++) {
        const pve_host_t *h = &cfg->hosts[i];

        /* Guest latches release with the host's, on the same shared edge —
         * whether or not the host itself is still latched this round. */
        if (mains_back) {
            for (int g = 0; g < PVE_MAX_GUESTS; g++) {
                if (st->guest_fired[i][g]) {
                    st->guest_fired[i][g] = false;
                    st->guest_reason[i][g][0] = '\0';
                }
            }
        }

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

        /* Each guest's own trigger, independent of the host's — a
         * disposable VM can go early while the hypervisor waits. */
        for (int g = 0; g < PVE_MAX_GUESTS; g++) {
            const pve_guest_t *gg = &h->guests[g];
            if (st->guest_fired[i][g] || !gg->enabled || gg->id == 0) {
                continue;
            }
            if (trigger_met(gg->on_battery_min, gg->charge_pct, st, obs, now_us,
                            st->guest_reason[i][g], sizeof(st->guest_reason[i][g]))) {
                st->guest_fired[i][g] = true;
                out.guests[i] |= 1u << g;
            }
        }

        if (!trigger_met(h->on_battery_min, h->charge_pct, st, obs, now_us,
                         st->reason[i], sizeof(st->reason[i]))) {
            continue;
        }
        st->fired[i] = true;
        out.hosts |= 1u << i;

        /* The node going down is the deadline every enabled guest has to
         * be off by, whatever it's individually configured for: sweep up
         * anything not already stopped, the same "stop these guests before
         * the node" this replaces — just for whoever hasn't already gone. */
        for (int g = 0; g < PVE_MAX_GUESTS; g++) {
            const pve_guest_t *gg = &h->guests[g];
            if (st->guest_fired[i][g] || !gg->enabled || gg->id == 0) {
                continue;
            }
            snprintf(st->guest_reason[i][g], sizeof(st->guest_reason[i][g]),
                     "with the node (%s)", st->reason[i]);
            st->guest_fired[i][g] = true;
            out.guests[i] |= 1u << g;
        }
    }
    return out;
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

int pve_guest_countdown_s(const pve_config_t *cfg, const pve_engine_t *st,
                          int i, int g, int64_t now_us)
{
    if (i < 0 || i >= PVE_MAX_HOSTS || g < 0 || g >= PVE_MAX_GUESTS) {
        return -1;
    }
    const pve_host_t *h = &cfg->hosts[i];
    const pve_guest_t *gg = &h->guests[g];
    if (!cfg->enabled || !h->enabled || !gg->enabled || gg->on_battery_min == 0 ||
        !st->on_battery || st->guest_fired[i][g]) {
        return -1;
    }
    int64_t due = st->on_battery_since_us + (int64_t)gg->on_battery_min * US_PER_MIN;
    int64_t left = due - now_us;
    return left > 0 ? (int)(left / US_PER_S) : 0;
}
