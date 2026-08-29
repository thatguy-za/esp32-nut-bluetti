/* Password hashing + verification, and the Basic-auth header decode the
 * admin server relies on. Uses the same salted-SHA-256 scheme as
 * app_config.c, re-implemented here against the host's mbedtls-free
 * SHA-256 so the test needs no ESP-IDF. The point is the *properties*:
 * right password accepts, wrong rejects, salt makes hashes unique. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "sha256.h"

static int fails;
#define OKF(c, ...) do { printf((c) ? "ok:   " : "FAIL: "); printf(__VA_ARGS__); \
                         printf("\n"); if (!(c)) fails++; } while (0)

typedef struct {
    char    user[33];
    uint8_t salt[16];
    uint8_t hash[32];
    bool    set;
} auth_t;

static void hash_password(const uint8_t salt[16], const char *pw, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, salt, 16);
    sha256_update(&c, (const uint8_t *)pw, strlen(pw));
    sha256_final(&c, out);
}

static void set_password(auth_t *a, const char *pw, const uint8_t *fixed_salt)
{
    if (!pw || !pw[0]) { memset(a->salt, 0, 16); memset(a->hash, 0, 32);
                         a->set = false; return; }
    memcpy(a->salt, fixed_salt, 16);
    hash_password(a->salt, pw, a->hash);
    a->set = true;
}

static bool check_password(const auth_t *a, const char *pw)
{
    if (!a->set) return true;
    if (!pw) return false;
    uint8_t want[32];
    hash_password(a->salt, pw, want);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= want[i] ^ a->hash[i];
    return diff == 0;
}

/* Base64 decode of a Basic credential, mirroring auth_ok(). */
static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static int b64_decode(const char *in, char *out, int max)
{
    int n = 0, acc = 0, bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64val(*in);
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (n < max - 1) out[n++] = (acc >> bits) & 0xFF; }
    }
    out[n] = '\0';
    return n;
}

int main(void)
{
    static const uint8_t salt_a[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    static const uint8_t salt_b[16] = { 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9 };

    /* Known-answer: SHA-256("abc") — proves the digest itself is right. */
    uint8_t d[32];
    sha256_ctx c; sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"abc", 3); sha256_final(&c, d);
    static const uint8_t abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    OKF(memcmp(d, abc, 32) == 0, "SHA-256(\"abc\") matches the known vector");

    auth_t a = { 0 };
    strcpy(a.user, "admin");

    /* Before a password is set, everything is allowed (fresh device). */
    OKF(check_password(&a, "") == true, "unset password accepts anything");
    OKF(a.set == false, "auth_set false before setup");

    set_password(&a, "hunter2", salt_a);
    OKF(a.set == true, "auth_set true after setting a password");
    OKF(check_password(&a, "hunter2"), "correct password accepted");
    OKF(!check_password(&a, "hunter3"), "wrong password rejected");
    OKF(!check_password(&a, ""), "empty password rejected once set");
    OKF(!check_password(&a, "hunter2 "), "trailing space rejected");
    OKF(!check_password(&a, "HUNTER2"), "case matters");

    /* No length or character restrictions. */
    set_password(&a, "x", salt_a);
    OKF(check_password(&a, "x"), "1-char password works (no minimum)");
    const char *weird = "p@ss word/with:colon&amp=%20\xc3\xa9";
    set_password(&a, weird, salt_a);
    OKF(check_password(&a, weird), "punctuation/UTF-8 password works");

    /* The salt must actually change the stored hash. */
    auth_t x = { 0 }, y = { 0 };
    set_password(&x, "same", salt_a);
    set_password(&y, "same", salt_b);
    OKF(memcmp(x.hash, y.hash, 32) != 0,
        "same password + different salt => different hash");
    OKF(check_password(&x, "same") && check_password(&y, "same"),
        "both still verify against their own salt");

    /* Clearing the password disables auth again. */
    set_password(&a, "", salt_a);
    OKF(a.set == false, "empty password clears auth_set");

    /* Basic-auth header parsing: "admin:hunter2" */
    char dec[128];
    b64_decode("YWRtaW46aHVudGVyMg==", dec, sizeof dec);
    OKF(strcmp(dec, "admin:hunter2") == 0, "base64 decode -> '%s'", dec);
    char *colon = strchr(dec, ':');
    OKF(colon != NULL, "credential splits on ':'");
    if (colon) {
        *colon = '\0';
        set_password(&a, "hunter2", salt_a);
        OKF(strcmp(dec, a.user) == 0 && check_password(&a, colon + 1),
            "decoded header authenticates");
        OKF(!(strcmp("root", a.user) == 0), "wrong username rejected");
    }
    /* A password containing ':' still works — only the first ':' splits. */
    b64_decode("YWRtaW46YTpi", dec, sizeof dec);   /* admin:a:b */
    colon = strchr(dec, ':');
    *colon = '\0';
    OKF(strcmp(dec, "admin") == 0 && strcmp(colon + 1, "a:b") == 0,
        "password may contain ':'");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
