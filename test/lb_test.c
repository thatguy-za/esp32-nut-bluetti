/* The LB (low battery) decision. Mirrors the logic in main.c's
 * publish_nut_from_bluetti(): LB is raised when the charge drops to the
 * percentage threshold OR the remaining runtime drops to the runtime
 * threshold, because percentage alone is misleading under load. */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static bool is_low(int soc_pct, int low_pct, int minutes_remaining,
                   int runtime_low_s)
{
    bool low_charge = soc_pct <= low_pct;
    bool low_runtime = runtime_low_s > 0 && minutes_remaining >= 0 &&
                       minutes_remaining * 60 <= runtime_low_s;
    return low_charge || low_runtime;
}

static int fails;
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

int main(void)
{
    const int LOW_PCT = 20, LOW_S = 300;   /* 5 minutes */

    /* Charge-based trigger */
    OKF(!is_low(100, LOW_PCT, 600, LOW_S), "full charge, 10h left -> no LB");
    OKF(!is_low(21, LOW_PCT, 600, LOW_S),  "just above threshold -> no LB");
    OKF(is_low(20, LOW_PCT, 600, LOW_S),   "at the charge threshold -> LB");
    OKF(is_low(5, LOW_PCT, 600, LOW_S),    "well below -> LB");

    /* Runtime-based trigger: plenty of charge, but draining fast. This is
     * the case the percentage alone would miss. */
    OKF(is_low(80, LOW_PCT, 4, LOW_S),
        "80%% charge but 4 min left -> LB (heavy load)");
    OKF(is_low(80, LOW_PCT, 5, LOW_S),
        "80%% charge, exactly 5 min left -> LB");
    OKF(!is_low(80, LOW_PCT, 6, LOW_S),
        "80%% charge, 6 min left -> no LB");

    /* A light load keeps a low percentage out of LB on runtime alone,
     * but the charge trigger still fires. */
    OKF(is_low(15, LOW_PCT, 240, LOW_S),
        "15%% charge with 4h left -> LB (charge trigger)");

    /* Unknown runtime must not trigger anything. */
    OKF(!is_low(50, LOW_PCT, -1, LOW_S), "unknown runtime -> no LB");
    OKF(is_low(10, LOW_PCT, -1, LOW_S),  "unknown runtime, low charge -> LB");

    /* Runtime threshold disabled */
    OKF(!is_low(80, LOW_PCT, 1, 0), "runtime threshold 0 disables it");
    OKF(is_low(10, LOW_PCT, 1, 0),  "...but charge still triggers");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
