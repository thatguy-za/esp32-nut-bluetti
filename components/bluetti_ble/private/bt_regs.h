#pragma once
/*
 * Elite 10 (EL10) register map and decode.
 *
 * Addresses come from Patrick762/bluetti-bt-lib PR #89 plus the
 * BaseDeviceV2 common fields. See docs/PROTOCOL.md. Registers are 16-bit
 * words, big-endian on the wire.
 */

#include <stddef.h>
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
int bt_regs_apply(uint16_t start_addr, const uint8_t *data, size_t len,
                  bluetti_state_t *st);

#ifdef __cplusplus
}
#endif
