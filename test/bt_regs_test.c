/* Exercises the real bt_regs.c: model identification, the per-field
 * polling plan, and the decode of individual Modbus field responses into
 * bluetti_state_t. Everything here mirrors what bluetti-bt-lib does for
 * the Elite 10; it cannot prove the register addresses are right (that
 * needs hardware) but it pins the arithmetic and the field gating. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "bt_regs.h"

static int fails;
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

/* A one-register (or n-register) big-endian response body. */
static size_t words_be(uint8_t *out, const uint16_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = w[i] >> 8;
        out[i * 2 + 1] = w[i] & 0xFF;
    }
    return n * 2;
}

static bluetti_state_t fresh(void)
{
    bluetti_state_t st;
    memset(&st, 0, sizeof st);
    st.soc_pct           = BLUETTI_UNKNOWN_I;
    st.minutes_remaining = BLUETTI_UNKNOWN_I;
    st.input_watts = st.output_watts = st.battery_watts = BLUETTI_UNKNOWN_F;
    st.ac_in_watts = st.ac_out_watts = BLUETTI_UNKNOWN_F;
    st.dc_in_watts = st.dc_out_watts = BLUETTI_UNKNOWN_F;
    st.ac_in_volts = st.ac_in_amps = st.ac_out_volts = BLUETTI_UNKNOWN_F;
    st.ac_switch = st.dc_switch = BLUETTI_UNKNOWN_I;
    return st;
}

/* Feed one field response at `addr`. */
static int one(const bt_device_t *dev, bluetti_state_t *st,
               uint16_t addr, uint16_t value)
{
    uint8_t buf[2];
    uint16_t w = value;
    words_be(buf, &w, 1);
    return bt_regs_apply(dev, addr, buf, 2, st);
}

