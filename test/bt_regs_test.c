/* Exercises the real bt_regs.c: model identification and its control
 * masks, the per-field polling plan, decode of individual Modbus field
 * responses into bluetti_state_t, and the control table (name -> register,
 * value validation, current-value mapping). It cannot prove the register
 * addresses are right — that needs hardware — but it pins the arithmetic,
 * the per-model gating, and which model gets which controls. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "bt_regs.h"

static int fails;
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

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
    st.eco_ac = st.eco_dc = st.eco_mode_ac = st.eco_mode_dc = BLUETTI_UNKNOWN_I;
    st.charging_mode = st.power_lifting = st.display_time = BLUETTI_UNKNOWN_I;
    st.soc_min = st.soc_max = BLUETTI_UNKNOWN_I;
    return st;
}

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
    const bt_device_t *EL10   = bt_device_lookup("EL102411");
    const bt_device_t *EL100  = bt_device_lookup("EL100V299");
    const bt_device_t *AC70   = bt_device_lookup("AC70123");
    const bt_device_t *AC60   = bt_device_lookup("AC60999");
    const bt_device_t *AC60P  = bt_device_lookup("AC60P5");
    const bt_device_t *GEN    = &BT_DEVICE_GENERIC;

    /* ---- model identification ---- */
    OKF(EL10 && EL10->full, "EL10 -> recognised, full telemetry");
    OKF(EL100 && EL100->full, "EL100V2 -> recognised, full telemetry");
    OKF(EL10 != EL100, "EL10 and EL100V2 are distinct entries");
    OKF(AC70 && !AC70->full, "AC70 -> recognised, basic telemetry");
    OKF(AC60 && !AC60->full && AC60P && AC60P != AC60,
        "AC60 and AC60P are told apart by the digit-tail rule");
    OKF(bt_device_lookup("AC200M7") == NULL, "a V1 model -> unrecognised");
    OKF(bt_device_lookup("EL10ABC") == NULL, "non-digit tail -> unrecognised");
    OKF(bt_device_lookup("") == NULL && bt_device_lookup(NULL) == NULL,
        "empty / NULL -> unrecognised");

    /* ---- control masks ---- */
    OKF(GEN->controls == 0, "an unrecognised unit has no controls");
    OKF((EL10->controls & BT_C_AC_OUT) && (EL10->controls & BT_C_DC_OUT) &&
        (EL10->controls & BT_C_CHARGE_MODE) && (EL10->controls & BT_C_DISPLAY),
        "EL10 has the full control set incl. screen timeout");
    OKF((EL100->controls & BT_C_SOC_MIN) && (EL100->controls & BT_C_SOC_MAX),
        "EL100V2 has the SOC min/max controls");
    OKF((EL10->controls & BT_C_SOC_MIN) && (EL10->controls & BT_C_SOC_MAX),
        "EL10 also polls SOC min/max (unconfirmed, self-selecting)");
    OKF((AC70->controls & BT_C_ECO_AC) && (AC70->controls & BT_C_CHARGE_MODE) &&
        !(AC70->controls & BT_C_DISPLAY) && !(AC70->controls & BT_C_SOC_MAX),
        "AC70 has ECO + charging mode but not screen timeout or SOC");
    OKF((AC60->controls & BT_C_AC_OUT) && (AC60->controls & BT_C_POWER_LIFT) &&
        !(AC60->controls & BT_C_ECO_AC) && !(AC60->controls & BT_C_CHARGE_MODE),
        "AC60 has only output switches and power lifting");

    /* ---- polling plan ---- */
    bt_reg_read_t plan[BT_REG_PLAN_MAX];
    size_t gn = bt_regs_plan(GEN, true, plan, BT_REG_PLAN_MAX);
    OKF(gn >= 5 && gn <= 8, "generic plan is the small common set (%zu)", gn);
    OKF(plan[0].addr == REG_BATTERY_SOC, "charge is polled first");
    bool gt = false;
    for (size_t i = 0; i < gn; i++) if (plan[i].addr == REG_DEVICE_TYPE) gt = true;
    OKF(gt, "generic plan reads register 110 to identify the model");

    size_t e0 = bt_regs_plan(EL10, false, plan, BT_REG_PLAN_MAX);
    size_t e1 = bt_regs_plan(EL10, true,  plan, BT_REG_PLAN_MAX);
    OKF(e1 > e0, "controls add reads to the EL10 plan (%zu -> %zu)", e0, e1);
    OKF(e1 <= BT_REG_PLAN_MAX, "the EL10-with-controls plan fits the buffer");
    bool has_cm = false, has_soc = false;
    for (size_t i = 0; i < e1; i++) {
        if (plan[i].addr == REG_CTRL_CHARGING_MODE) has_cm = true;
        if (plan[i].addr == REG_CTRL_SOC_MAX)       has_soc = true;
    }
    OKF(has_cm && has_soc, "the EL10-with-controls plan covers charging mode and SOC max");

    size_t a1 = bt_regs_plan(AC70, true, plan, BT_REG_PLAN_MAX);
    bool ac70_disp = false, ac70_soc = false;
    for (size_t i = 0; i < a1; i++) {
        if (plan[i].addr == REG_CTRL_DISPLAY_TIME) ac70_disp = true;
        if (plan[i].addr == REG_CTRL_SOC_MAX)      ac70_soc = true;
    }
    OKF(!ac70_disp && !ac70_soc,
        "the AC70 plan omits controls it does not have (screen timeout, SOC)");
    OKF(bt_regs_plan(GEN, true, plan, BT_REG_PLAN_MAX) == gn,
        "an unrecognised unit gets no control reads even when asked");

    /* ---- control table ---- */
    OKF(bt_control_lookup("ac_output")->reg == REG_CTRL_AC, "ac_output -> 2011");
    OKF(bt_control_lookup("soc_max")->reg == REG_CTRL_SOC_MAX, "soc_max -> 2023");
    OKF(bt_control_lookup("soc_min")->reg == REG_CTRL_SOC_MIN, "soc_min -> 2022");
    OKF(bt_control_lookup("nope") == NULL, "unknown control -> NULL");

    const bt_control_t *ac = bt_control_lookup("ac_output");
    OKF(bt_control_valid(ac, 0) && bt_control_valid(ac, 1) && !bt_control_valid(ac, 2),
        "a bool control accepts only 0 or 1");
    const bt_control_t *cm = bt_control_lookup("charging_mode");
    OKF(bt_control_valid(cm, 0) && bt_control_valid(cm, 4) && !bt_control_valid(cm, 3),
        "charging_mode allows 0,1,2,4 but not 3");
    const bt_control_t *sm = bt_control_lookup("soc_max");
    OKF(bt_control_valid(sm, 0) && bt_control_valid(sm, 100) &&
        !bt_control_valid(sm, 101) && !bt_control_valid(sm, -1),
        "a range control accepts 0..100 only");
    const bt_control_t *em = bt_control_lookup("eco_mode_ac");
    OKF(bt_control_valid(em, 1) && bt_control_valid(em, 4) &&
        !bt_control_valid(em, 0) && !bt_control_valid(em, 5),
        "eco timeout allows 1..4 only");

    /* ---- decode: charge ---- */
    bluetti_state_t st = fresh();
    OKF(one(EL10, &st, REG_BATTERY_SOC, 75) == 1 && st.soc_pct == 75, "SOC 75 decodes");
    st = fresh();
    one(EL10, &st, REG_BATTERY_SOC, 200);
    OKF(st.soc_pct == BLUETTI_UNKNOWN_I, "SOC 200 out of range -> ignored");

    /* ---- decode: power summed across separate reads ---- */
    st = fresh();
    one(EL10, &st, REG_AC_OUTPUT_POWER, 45);
    one(EL10, &st, REG_DC_OUTPUT_POWER, 5);
    OKF(st.output_watts > 49.5f && st.output_watts < 50.5f,
        "output = AC(45) + DC(5) from two reads");
    one(EL10, &st, REG_AC_INPUT_POWER, 60);
    OKF(st.ac_input_present && st.battery_watts > -10.5f && st.battery_watts < -9.5f,
        "on line, battery flow = 50 - 60 = -10 (charging)");

    /* ---- decode: runtime, EL10 only ---- */
    st = fresh();
    one(EL10, &st, REG_TIME_REMAINING, 240);
    OKF(st.minutes_remaining == 240, "EL10 decodes runtime");
    st = fresh();
    one(AC70, &st, REG_TIME_REMAINING, 240);
    OKF(st.minutes_remaining == BLUETTI_UNKNOWN_I,
        "AC70 (basic telemetry) does not decode runtime");

    /* ---- decode: controls, gated by the model mask ---- */
    st = fresh();
    one(EL10, &st, REG_CTRL_CHARGING_MODE, 2);
    OKF(st.charging_mode == 2 &&
        bt_control_current(bt_control_lookup("charging_mode"), &st) == 2,
        "EL10 decodes charging mode and reads it back");
    st = fresh();
    one(AC60, &st, REG_CTRL_CHARGING_MODE, 2);
    OKF(st.charging_mode == BLUETTI_UNKNOWN_I,
        "AC60 does not decode charging mode (not in its mask)");
    st = fresh();
    one(EL10, &st, REG_CTRL_CHARGING_MODE, 3);
    OKF(st.charging_mode == BLUETTI_UNKNOWN_I, "charging mode 3 is invalid -> absent");

    st = fresh();
    one(EL10, &st, REG_CTRL_AC, 2);
    OKF(st.ac_switch == BLUETTI_UNKNOWN_I, "switch value 2 -> absent (lib's None)");
    st = fresh();
    one(EL10, &st, REG_CTRL_AC, 1);
    OKF(st.ac_switch == 1, "switch value 1 -> on");

    /* ---- decode: SOC min/max ---- */
    st = fresh();
    one(EL10, &st, REG_CTRL_SOC_MIN, 15);
    one(EL10, &st, REG_CTRL_SOC_MAX, 90);
    OKF(st.soc_min == 15 && st.soc_max == 90, "SOC floor 15 / ceiling 90 decode");
    OKF(bt_control_current(bt_control_lookup("soc_min"), &st) == 15 &&
        bt_control_current(bt_control_lookup("soc_max"), &st) == 90,
        "bt_control_current maps soc_min/soc_max");
    st = fresh();
    one(EL10, &st, REG_CTRL_SOC_MAX, 250);
    OKF(st.soc_max == BLUETTI_UNKNOWN_I, "SOC max 250 out of range -> ignored");
    st = fresh();
    one(AC70, &st, REG_CTRL_SOC_MAX, 90);
    OKF(st.soc_max == BLUETTI_UNKNOWN_I, "AC70 has no SOC max in its mask");

    /* ---- decode: serial, least-significant word first ---- */
    st = fresh();
    {
        uint8_t buf[8];
        uint16_t w[4] = { 0x0000, 0x0001, 0x0000, 0x0000 };  /* = 65536 */
        words_be(buf, w, 4);
        bt_regs_apply(EL10, REG_DEVICE_SN, buf, 8, &st);
        OKF(strcmp(st.serial, "65536") == 0, "serial {0,1,0,0} -> '65536'");
    }

    /* ---- decode: swapped model string ---- */
    st = fresh();
    {
        uint8_t buf[12];
        uint16_t w[6] = { ('L' << 8) | 'E', ('0' << 8) | '1', 0, 0, 0, 0 };
        words_be(buf, w, 6);
        bt_regs_apply(EL10, REG_DEVICE_TYPE, buf, 12, &st);
        OKF(strcmp(st.model, "EL10") == 0, "swapped string -> '%s'", st.model);
    }

    /* ---- a stray field for an unknown address changes nothing ---- */
    st = fresh();
    OKF(one(EL10, &st, 9999, 1234) == 0 && !st.valid,
        "an unknown address is ignored, state stays invalid");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
