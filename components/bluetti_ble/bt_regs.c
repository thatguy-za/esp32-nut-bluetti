#include "bt_regs.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bt_regs";

const bt_device_t BT_DEVICE_EL10    = { "EL10", true };
const bt_device_t BT_DEVICE_GENERIC = { "unknown", false };

const bt_device_t *bt_device_lookup(const char *name)
{
    if (!name || !name[0]) {
        return NULL;
    }
    /* EL10 and EL100V2 share an identical field list and scaling. Any
     * other model falls through to the generic (charge + power) path. */
    static const char *FAMILY[] = { "EL10", "EL100V2" };
    for (size_t i = 0; i < sizeof(FAMILY) / sizeof(FAMILY[0]); i++) {
        size_t n = strlen(FAMILY[i]);
        if (strncmp(name, FAMILY[i], n) != 0) {
            continue;
        }
        size_t j = n;
        while (name[j] >= '0' && name[j] <= '9') {
            j++;
        }
        if (name[j] == '\0') {
            return &BT_DEVICE_EL10;
        }
    }
    return NULL;
}

size_t bt_regs_plan(const bt_device_t *dev, bool with_controls,
                    bt_reg_read_t *out, size_t max)
{
    /* Common to every V2 unit, most-important-first. */
    static const bt_reg_read_t COMMON[] = {
        { REG_BATTERY_SOC,     1 },
        { REG_AC_INPUT_POWER,  1 },
        { REG_AC_OUTPUT_POWER, 1 },
        { REG_DC_INPUT_POWER,  1 },
        { REG_DC_OUTPUT_POWER, 1 },
        { REG_DEVICE_TYPE,     6 },
    };
    /* Elite-10 extras: telemetry, then the two output switches. */
    static const bt_reg_read_t EL10_EXTRA[] = {
        { REG_TIME_REMAINING,    1 },
        { REG_AC_INPUT_VOLTAGE,  1 },
        { REG_AC_INPUT_CURRENT,  1 },
        { REG_AC_OUTPUT_VOLTAGE, 1 },
        { REG_CTRL_AC,           1 },
        { REG_CTRL_DC,           1 },
        { REG_DEVICE_SN,         4 },
    };
    /* Only polled when device controls are enabled, so a monitoring-only
     * setup keeps a short, fast sweep. */
    static const bt_reg_read_t EL10_CONTROLS[] = {
        { REG_CTRL_ECO_DC,        1 },
        { REG_CTRL_ECO_MODE_DC,   1 },
        { REG_CTRL_ECO_AC,        1 },
        { REG_CTRL_ECO_MODE_AC,   1 },
        { REG_CTRL_CHARGING_MODE, 1 },
        { REG_CTRL_POWER_LIFTING, 1 },
        { REG_CTRL_DISPLAY_TIME,  1 },
    };

    size_t n = 0;
    for (size_t i = 0; i < sizeof(COMMON) / sizeof(COMMON[0]) && n < max; i++) {
        out[n++] = COMMON[i];
    }
    if (dev && dev->full) {
        for (size_t i = 0;
             i < sizeof(EL10_EXTRA) / sizeof(EL10_EXTRA[0]) && n < max; i++) {
            out[n++] = EL10_EXTRA[i];
        }
        if (with_controls) {
            for (size_t i = 0;
                 i < sizeof(EL10_CONTROLS) / sizeof(EL10_CONTROLS[0]) && n < max;
                 i++) {
                out[n++] = EL10_CONTROLS[i];
            }
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Writeable controls                                                  */
/* ------------------------------------------------------------------ */

const bt_control_t BT_CONTROLS[] = {
    { "ac_output",       REG_CTRL_AC,             true,  NULL      },
    { "dc_output",       REG_CTRL_DC,             true,  NULL      },
    { "eco_ac",          REG_CTRL_ECO_AC,         true,  NULL      },
    { "eco_dc",          REG_CTRL_ECO_DC,         true,  NULL      },
    { "eco_mode_ac",     REG_CTRL_ECO_MODE_AC,    false, "1,2,3,4" },
    { "eco_mode_dc",     REG_CTRL_ECO_MODE_DC,    false, "1,2,3,4" },
    { "charging_mode",   REG_CTRL_CHARGING_MODE,  false, "0,1,2,4" },
    { "power_lifting",   REG_CTRL_POWER_LIFTING,  true,  NULL      },
    { "display_time",    REG_CTRL_DISPLAY_TIME,   false, "2,3,4,5" },
};
const size_t BT_CONTROL_COUNT = sizeof(BT_CONTROLS) / sizeof(BT_CONTROLS[0]);

const bt_control_t *bt_control_lookup(const char *field)
{
    if (!field) {
        return NULL;
    }
    for (size_t i = 0; i < BT_CONTROL_COUNT; i++) {
        if (strcmp(field, BT_CONTROLS[i].field) == 0) {
            return &BT_CONTROLS[i];
        }
    }
    return NULL;
}

bool bt_control_valid(const bt_control_t *c, int value)
{
    if (!c) {
        return false;
    }
    if (c->is_bool) {
        return value == 0 || value == 1;
    }
    /* Membership in the comma-separated allow list. */
    for (const char *p = c->allowed; p && *p; ) {
        int v = 0;
        bool digits = false;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); digits = true; }
        if (digits && v == value) {
            return true;
        }
        while (*p == ',') p++;
    }
    return false;
}

int bt_control_current(const bt_control_t *c, const bluetti_state_t *st)
{
    if (!c || !st) {
        return -1;
    }
    switch (c->reg) {
    case REG_CTRL_AC:             return st->ac_switch;
    case REG_CTRL_DC:             return st->dc_switch;
    case REG_CTRL_ECO_AC:         return st->eco_ac;
    case REG_CTRL_ECO_DC:         return st->eco_dc;
    case REG_CTRL_ECO_MODE_AC:    return st->eco_mode_ac;
    case REG_CTRL_ECO_MODE_DC:    return st->eco_mode_dc;
    case REG_CTRL_CHARGING_MODE:  return st->charging_mode;
    case REG_CTRL_POWER_LIFTING:  return st->power_lifting;
    case REG_CTRL_DISPLAY_TIME:   return st->display_time;
    default:                      return -1;
    }
}

/* One register out of a single field's response, or -1 if not present. */
static int reg(uint16_t start, const uint8_t *data, size_t len, uint16_t addr)
{
    if (addr < start) {
        return -1;
    }
    size_t idx = (size_t)(addr - start) * 2;
    if (idx + 1 >= len) {
        return -1;
    }
    return ((int)data[idx] << 8) | data[idx + 1];
}

/*
 * BLUETTI stores text as 16-bit words with the bytes swapped within each
 * word, so a word 0x3130 reads as "01".
 */
static void swap_string(uint16_t start, const uint8_t *data, size_t len,
                        uint16_t addr, int words, char *out, size_t out_sz)
{
    size_t o = 0;
    for (int w = 0; w < words && o + 2 < out_sz; w++) {
        int v = reg(start, data, len, addr + w);
        if (v < 0) {
            break;
        }
        char lo = (char)(v & 0xFF);
        char hi = (char)((v >> 8) & 0xFF);
        if (lo) out[o++] = lo;
        if (hi) out[o++] = hi;
    }
    out[o] = '\0';
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\0')) {
        out[--o] = '\0';
    }
}

