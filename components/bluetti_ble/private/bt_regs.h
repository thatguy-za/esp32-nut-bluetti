#pragma once
/*
 * BLUETTI V2 register map and decode.
 *
 * Addresses and scaling are taken field-for-field from
 * Patrick762/bluetti-bt-lib (the library behind the Home Assistant
 * integration) — see docs/PROTOCOL.md. Registers are 16-bit words,
 * big-endian on the wire.
 *
 * Only the Elite 10 (and the byte-identical EL100V2) are decoded in full.
 * bluetti-bt-lib reads each field with its own Modbus transaction and its
 * own per-model scaling; other V2 models differ in both which fields
 * exist and how a few are scaled (runtime and AC-input voltage in
 * particular). Rather than reproduce every model's quirks unverified, an
 * unrecognised unit gets the four fields that are identical across every
 * V2 model — charge and the AC/DC power readings — and nothing else.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "bluetti_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Common to every BaseDeviceV2 */
#define REG_BATTERY_SOC       102   /* uint, %                          */
#define REG_TIME_REMAINING    104   /* uint, minutes (EL10 scaling)     */
#define REG_DEVICE_TYPE       110   /* swapped string, 6 words          */
#define REG_DEVICE_SN         116   /* serial, 4 words                  */
#define REG_DC_OUTPUT_POWER   140   /* uint, W                          */
#define REG_AC_OUTPUT_POWER   142   /* uint, W                          */
#define REG_DC_INPUT_POWER    144   /* uint, W                          */
#define REG_AC_INPUT_POWER    146   /* uint, W                          */

/* EL10 / EL100V2 */
#define REG_AC_INPUT_VOLTAGE  1314  /* decimal, ÷10                     */
#define REG_AC_INPUT_CURRENT  1315  /* decimal, ÷10                     */
#define REG_AC_OUTPUT_VOLTAGE 1511  /* decimal, ÷10                     */

/* EL10 writeable controls (Modbus 0x06). Values documented in bt_regs.c. */
#define REG_CTRL_AC             2011  /* bool  — AC output               */
#define REG_CTRL_DC             2012  /* bool  — DC output               */
#define REG_CTRL_ECO_DC        2014  /* bool  — DC ECO mode             */
#define REG_CTRL_ECO_MODE_DC   2015  /* enum  — DC ECO timeout, 1..4 h  */
#define REG_CTRL_ECO_AC        2017  /* bool  — AC ECO mode             */
#define REG_CTRL_ECO_MODE_AC   2018  /* enum  — AC ECO timeout, 1..4 h  */
#define REG_CTRL_CHARGING_MODE 2020  /* enum  — 0 std/1 silent/2 turbo/4 custom */
#define REG_CTRL_POWER_LIFTING 2021  /* bool  — power lifting           */
#define REG_CTRL_SOC_MIN       2022  /* uint  — discharge floor, %      */
#define REG_CTRL_SOC_MAX       2023  /* uint  — charge ceiling, %       */
#define REG_CTRL_DISPLAY_TIME  2067  /* enum  — 2/3/4/5 (30s/1m/5m/never) */

/*
 * Which writeable controls a model exposes. The control registers
 * (2011-2067) and their meanings are identical across every V2 model that
 * implements them — a bool is 0/1, the enums are shared classes — so this
 * is a per-model *presence* mask, taken from bluetti-bt-lib's SwitchField
 * / SelectField / UIntField definitions, not a decode difference. (That is
 * why controls are not limited to the Elite 10 the way full telemetry is:
 * only the *readings* need per-model scaling.)
 */
enum {
    BT_C_AC_OUT      = 1u << 0,
    BT_C_DC_OUT      = 1u << 1,
    BT_C_ECO_AC      = 1u << 2,
    BT_C_ECO_DC      = 1u << 3,
    BT_C_ECO_MODE_AC = 1u << 4,
    BT_C_ECO_MODE_DC = 1u << 5,
    BT_C_CHARGE_MODE = 1u << 6,
    BT_C_POWER_LIFT  = 1u << 7,
    BT_C_DISPLAY     = 1u << 8,
    BT_C_SOC_MIN     = 1u << 9,
    BT_C_SOC_MAX     = 1u << 10,
};

/*
 * A recognised model. `full` marks the Elite-10 family (full telemetry
 * decode); everything else falls back to charge + power. `controls` is
 * the mask above — independent of `full`.
 */
typedef struct {
    const char *name;      /* advertised name; digits follow */
    bool        full;
    uint16_t    controls;
} bt_device_t;

extern const bt_device_t BT_DEVICE_GENERIC;   /* unrecognised V2 unit */

/*
 * Match an advertised or self-reported name to a model. A name is a model
 * followed by digits, so the tail after the prefix must be digits — that
 * keeps "EL10" from claiming an "EL100V2" and "AC60" from claiming an
 * "AC60P". Returns NULL for an unrecognised name (the caller then uses
 * BT_DEVICE_GENERIC, which has no controls).
 */
const bt_device_t *bt_device_lookup(const char *name);

/*
 * The polling plan: one entry per field, each its own Modbus read, the
 * way bluetti-bt-lib polls. Reading fields individually rather than in
 * ranges avoids any question of whether the unit answers a range that
 * spans registers it does not implement.
 */
typedef struct {
    uint16_t addr;
    uint8_t  words;
} bt_reg_read_t;

#define BT_REG_PLAN_MAX 24

/*
 * Fill `out` (capacity `max`) with the reads to poll for `dev` (NULL =
 * generic). UPS-critical fields come first. With `with_controls`, an EL10
 * plan also reads the writeable control registers so the UI can show
 * their current state. Returns the count.
 */
#define BT_CTRL_PLAN_MAX 12

size_t bt_regs_plan(const bt_device_t *dev, bool with_controls,
                    bt_reg_read_t *out, size_t max);

/*
 * Apply one field's response to the state. `start_addr`/`data`/`len` are a
 * single Modbus read's register address and data bytes. Recomputes the
 * derived values (mains present, charging, battery flow) from whatever
 * has accumulated in `st` so far. Returns the number of raw fields this
 * call recognised.
 */
int bt_regs_apply(const bt_device_t *dev, uint16_t start_addr,
                  const uint8_t *data, size_t len, bluetti_state_t *st);

/*
 * A writeable control, addressed by a stable API field name so the web
 * layer never handles a raw register.
 */
typedef enum {
    BT_KIND_BOOL,    /* 0 / 1                                             */
    BT_KIND_ENUM,    /* one of `allowed`, a comma list of ints            */
    BT_KIND_RANGE,   /* an integer in [lo, hi]; `allowed` is "lo,hi"      */
} bt_ctrl_kind_t;

typedef struct {
    const char    *field;   /* "ac_output", "charging_mode", "soc_min"…   */
    uint16_t       reg;
    uint16_t       bit;     /* BT_C_* — which models have it              */
    bt_ctrl_kind_t kind;
    const char    *allowed; /* enum: "0,1,2,4"; range: "0,100"; bool: NULL */
} bt_control_t;

extern const bt_control_t BT_CONTROLS[];
extern const size_t       BT_CONTROL_COUNT;

const bt_control_t *bt_control_lookup(const char *field);
bool bt_control_valid(const bt_control_t *c, int value);

/* Current value of a control in `st`, or -1 if unknown. */
int bt_control_current(const bt_control_t *c, const bluetti_state_t *st);

#ifdef __cplusplus
}
#endif
