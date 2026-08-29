/* The dotted-quad validator that guards static addressing. A bad value
 * accepted here strands the device on an unreachable address, so this
 * leans on rejecting the lenient forms esp_ip4addr_aton() would take. */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

/* Kept byte-identical to valid_ipv4() in provisioning.c. */
static bool valid_ipv4(const char *s)
{
    if (!s || !*s) {
        return false;
    }
    int parts = 0;
    for (;;) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        int val = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s++ - '0');
            if (++digits > 3 || val > 255) {
                return false;
            }
        }
        parts++;
        if (*s == '\0') {
            break;
        }
        if (*s != '.' || parts == 4) {
            return false;
        }
        s++;
    }
    return parts == 4;
}

static int fails;
static void expect(const char *s, bool want)
{
    bool got = valid_ipv4(s);
    if (got == want) {
        printf("ok:   %-24s -> %s\n", s ? (*s ? s : "\"\"") : "(null)",
               got ? "valid" : "rejected");
    } else {
        printf("FAIL: %-24s -> %s (wanted %s)\n", s ? s : "(null)",
               got ? "valid" : "rejected", want ? "valid" : "rejected");
        fails++;
    }
}

int main(void)
{
    /* Accepted */
    expect("192.168.1.50", true);
    expect("0.0.0.0", true);
    expect("255.255.255.255", true);
    expect("255.255.255.0", true);
    expect("8.8.8.8", true);
    expect("10.0.0.1", true);

    /* Rejected — malformed */
    expect(NULL, false);
    expect("", false);
    expect("192.168.1", false);          /* aton() would accept this */
    expect("192.168.1.50.1", false);
    expect("192.168.1.", false);
    expect(".192.168.1", false);
    expect("192..168.1", false);
    expect("192.168.1.256", false);
    expect("999.1.1.1", false);
    expect("192.168.1.0500", false);     /* >3 digits */

    /* Rejected — not digits */
    expect("192.168.1.a", false);
    expect("0xC0.0xA8.1.1", false);      /* aton() takes hex */
    expect("192.168.1.1 ", false);
    expect(" 192.168.1.1", false);
    expect("+192.168.1.1", false);
    expect("-1.1.1.1", false);
    expect("192.168.1.1\n", false);

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