/* Recompute everything derived from the raw fields accumulated in `st`. */
static void recompute(bluetti_state_t *st)
{
    float ac_out = st->ac_out_watts  >= 0.0f ? st->ac_out_watts  : 0.0f;
    float dc_out = st->dc_out_watts  >= 0.0f ? st->dc_out_watts  : 0.0f;
    if (st->ac_out_watts >= 0.0f || st->dc_out_watts >= 0.0f) {
        st->output_watts = ac_out + dc_out;
    }

    float ac_in = st->ac_in_watts >= 0.0f ? st->ac_in_watts : 0.0f;
    float dc_in = st->dc_in_watts >= 0.0f ? st->dc_in_watts : 0.0f;
    if (st->ac_in_watts >= 0.0f || st->dc_in_watts >= 0.0f) {
        st->input_watts = ac_in + dc_in;
    }

    /* No explicit mains flag in the map: infer it from AC input power, or
     * line voltage (a plugged-in but idle unit reads 0 W but shows volts). */
    st->ac_input_present = (st->ac_in_watts  > 0.0f) ||
                           (st->ac_in_volts  > 0.0f);
    st->charging = st->ac_input_present && st->soc_pct >= 0 && st->soc_pct < 100;

    if (st->input_watts >= 0.0f && st->output_watts >= 0.0f) {
        /* NUT-layer sign convention: >0 = discharging. */
        st->battery_watts = st->output_watts - st->input_watts;
    }
}

