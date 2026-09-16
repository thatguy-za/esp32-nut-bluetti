/*
 * The decision half of pve_shutdown: pure functions over a small state
 * struct, no I/O, no RTOS — so the host suite can drive an outage through
 * it minute by minute. See pve_shutdown.h for the rules being encoded.
 *
 * One outage, several hosts, an unbounded number of guests per host: the
 * on-battery edge is shared, but each host and each configured guest has
 * its own thresholds and its own fire-once latch, so a host at ten minutes
 * and one at thirty — or a disposable VM at five while its hypervisor
 * waits — are independent countdowns off the same clock. A guest with no
 * trigger of its own still goes down, swept up the moment its host's does.
 *
 * pve_engine_sync_guests() is the one place here that allocates — it keeps
 * st->guests matched to the caller's (unbounded, possibly just-edited)
 * configured guest list. pve_eval() itself still touches no memory beyond
 * what it's handed and stays as pure as the host suite needs.
 */
#include "pve_shutdown.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define US_PER_S   1000000LL
#define US_PER_MIN (60 * US_PER_S)

void pve_engine_sync_guests(pve_guest_engine_list_t *st_guests,
                            const pve_guest_list_t *cfg_guests)
{
    pve_guest_engine_t *fresh = NULL;
    if (cfg_guests->n > 0) {
        fresh = calloc((size_t)cfg_guests->n, sizeof(*fresh));
    }
    for (int i = 0; i < cfg_guests->n; i++) {
        int id = cfg_guests->items[i].id;
        fresh[i].id = id;
        /* Carry over fired/reason for an id that was already tracked;
         * anything new starts unfired. fired_now never carries over — it
         * only ever means "this pve_eval() call". */
        for (int j = 0; j < st_guests->n; j++) {
            if (st_guests->items[j].id == id) {
                fresh[i].fired = st_guests->items[j].fired;
                strlcpy(fresh[i].reason, st_guests->items[j].reason, sizeof(fresh[i].reason));
                break;
            }
        }
    }
    free(st_guests->items);
    st_guests->items = fresh;
    st_guests->n = cfg_guests->n;
}

static pve_guest_engine_t *find_guest_state(pve_guest_engine_list_t *list, int id)
{
    for (int i = 0; i < list->n; i++) {
        if (list->items[i].id == id) {
            return &list->items[i];
        }
    }
    return NULL;
}

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

pve_fire_t pve_eval(const pve_config_t *cfg, const pve_guest_list_t guests[PVE_MAX_HOSTS],
                    pve_engine_t *st, const pve_obs_t *obs, int64_t now_us)
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
        pve_guest_engine_list_t *gst = &st->guests[i];

        for (int g = 0; g < gst->n; g++) {
            gst->items[g].fired_now = false;
        }

        /* Guest latches release with the host's, on the same shared edge —
         * whether or not the host itself is still latched this round. */
        if (mains_back) {
            for (int g = 0; g < gst->n; g++) {
                if (gst->items[g].fired) {
                    gst->items[g].fired = false;
                    gst->items[g].reason[0] = '\0';
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
        for (int g = 0; g < guests[i].n; g++) {
            const pve_guest_t *gg = &guests[i].items[g];
            pve_guest_engine_t *gs = find_guest_state(gst, gg->id);
            if (!gs || gs->fired) {
                continue;
            }
            if (trigger_met(gg->on_battery_min, gg->charge_pct, st, obs, now_us,
                            gs->reason, sizeof(gs->reason))) {
                gs->fired = true;
                gs->fired_now = true;
            }
        }

        if (!trigger_met(h->on_battery_min, h->charge_pct, st, obs, now_us,
                         st->reason[i], sizeof(st->reason[i]))) {
            continue;
        }
        st->fired[i] = true;
        out.hosts |= 1u << i;

        /* The node going down is the deadline every configured guest has
         * to be off by, whatever it's individually set for: sweep up
         * anything not already stopped, the same "stop these guests before
         * the node" this replaces — just for whoever hasn't already gone. */
        for (int g = 0; g < guests[i].n; g++) {
            const pve_guest_t *gg = &guests[i].items[g];
            pve_guest_engine_t *gs = find_guest_state(gst, gg->id);
            if (!gs || gs->fired) {
                continue;
            }
            snprintf(gs->reason, sizeof(gs->reason), "with the node (%s)", st->reason[i]);
            gs->fired = true;
            gs->fired_now = true;
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
                          const pve_guest_list_t *guests, int i, int id,
                          int64_t now_us)
{
    if (i < 0 || i >= PVE_MAX_HOSTS || !cfg->enabled || !cfg->hosts[i].enabled ||
        !st->on_battery) {
        return -1;
    }
    const pve_guest_t *gg = NULL;
    for (int g = 0; g < guests->n; g++) {
        if (guests->items[g].id == id) {
            gg = &guests->items[g];
            break;
        }
    }
    if (!gg || gg->on_battery_min == 0) {
        return -1;
    }
    for (int g = 0; g < st->guests[i].n; g++) {
        if (st->guests[i].items[g].id == id && st->guests[i].items[g].fired) {
            return -1;
        }
    }
    int64_t due = st->on_battery_since_us + (int64_t)gg->on_battery_min * US_PER_MIN;
    int64_t left = due - now_us;
    return left > 0 ? (int)(left / US_PER_S) : 0;
}
