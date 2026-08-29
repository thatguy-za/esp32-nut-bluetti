/* Elite 10 register decode. Mirrors bt_regs.c's arithmetic against
 * synthetic Modbus blocks, so the address maths and the inferred fields
 * (mains present, charging) are pinned down before hardware exists.
 *
 * The addresses come from an unmerged PR; if they turn out wrong the
 * failure will be in the map, not in this arithmetic. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define REG_BATTERY_SOC      102
#define REG_TIME_REMAINING   104
#define REG_AC_OUTPUT_POWER  142
#define REG_AC_INPUT_POWER   146

static int fails;
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

/* Same accessor as bt_regs.c. */
static int reg(uint16_t start, const uint8_t *d, size_t len, uint16_t addr)
{
    if (addr < start) return -1;
    size_t i = (size_t)(addr - start) * 2;
    if (i + 1 >= len) return -1;
    return ((int)d[i] << 8) | d[i + 1];
}

static void put(uint8_t *d, uint16_t start, uint16_t addr, uint16_t val)
{
    size_t i = (size_t)(addr - start) * 2;
    d[i] = val >> 8;
    d[i + 1] = val & 0xFF;
}

/* The swapped-string decode used for the model name. */
static void swap_string(uint16_t start, const uint8_t *d, size_t len,
                        uint16_t addr, int words, char *out, size_t out_sz)
{
    size_t o = 0;
    for (int w = 0; w < words && o + 2 < out_sz; w++) {
        int v = reg(start, d, len, addr + w);
        if (v < 0) break;
        char lo = (char)(v & 0xFF), hi = (char)((v >> 8) & 0xFF);
        if (lo) out[o++] = lo;
        if (hi) out[o++] = hi;
    }
    out[o] = '\0';
    while (o > 0 && out[o - 1] == ' ') out[--o] = '\0';
}

int main(void)
{
    /* A 16-register block starting at 102. */
    const uint16_t start = 102;
    uint8_t d[32];
    memset(d, 0, sizeof(d));

    put(d, start, REG_BATTERY_SOC, 75);
    put(d, start, REG_TIME_REMAINING, 240);

    OKF(reg(start, d, sizeof(d), REG_BATTERY_SOC) == 75, "SOC reads 75");
    OKF(reg(start, d, sizeof(d), REG_TIME_REMAINING) == 240, "runtime reads 240 min");

    /* Out-of-block addresses must report absent, not garbage. */
    OKF(reg(start, d, sizeof(d), 101) == -1, "address below the block -> absent");
    OKF(reg(start, d, sizeof(d), 200) == -1, "address past the block -> absent");
    OKF(reg(start, d, sizeof(d), start + 15) >= 0, "last register in range");
    OKF(reg(start, d, sizeof(d), start + 16) == -1, "one past the end -> absent");

    /* Swapped strings: word 0x4C45 should read "EL", not "LE". */
    uint8_t s[12];
    memset(s, 0, sizeof(s));
    const uint16_t sstart = 110;
    put(s, sstart, 110, 0x4C45);   /* 'L','E' -> "EL" */
    put(s, sstart, 111, 0x3031);   /* '0','1' -> "10" */
    char model[24];
    swap_string(sstart, s, sizeof(s), 110, 6, model, sizeof(model));
    OKF(strncmp(model, "EL10", 4) == 0, "swapped string decodes to '%s'", model);

    /* Mains present is inferred from AC input power. */
    uint8_t p[16];
    memset(p, 0, sizeof(p));
    const uint16_t pstart = 140;
    put(p, pstart, REG_AC_OUTPUT_POWER, 45);
    put(p, pstart, REG_AC_INPUT_POWER, 0);
    bool ac = reg(pstart, p, sizeof(p), REG_AC_INPUT_POWER) > 0;
    OKF(!ac, "no AC input power -> on battery");
    OKF(reg(pstart, p, sizeof(p), REG_AC_OUTPUT_POWER) == 45, "AC output 45 W");

    put(p, pstart, REG_AC_INPUT_POWER, 60);
    ac = reg(pstart, p, sizeof(p), REG_AC_INPUT_POWER) > 0;
    OKF(ac, "AC input power present -> on line");

    /* Charging is inferred: mains in and not full. */
    int soc = 75;
    OKF(ac && soc < 100, "mains in at 75%% -> charging");
    soc = 100;
    OKF(!(ac && soc < 100), "mains in at 100%% -> not charging");

    /* A zero runtime means "unknown", not "no time left" — it reads zero
     * both when full and when the device has no estimate. */
    put(d, start, REG_TIME_REMAINING, 0);
    int rt = reg(start, d, sizeof(d), REG_TIME_REMAINING);
    OKF(rt == 0, "runtime register can read zero");
    OKF((rt > 0 ? rt : -1) == -1, "zero runtime maps to unknown, not 0 minutes");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
