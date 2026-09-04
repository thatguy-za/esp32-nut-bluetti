#include "bt_regs.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bt_regs";

const bt_device_t BT_DEVICE_GENERIC = { "unknown", false, 0 };

/* The output-switch pair every controllable model shares. */
#define C_OUT   (BT_C_AC_OUT | BT_C_DC_OUT)
/* AC + DC output, both ECO modes and their timeouts, charging mode,
 * power lifting — the set the EL10, AC70, AC180 and EL30V2 all carry. */
#define C_FULL  (C_OUT | BT_C_ECO_AC | BT_C_ECO_DC | \
                 BT_C_ECO_MODE_AC | BT_C_ECO_MODE_DC | \
                 BT_C_CHARGE_MODE | BT_C_POWER_LIFT)

/*
 * Models recognised for control. `full` (full telemetry decode) is only
 * the Elite-10 family; the control mask is wider because the control
 * registers do not need per-model scaling. From bluetti-bt-lib's
 * SwitchField / SelectField / UIntField definitions.
 *
 * SOC min/max (2022/2023): declared for the EL100V2 upstream; also polled
 * on the EL10, where it is unconfirmed — the control only appears if the
 * register actually answers.
 */
static const bt_device_t DEVICES[] = {
    { "EL100V2",     true,  C_FULL | BT_C_DISPLAY | BT_C_SOC_MIN | BT_C_SOC_MAX },
    { "EL10",        true,  C_FULL | BT_C_DISPLAY | BT_C_SOC_MIN | BT_C_SOC_MAX },
    { "AC70",        false, C_FULL },
    { "AC180",       false, C_FULL },
    { "EL30V2",      false, C_FULL },
    { "AC180P",      false, C_OUT | BT_C_CHARGE_MODE | BT_C_POWER_LIFT },
    { "AC2P",        false, C_OUT | BT_C_POWER_LIFT },
    { "AC60",        false, C_OUT | BT_C_POWER_LIFT },
    { "AC60P",       false, C_OUT | BT_C_POWER_LIFT },
    { "Handsfree 2", false, C_OUT },
};

