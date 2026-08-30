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
#define REG_CTRL_AC           2011  /* bool (0/1 only)                  */
#define REG_CTRL_DC           2012  /* bool (0/1 only)                  */

/*
 * A recognised model. `full` distinguishes the Elite-10 family (decode
 * everything) from the generic fallback (charge + power only).
 */
typedef struct {
    const char *name;   /* advertised name; digits follow */
    bool        full;
} bt_device_t;

extern const bt_device_t BT_DEVICE_EL10;      /* EL10 and EL100V2 */
extern const bt_device_t BT_DEVICE_GENERIC;   /* any other V2 unit */

/*
 * Match an advertised or self-reported name to a model. A name is a model
 * followed by digits, so the tail after the prefix must be digits — that
 * is what keeps "EL10" from claiming an "EL100V2". Returns &BT_DEVICE_EL10
 * for the Elite-10 family, NULL for anything else (the caller then uses
 * BT_DEVICE_GENERIC).
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

#define BT_REG_PLAN_MAX 16

/* Fill `out` (capacity `max`) with the reads to poll for `dev` (NULL =
 * generic). UPS-critical fields come first. Returns the count. */
size_t bt_regs_plan(const bt_device_t *dev, bt_reg_read_t *out, size_t max);

/*
 * Apply one field's response to the state. `start_addr`/`data`/`len` are a
 * single Modbus read's register address and data bytes. Recomputes the
 * derived values (mains present, charging, battery flow) from whatever
 * has accumulated in `st` so far. Returns the number of raw fields this
 * call recognised.
 */
int bt_regs_apply(const bt_device_t *dev, uint16_t start_addr,
                  const uint8_t *data, size_t len, bluetti_state_t *st);

#ifdef __cplusplus
}
#endif
