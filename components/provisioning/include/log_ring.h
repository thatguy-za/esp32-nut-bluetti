#pragma once
/*
 * Captures the ESP-IDF log stream into a small in-RAM ring buffer so the
 * web UI can tail it. Installs an esp_log_set_vprintf() hook that tees
 * every line to the original UART sink and to the ring.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start teeing logs into the ring. Safe to call once, early. */
void log_ring_init(void);

/* Copy up to `cap` bytes written after byte position `from` into `out`.
 * Returns the number of bytes copied and updates *next to the new
 * position to pass on the following call. If `from` is older than what
 * the ring still holds, output starts at the oldest retained byte and
 * *next reflects that jump. */
size_t log_ring_read(uint64_t from, char *out, size_t cap, uint64_t *next);

/* Total bytes ever written (the head cursor). */
uint64_t log_ring_head(void);

#ifdef __cplusplus
}
#endif