const bt_device_t *bt_device_lookup(const char *name)
{
    if (!name || !name[0]) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(DEVICES) / sizeof(DEVICES[0]); i++) {
        size_t n = strlen(DEVICES[i].name);
        if (strncmp(name, DEVICES[i].name, n) != 0) {
            continue;
        }
        /* The tail must be digits (or empty): keeps "EL10" off "EL100V2"
         * and "AC60" off "AC60P". Every entry is tried, so ordering does
         * not matter. */
        size_t j = n;
        while (name[j] >= '0' && name[j] <= '9') {
            j++;
        }
        if (name[j] == '\0') {
            return &DEVICES[i];
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
    /* Elite-10 telemetry extras (full decode only). */
    static const bt_reg_read_t EL10_EXTRA[] = {
        { REG_TIME_REMAINING,    1 },
        { REG_AC_INPUT_VOLTAGE,  1 },
        { REG_AC_INPUT_CURRENT,  1 },
        { REG_AC_OUTPUT_VOLTAGE, 1 },
        { REG_DEVICE_SN,         4 },
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
    }
    /* Control registers: only the ones this model has, and only when the
     * user has enabled controls — so a monitoring-only setup keeps a
     * short, fast sweep. Each is its own single-register read. */
    if (with_controls && dev && dev->controls) {
        for (size_t i = 0; i < BT_CONTROL_COUNT && n < max; i++) {
            if (dev->controls & BT_CONTROLS[i].bit) {
                out[n++] = (bt_reg_read_t){ BT_CONTROLS[i].reg, 1 };
            }
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Writeable controls                                                  */
/* ------------------------------------------------------------------ */

const bt_control_t BT_CONTROLS[] = {
  { "ac_output",     REG_CTRL_AC,            BT_C_AC_OUT,      BT_KIND_BOOL,  NULL      },
  { "dc_output",     REG_CTRL_DC,            BT_C_DC_OUT,      BT_KIND_BOOL,  NULL      },
  { "eco_ac",        REG_CTRL_ECO_AC,        BT_C_ECO_AC,      BT_KIND_BOOL,  NULL      },
  { "eco_dc",        REG_CTRL_ECO_DC,        BT_C_ECO_DC,      BT_KIND_BOOL,  NULL      },
  { "eco_mode_ac",   REG_CTRL_ECO_MODE_AC,   BT_C_ECO_MODE_AC, BT_KIND_ENUM,  "1,2,3,4" },
  { "eco_mode_dc",   REG_CTRL_ECO_MODE_DC,   BT_C_ECO_MODE_DC, BT_KIND_ENUM,  "1,2,3,4" },
  { "charging_mode", REG_CTRL_CHARGING_MODE, BT_C_CHARGE_MODE, BT_KIND_ENUM,  "0,1,2,4" },
  { "power_lifting", REG_CTRL_POWER_LIFTING, BT_C_POWER_LIFT,  BT_KIND_BOOL,  NULL      },
  { "soc_min",       REG_CTRL_SOC_MIN,       BT_C_SOC_MIN,     BT_KIND_RANGE, "0,100"   },
  { "soc_max",       REG_CTRL_SOC_MAX,       BT_C_SOC_MAX,     BT_KIND_RANGE, "0,100"   },
  { "display_time",  REG_CTRL_DISPLAY_TIME,  BT_C_DISPLAY,     BT_KIND_ENUM,  "2,3,4,5" },
};
const size_t BT_CONTROL_COUNT = sizeof(BT_CONTROLS) / sizeof(BT_CONTROLS[0]);

/* Parse "a,b" (range) or "a,b,c,…" (enum list) into up to `n` ints. */
static size_t parse_ints(const char *s, int *out, size_t n)
{
    size_t k = 0;
    for (const char *p = s; p && *p && k < n; ) {
        int v = 0;
        bool digits = false;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); digits = true; }
        if (digits) out[k++] = v;
        while (*p == ',') p++;
    }
    return k;
}

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
    if (c->kind == BT_KIND_BOOL) {
        return value == 0 || value == 1;
    }
    int vals[8];
    size_t k = parse_ints(c->allowed, vals, 8);
    if (c->kind == BT_KIND_RANGE) {
        return k == 2 && value >= vals[0] && value <= vals[1];
    }
    for (size_t i = 0; i < k; i++) {
        if (vals[i] == value) {
            return true;
        }
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
    case REG_CTRL_SOC_MIN:        return st->soc_min;
    case REG_CTRL_SOC_MAX:        return st->soc_max;
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
    }

    /*
     * Writeable controls. Gated on the model's mask, not on `full`: the
     * control registers need no per-model scaling. A switch field is
     * strictly 0 or 1 and an enum strictly one of its values — anything
     * else means the register is not really there (bluetti-bt-lib returns
     * None), so it stays unknown and the UI does not show it.
     */
    if (dev->controls) {
        #define B(bit, R, F) \
            if ((dev->controls & (bit)) && \
                ((v = reg(start_addr, data, len, (R))) == 0 || v == 1)) { \
                st->F = v; matched++; }
        B(BT_C_AC_OUT,     REG_CTRL_AC,            ac_switch)
        B(BT_C_DC_OUT,     REG_CTRL_DC,            dc_switch)
        B(BT_C_ECO_AC,     REG_CTRL_ECO_AC,        eco_ac)
        B(BT_C_ECO_DC,     REG_CTRL_ECO_DC,        eco_dc)
        B(BT_C_POWER_LIFT, REG_CTRL_POWER_LIFTING, power_lifting)
        #undef B
        if ((dev->controls & BT_C_ECO_MODE_AC) &&
            (v = reg(start_addr, data, len, REG_CTRL_ECO_MODE_AC)) >= 1 && v <= 4) {
            st->eco_mode_ac = v; matched++;
        }
        if ((dev->controls & BT_C_ECO_MODE_DC) &&
            (v = reg(start_addr, data, len, REG_CTRL_ECO_MODE_DC)) >= 1 && v <= 4) {
            st->eco_mode_dc = v; matched++;
        }
        if ((dev->controls & BT_C_CHARGE_MODE) &&
            (v = reg(start_addr, data, len, REG_CTRL_CHARGING_MODE)) >= 0 &&
            (v == 0 || v == 1 || v == 2 || v == 4)) {
            st->charging_mode = v; matched++;
        }
        if ((dev->controls & BT_C_DISPLAY) &&
            (v = reg(start_addr, data, len, REG_CTRL_DISPLAY_TIME)) >= 2 && v <= 5) {
            st->display_time = v; matched++;
        }
        if ((dev->controls & BT_C_SOC_MIN) &&
            (v = reg(start_addr, data, len, REG_CTRL_SOC_MIN)) >= 0 && v <= 100) {
            st->soc_min = v; matched++;
        }
        if ((dev->controls & BT_C_SOC_MAX) &&
            (v = reg(start_addr, data, len, REG_CTRL_SOC_MAX)) >= 0 && v <= 100) {
            st->soc_max = v; matched++;
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