int bt_regs_apply(const bt_device_t *dev, uint16_t start_addr,
                  const uint8_t *data, size_t len, bluetti_state_t *st)
{
    int matched = 0;
    int v;

    if (!dev) {
        dev = &BT_DEVICE_GENERIC;
    }

    if ((v = reg(start_addr, data, len, REG_BATTERY_SOC)) >= 0 && v <= 100) {
        st->soc_pct = v;
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_DEVICE_TYPE)) >= 0) {
        swap_string(start_addr, data, len, REG_DEVICE_TYPE, 6,
                    st->model, sizeof(st->model));
        matched++;
    }

    if ((v = reg(start_addr, data, len, REG_DC_OUTPUT_POWER)) >= 0) {
        st->dc_out_watts = (float)v;
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_AC_OUTPUT_POWER)) >= 0) {
        st->ac_out_watts = (float)v;
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_DC_INPUT_POWER)) >= 0) {
        st->dc_in_watts = (float)v;
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_AC_INPUT_POWER)) >= 0) {
        st->ac_in_watts = (float)v;
        matched++;
    }

    if (dev->full) {
        if ((v = reg(start_addr, data, len, REG_TIME_REMAINING)) >= 0) {
            /* Raw is minutes on the EL10 family. 0 covers both "full" and
             * "no estimate" — treat as unknown, not "no runtime left". */
            st->minutes_remaining = v > 0 ? v : BLUETTI_UNKNOWN_I;
            matched++;
        }
        if ((v = reg(start_addr, data, len, REG_DEVICE_SN)) >= 0) {
            /* Four words, least-significant first. */
            uint64_t sn = 0;
            bool ok = true;
            for (int w = 3; w >= 0; w--) {
                int x = reg(start_addr, data, len, REG_DEVICE_SN + w);
                if (x < 0) { ok = false; break; }
                sn = (sn << 16) | (uint16_t)x;
            }
            if (ok && sn) {
                snprintf(st->serial, sizeof(st->serial), "%llu",
                         (unsigned long long)sn);
                matched++;
            }
        }
        if ((v = reg(start_addr, data, len, REG_AC_INPUT_VOLTAGE)) >= 0) {
            st->ac_in_volts = (float)v / 10.0f;
            matched++;
        }
        if ((v = reg(start_addr, data, len, REG_AC_INPUT_CURRENT)) >= 0) {
            st->ac_in_amps = (float)v / 10.0f;
            matched++;
        }
        if ((v = reg(start_addr, data, len, REG_AC_OUTPUT_VOLTAGE)) >= 0) {
            st->ac_out_volts = (float)v / 10.0f;
            matched++;
        }
        /* Switch fields are strictly 0 or 1; anything else means the
         * register is not really there (bluetti-bt-lib returns None). */
        if ((v = reg(start_addr, data, len, REG_CTRL_AC)) == 0 || v == 1) {
            st->ac_switch = v; matched++;
        }
        if ((v = reg(start_addr, data, len, REG_CTRL_DC)) == 0 || v == 1) {
            st->dc_switch = v; matched++;
        }
        if ((v = reg(start_addr, data, len, REG_CTRL_ECO_AC)) == 0 || v == 1) {
            st->eco_ac = v; matched++;
        }
        if ((v = reg(start_addr, data, len, REG_CTRL_ECO_DC)) == 0 || v == 1) {
            st->eco_dc = v; matched++;
        }
        if ((v = reg(start_addr, data, len, REG_CTRL_POWER_LIFTING)) == 0 || v == 1) {
            st->power_lifting = v; matched++;
        }
        if ((v = reg(start_addr, data, len, REG_CTRL_ECO_MODE_AC)) >= 1 && v <= 4) {
            st->eco_mode_ac = v; matched++;
        }
        if ((v = reg(start_addr, data, len, REG_CTRL_ECO_MODE_DC)) >= 1 && v <= 4) {
            st->eco_mode_dc = v; matched++;
        }
        if ((v = reg(start_addr, data, len, REG_CTRL_CHARGING_MODE)) >= 0 &&
            (v == 0 || v == 1 || v == 2 || v == 4)) {
            st->charging_mode = v; matched++;
        }
        if ((v = reg(start_addr, data, len, REG_CTRL_DISPLAY_TIME)) >= 2 && v <= 5) {
            st->display_time = v; matched++;
        }
    }

    if (matched > 0) {
        recompute(st);
        st->valid = true;
        st->updated_us = esp_timer_get_time();
    }
    ESP_LOGD(TAG, "field @%u len %u -> %d", start_addr, (unsigned)len, matched);
    return matched;
}
