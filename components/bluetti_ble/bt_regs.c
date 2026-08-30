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

size_t bt_regs_plan(const bt_device_t *dev, bt_reg_read_t *out, size_t max)
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
    /* Elite-10 extras. */
    static const bt_reg_read_t EL10_EXTRA[] = {
        { REG_TIME_REMAINING,    1 },
        { REG_AC_INPUT_VOLTAGE,  1 },
        { REG_AC_INPUT_CURRENT,  1 },
        { REG_AC_OUTPUT_VOLTAGE, 1 },
        { REG_CTRL_AC,           1 },
        { REG_CTRL_DC,           1 },
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
    return n;
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
            st->ac_switch = v;
            matched++;
        }
        if ((v = reg(start_addr, data, len, REG_CTRL_DC)) == 0 || v == 1) {
            st->dc_switch = v;
            matched++;
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
