#pragma once
/*
 * Minimal protobuf reader for the River 3 `DisplayPropertyUpload`
 * message (proto file `pr705`). We only extract the ~15 fields the NUT
 * bridge needs, so instead of a generated decoder this walks the wire
 * format and picks known field numbers.
 */

#include <stddef.h>
#include <stdint.h>
#include "ecoflow_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Apply a DisplayPropertyUpload payload to *st. Returns the number of
 * recognised fields (>0 => st updated and st->valid set). */
int ef_proto_apply_display(const uint8_t *pb, size_t len, ecoflow_state_t *st);

#ifdef __cplusplus
}
#endif
