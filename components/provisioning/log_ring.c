#include "log_ring.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#define RING_SZ 12288   /* bytes retained for the web log tail */

static struct {
    char        buf[RING_SZ];
    uint64_t    head;                 /* total bytes ever written */
    portMUX_TYPE mux;
    vprintf_like_t prev;
    bool        inited;
} r = { .mux = portMUX_INITIALIZER_UNLOCKED };

static void ring_write(const char *data, size_t len)
{
    if (len == 0) {
        return;
    }
    if (len > RING_SZ) {              /* keep only the tail of a huge line */
        data += (len - RING_SZ);
        len = RING_SZ;
    }
    portENTER_CRITICAL(&r.mux);
    size_t off = (size_t)(r.head % RING_SZ);
    size_t first = RING_SZ - off;
    if (first > len) {
        first = len;
    }
    memcpy(&r.buf[off], data, first);
    if (len > first) {
        memcpy(&r.buf[0], data + first, len - first);
    }
    r.head += len;
    portEXIT_CRITICAL(&r.mux);
}

/* Drop ANSI CSI sequences (ESC '[' ... final byte 0x40-0x7E) in place.
 * The serial console keeps its colour (CONFIG_LOG_COLORS); the web tail
 * renders in a <pre>, where the raw escapes would show as "[0;32m" litter
 * around every line. Returns the new length. */
static size_t strip_ansi(char *s, size_t len)
{
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\033' && i + 1 < len && s[i + 1] == '[') {
            i += 2;
            while (i < len && (s[i] < 0x40 || s[i] > 0x7E)) {
                i++;
            }
            continue;   /* also drops the final byte */
        }
        s[w++] = s[i];
    }
    return w;
}

static int ring_vprintf(const char *fmt, va_list ap)
{
    char line[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        ring_write(line, strip_ansi(line, len));
    }
    return r.prev ? r.prev(fmt, ap) : vprintf(fmt, ap);
}

void log_ring_init(void)
{
    if (r.inited) {
        return;
    }
    r.inited = true;
    r.prev = esp_log_set_vprintf(ring_vprintf);
}

uint64_t log_ring_head(void)
{
    return r.head;
}

size_t log_ring_read(uint64_t from, char *out, size_t cap, uint64_t *next)
{
    portENTER_CRITICAL(&r.mux);
    uint64_t head = r.head;
    uint64_t oldest = head > RING_SZ ? head - RING_SZ : 0;
    if (from < oldest) {
        from = oldest;
    }
    if (from > head) {
        from = head;
    }
    size_t avail = (size_t)(head - from);
    if (avail > cap) {
        avail = cap;
        from = head - avail;
    }
    size_t off = (size_t)(from % RING_SZ);
    size_t first = RING_SZ - off;
    if (first > avail) {
        first = avail;
    }
    memcpy(out, &r.buf[off], first);
    if (avail > first) {
        memcpy(out + first, &r.buf[0], avail - first);
    }
    *next = from + avail;
    portEXIT_CRITICAL(&r.mux);
    return avail;
}