int main(void)
{
    /* ---- model identification ---- */
    const bt_device_t *el = bt_device_lookup("EL102411000123");
    OKF(el == &BT_DEVICE_EL10, "EL10 with a serial -> full decode");
    OKF(bt_device_lookup("EL100V22411") == &BT_DEVICE_EL10,
        "EL100V2 -> full decode (identical field list)");
    OKF(bt_device_lookup("EL100V22411") == bt_device_lookup("EL102411"),
        "EL10 and EL100V2 resolve to the same descriptor");
    OKF(bt_device_lookup("AC70123") == NULL, "AC70 is not claimed -> generic");
    OKF(bt_device_lookup("AC200M9") == NULL, "a V1 model -> generic");
    OKF(bt_device_lookup("EL10ABC") == NULL, "non-digit tail does not match");
    OKF(bt_device_lookup("") == NULL && bt_device_lookup(NULL) == NULL,
        "empty / NULL name -> generic");
    OKF(!BT_DEVICE_GENERIC.full && BT_DEVICE_EL10.full,
        "generic is not full, EL10 is");

    /* ---- polling plan ---- */
    bt_reg_read_t plan[BT_REG_PLAN_MAX];
    size_t gn = bt_regs_plan(&BT_DEVICE_GENERIC, plan, BT_REG_PLAN_MAX);
    OKF(gn >= 5 && gn <= 8, "generic plan is the small common set (%zu)", gn);
    OKF(plan[0].addr == REG_BATTERY_SOC, "charge is polled first");
    bool has_type = false;
    for (size_t i = 0; i < gn; i++) if (plan[i].addr == REG_DEVICE_TYPE) has_type = true;
    OKF(has_type, "generic plan still reads register 110 to identify the model");

    size_t en = bt_regs_plan(&BT_DEVICE_EL10, plan, BT_REG_PLAN_MAX);
    OKF(en > gn, "EL10 plan has more fields than generic (%zu > %zu)", en, gn);
    bool has_ctrl = false, has_sn = false, has_rt = false;
    for (size_t i = 0; i < en; i++) {
        if (plan[i].addr == REG_CTRL_AC)     has_ctrl = true;
        if (plan[i].addr == REG_DEVICE_SN)   has_sn = true;
        if (plan[i].addr == REG_TIME_REMAINING) has_rt = true;
    }
    OKF(has_ctrl && has_sn && has_rt, "EL10 plan covers switches, serial, runtime");
    OKF(en <= BT_REG_PLAN_MAX, "plan fits the buffer");

    /* ---- decode: charge ---- */
    bluetti_state_t st = fresh();
    OKF(one(&BT_DEVICE_EL10, &st, REG_BATTERY_SOC, 75) == 1 && st.soc_pct == 75,
        "SOC 75 decodes");
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_BATTERY_SOC, 200);
    OKF(st.soc_pct == BLUETTI_UNKNOWN_I, "SOC 200 is out of range -> ignored");
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_BATTERY_SOC, 0xFFFF);
    OKF(st.soc_pct == BLUETTI_UNKNOWN_I, "SOC 0xFFFF (sentinel) -> ignored");

    /* ---- decode: power sums across separate reads ---- */
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_AC_OUTPUT_POWER, 45);
    one(&BT_DEVICE_EL10, &st, REG_DC_OUTPUT_POWER, 5);
    OKF(st.output_watts > 49.5f && st.output_watts < 50.5f,
        "output = AC(45) + DC(5) from two separate reads");
    one(&BT_DEVICE_EL10, &st, REG_AC_INPUT_POWER, 60);
    OKF(st.input_watts > 59.5f && st.input_watts < 60.5f, "input = AC(60)");
    OKF(st.ac_input_present, "AC input power > 0 -> on line");
    OKF(st.battery_watts > -10.5f && st.battery_watts < -9.5f,
        "battery flow = out(50) - in(60) = -10 (charging)");

    /* ---- decode: mains from line voltage alone ---- */
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_AC_INPUT_POWER, 0);
    OKF(!st.ac_input_present, "0 W AC input, no voltage yet -> on battery");
    one(&BT_DEVICE_EL10, &st, REG_AC_INPUT_VOLTAGE, 2301);
    OKF(st.ac_in_volts > 230.0f && st.ac_in_volts < 230.2f,
        "AC input voltage 2301 -> 230.1 V");
    OKF(st.ac_input_present, "line voltage present -> back on line even at 0 W");

    /* ---- decode: charging inference ---- */
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_BATTERY_SOC, 80);
    one(&BT_DEVICE_EL10, &st, REG_AC_INPUT_POWER, 100);
    OKF(st.charging, "mains in at 80pct then charging");
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_BATTERY_SOC, 100);
    one(&BT_DEVICE_EL10, &st, REG_AC_INPUT_POWER, 100);
    OKF(!st.charging, "mains in at 100pct then not charging");

    /* ---- decode: runtime ---- */
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_TIME_REMAINING, 240);
    OKF(st.minutes_remaining == 240, "runtime 240 min");
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_TIME_REMAINING, 0);
    OKF(st.minutes_remaining == BLUETTI_UNKNOWN_I,
        "runtime 0 -> unknown, not 'no time left'");
    /* generic model must not decode runtime at all */
    st = fresh();
    one(&BT_DEVICE_GENERIC, &st, REG_TIME_REMAINING, 240);
    OKF(st.minutes_remaining == BLUETTI_UNKNOWN_I,
        "generic model does not decode runtime");

    /* ---- decode: switches are strictly 0/1 ---- */
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_CTRL_AC, 1);
    OKF(st.ac_switch == 1, "CTRL_AC 1 -> outlet on");
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_CTRL_AC, 0);
    OKF(st.ac_switch == 0, "CTRL_AC 0 -> outlet off");
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_CTRL_AC, 2);
    OKF(st.ac_switch == BLUETTI_UNKNOWN_I,
        "CTRL_AC 2 is neither on nor off -> absent (matches lib's None)");
    st = fresh();
    one(&BT_DEVICE_EL10, &st, REG_CTRL_AC, 0xFFFF);
    OKF(st.ac_switch == BLUETTI_UNKNOWN_I, "CTRL_AC 0xFFFF -> absent");

    /* ---- decode: serial across four words, least-significant first ---- */
    st = fresh();
    {
        uint8_t buf[8];
        uint16_t w[4] = { 0x0001, 0x0000, 0x0000, 0x0000 };  /* = 1 */
        words_be(buf, w, 4);
        bt_regs_apply(&BT_DEVICE_EL10, REG_DEVICE_SN, buf, 8, &st);
        OKF(strcmp(st.serial, "1") == 0, "serial words LE: {1,0,0,0} -> '1'");

        uint16_t w2[4] = { 0x0000, 0x0001, 0x0000, 0x0000 };  /* = 65536 */
        words_be(buf, w2, 4);
        st = fresh();
        bt_regs_apply(&BT_DEVICE_EL10, REG_DEVICE_SN, buf, 8, &st);
        OKF(strcmp(st.serial, "65536") == 0, "serial {0,1,0,0} -> '65536'");
    }

    /* ---- decode: swapped model string ---- */
    st = fresh();
    {
        uint8_t buf[12];
        /* "EL10" as byte-swapped words: 'L','E' then '0','1' */
        uint16_t w[6] = { ('L' << 8) | 'E', ('0' << 8) | '1', 0, 0, 0, 0 };
        words_be(buf, w, 6);
        bt_regs_apply(&BT_DEVICE_EL10, REG_DEVICE_TYPE, buf, 12, &st);
        OKF(strcmp(st.model, "EL10") == 0, "swapped string -> '%s'", st.model);
    }

    /* ---- a stray field for the wrong address changes nothing ---- */
    st = fresh();
    OKF(one(&BT_DEVICE_EL10, &st, 9999, 1234) == 0 && !st.valid,
        "an unknown address is ignored, state stays invalid");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
