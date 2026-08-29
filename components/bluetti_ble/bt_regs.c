#include "bt_regs.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bt_regs";

/* Contiguous blocks covering everything the decoder uses. */
const bt_reg_block_t BT_EL10_BLOCKS[] = {
    { 102,  16 },   /* SOC, time remaining, device type, serial */
    { 140,   8 },   /* the four power readings                  */
    { 1314,  2 },   /* AC input voltage + current               */
    { 1511,  1 },   /* AC output voltage                        */
    { 2011,  2 },   /* AC / DC output switches                  */
};
const size_t BT_EL10_BLOCK_COUNT =
    sizeof(BT_EL10_BLOCKS) / sizeof(BT_EL10_BLOCKS[0]);

/* Fetch one register out of a response block, or -1 if out of range. */
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
    /* Trim trailing spaces the device pads with. */
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\0')) {
        out[--o] = '\0';
    }
}

int bt_regs_apply(uint16_t start_addr, const uint8_t *data, size_t len,
                  bluetti_state_t *st)
{
    int matched = 0;
    int v;

    if ((v = reg(start_addr, data, len, REG_BATTERY_SOC)) >= 0 && v <= 100) {
        st->soc_pct = v;
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_TIME_REMAINING)) >= 0) {
        /* 0 shows up both when full and when the estimate is unavailable;
         * treat it as unknown rather than "no runtime left". */
        st->minutes_remaining = v > 0 ? v : BLUETTI_UNKNOWN_I;
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_DEVICE_TYPE)) >= 0) {
        swap_string(start_addr, data, len, REG_DEVICE_TYPE, 6,
                    st->model, sizeof(st->model));
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_DEVICE_SN)) >= 0) {
        /* The serial is four words, little-endian across the pair. */
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

    if ((v = reg(start_addr, data, len, REG_DC_OUTPUT_POWER)) >= 0) {
        st->output_watts = (float)v;   /* provisional; AC added below */
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_AC_OUTPUT_POWER)) >= 0) {
        st->ac_out_watts = (float)v;
        int dc = reg(start_addr, data, len, REG_DC_OUTPUT_POWER);
        st->output_watts = (float)v + (dc > 0 ? (float)dc : 0.0f);
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_DC_INPUT_POWER)) >= 0) {
        st->input_watts = (float)v;
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_AC_INPUT_POWER)) >= 0) {
        st->ac_in_watts = (float)v;
        int dc = reg(start_addr, data, len, REG_DC_INPUT_POWER);
        st->input_watts = (float)v + (dc > 0 ? (float)dc : 0.0f);
        /* No explicit mains flag in the map, so infer it from input. */
        st->ac_input_present = v > 0;
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_AC_INPUT_VOLTAGE)) >= 0) {
        /* Voltage corroborates the mains flag: a plugged-in but idle unit
         * can report 0 W while still showing line voltage. */
        st->ac_in_volts = (float)v / 10.0f;
        if (v > 0) {
            st->ac_input_present = true;
        }
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
    if ((v = reg(start_addr, data, len, REG_CTRL_AC)) >= 0) {
        st->ac_switch = v ? 1 : 0;
        matched++;
    }
    if ((v = reg(start_addr, data, len, REG_CTRL_DC)) >= 0) {
        st->dc_switch = v ? 1 : 0;
        matched++;
    }

    if (matched > 0) {
        /* Charging is inferred: mains in, and not full. */
        st->charging = st->ac_input_present && st->soc_pct < 100;
        /* battery_watts sign convention matches the NUT layer: >0 = out. */
        if (st->input_watts >= 0.0f && st->output_watts >= 0.0f) {
            st->battery_watts = st->output_watts - st->input_watts;
        }
        st->valid = true;
        st->updated_us = esp_timer_get_time();
    }
    ESP_LOGD(TAG, "block %u len %u -> %d fields", start_addr,
             (unsigned)len, matched);
    return matched;
}
