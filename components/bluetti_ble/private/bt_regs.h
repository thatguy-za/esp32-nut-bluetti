#pragma once
/*
 * Elite 10 (EL10) register map and decode.
 *
 * Addresses come from Patrick762/bluetti-bt-lib PR #89 plus the
 * BaseDeviceV2 common fields. See docs/PROTOCOL.md. Registers are 16-bit
 * words, big-endian on the wire.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "bluetti_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Common (BaseDeviceV2) */
#define REG_BATTERY_SOC       102   /* uint, %                        */
#define REG_TIME_REMAINING    104   /* uint, minutes                  */
#define REG_DEVICE_TYPE       110   /* swapped string, 6 words        */
#define REG_DEVICE_SN         116   /* serial, 4 words               */

/* EL10 */
#define REG_DC_OUTPUT_POWER   140   /* uint, W                        */
#define REG_AC_OUTPUT_POWER   142   /* uint, W                        */
#define REG_DC_INPUT_POWER    144   /* uint, W                        */
#define REG_AC_INPUT_POWER    146   /* uint, W                        */
#define REG_AC_INPUT_VOLTAGE  1314  /* decimal, 1 dp                  */
#define REG_AC_INPUT_CURRENT  1315  /* decimal, 1 dp                  */
#define REG_AC_OUTPUT_VOLTAGE 1511  /* decimal, 1 dp                  */
#define REG_CTRL_AC           2011  /* bool                           */
#define REG_CTRL_DC           2012  /* bool                           */

/*
 * A model, and which of the optional registers it actually carries.
 *
 * Every V2 portable unit uses the same addresses; they differ only in which
 * fields exist. Reading an address a model does not declare is harmless
 * (upstream sweeps 0..20000 in blocks of ten on every V2 device), but
 * *interpreting* one is not — an absent register reads as zero, which would
 * publish a real-looking 0 V or a 0-minute runtime. So each optional field
 * is gated on the model.
 *
 * Generated from Patrick762/bluetti-bt-lib's device definitions.
 */
typedef struct {
    const char *name;             /* advertised name, digits follow      */
    bool has_runtime;             /* 104                                 */
    bool has_dc_input;            /* 144                                 */
    bool has_ac_in_volts;         /* 1314                                */
    bool has_ac_in_amps;          /* 1315                                */
    bool has_ac_out_volts;        /* 1511                                */
} bt_device_t;

extern const bt_device_t BT_DEVICES[];
extern const size_t      BT_DEVICE_COUNT;

/*
 * Match an advertised or self-reported name to a model. Names are a model
 * followed by digits, so the tail after the prefix must be digits — that is
 * what keeps "EL10" from claiming an "EL100V2". NULL if unrecognised.
 */
const bt_device_t *bt_device_lookup(const char *name);

/* The conservative subset every supported model shares: charge and the
 * three power readings. Used until the unit names itself, and for models
 * not in the table. */
extern const bt_device_t BT_DEVICE_GENERIC;

/*
 * The polling plan. Modbus caps a read at 125 registers, and the device
 * is happier with small contiguous blocks, so the map is covered by a
 * handful of reads rather than one sweep.
 */
typedef struct {
    uint16_t addr;
    uint16_t count;
} bt_reg_block_t;

extern const bt_reg_block_t BT_EL10_BLOCKS[];
extern const size_t         BT_EL10_BLOCK_COUNT;

/* Apply one register block to the state. Returns the number of fields
 * recognised, so the caller can tell a useful frame from a stray one. */
int bt_regs_apply(const bt_device_t *dev, uint16_t start_addr,
                  const uint8_t *data, size_t len, bluetti_state_t *st);

/* Whether a block is worth polling for this model. A NULL model means we
 * have not identified it yet, so everything is polled. */
bool bt_regs_block_wanted(const bt_device_t *dev, uint16_t addr);

#ifdef __cplusplus
}
#endif
