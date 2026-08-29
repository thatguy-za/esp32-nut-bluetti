/* Model identification.
 *
 * BLUETTI names are a model followed by digits, and several models are
 * prefixes of others (EL10/EL100V2, AC60/AC60P, AC180/AC180T/AC180P,
 * PR30V2/PR100V2). Picking the wrong entry means decoding a unit with
 * another unit's field set, so the matching rule is worth pinning. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static int fails;
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

typedef struct {
    const char *name;
    bool has_runtime, has_dc_input, has_ac_in_volts, has_ac_in_amps,
         has_ac_out_volts;
} bt_device_t;

/* Same table and order as bt_regs.c. */
static const bt_device_t DEVICES[] = {
    { "AC180",       false, true , true , true , true  },
    { "AC180P",      false, true , true , false, false },
    { "AC180T",      false, true , true , true , true  },
    { "AC2A",        false, true , false, false, false },
    { "AC2P",        false, true , false, false, false },
    { "AC50B",       true , false, false, false, false },
    { "AC60",        false, true , true , false, false },
    { "AC60P",       false, true , true , false, false },
    { "AC70",        true , true , true , true , true  },
    { "AC70P",       false, true , true , true , true  },
    { "AP300",       false, true , true , false, false },
    { "EL10",        true , true , true , true , true  },
    { "EL100V2",     true , true , true , true , true  },
    { "EL30V2",      true , true , true , false, false },
    { "Handsfree 1", true , true , true , true , true  },
    { "PR100V2",     false, true , true , false, false },
    { "PR30V2",      false, true , true , false, false },
};
#define N (sizeof(DEVICES) / sizeof(DEVICES[0]))

/* Same rule as bt_device_lookup(). */
static const bt_device_t *lookup(const char *name)
{
    if (!name || !name[0]) return NULL;
    for (size_t i = 0; i < N; i++) {
        size_t n = strlen(DEVICES[i].name);
        if (strncmp(name, DEVICES[i].name, n) != 0) continue;
        size_t j = n;
        while (name[j] >= '0' && name[j] <= '9') j++;
        if (name[j] == '\0') return &DEVICES[i];
    }
    return NULL;
}

int main(void)
{
    const bt_device_t *d;

    d = lookup("EL102411000123");
    OKF(d && strcmp(d->name, "EL10") == 0, "EL10 with a serial identifies as EL10");

    /* The collision that motivated the digit rule: a plain prefix test
     * hands an EL100V2 the EL10 field set. */
    d = lookup("EL100V22411000123");
    OKF(d && strcmp(d->name, "EL100V2") == 0, "EL100V2 is not claimed by EL10");

    d = lookup("AC60P12345");
    OKF(d && strcmp(d->name, "AC60P") == 0, "AC60P is not claimed by AC60");
    d = lookup("AC6012345");
    OKF(d && strcmp(d->name, "AC60") == 0, "AC60 still matches itself");

    d = lookup("AC180T999");
    OKF(d && strcmp(d->name, "AC180T") == 0, "AC180T is not claimed by AC180");
    d = lookup("AC180P999");
    OKF(d && strcmp(d->name, "AC180P") == 0, "AC180P is not claimed by AC180");

    d = lookup("PR30V2555");
    OKF(d && strcmp(d->name, "PR30V2") == 0, "PR30V2 resolves");
    d = lookup("PR100V2555");
    OKF(d && strcmp(d->name, "PR100V2") == 0, "PR100V2 resolves");

    /* The space is part of the name upstream (Handsfree\s1). */
    d = lookup("Handsfree 1123");
    OKF(d && strcmp(d->name, "Handsfree 1") == 0, "'Handsfree 1' resolves");

    OKF(lookup("EL10") != NULL, "bare model name with no serial resolves");
    OKF(lookup("EP600123") == NULL, "EP600 is not in the table -> unknown");
    OKF(lookup("AC200M123") == NULL, "V1-protocol model -> unknown");
    OKF(lookup("EL10ABC") == NULL, "non-digit tail does not match");
    OKF(lookup("XYZ123") == NULL, "unrelated name -> unknown");
    OKF(lookup("") == NULL, "empty name -> unknown");
    OKF(lookup(NULL) == NULL, "NULL name -> unknown");

    /* Field sets must differ where upstream says they differ, or the
     * gating is pointless. */
    OKF(!lookup("AC2A1")->has_ac_in_volts, "AC2A has no AC input voltage");
    OKF(lookup("EL101")->has_ac_in_volts, "EL10 does have AC input voltage");
    OKF(!lookup("AC50B1")->has_dc_input, "AC50B has no DC input power");
    OKF(!lookup("AC180P1")->has_ac_out_volts, "AC180P has no AC output voltage");
    OKF(!lookup("AC1801")->has_runtime, "AC180 has no runtime estimate");
    OKF(lookup("AC50B1")->has_runtime, "AC50B does report runtime");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
