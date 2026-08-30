/* Password hashing + verification, and the session-cookie parsing the
 * admin server relies on. Uses the same salted-SHA-256 scheme as
 * app_config.c, re-implemented here against the host's mbedtls-free
 * SHA-256 so the test needs no ESP-IDF. The point is the *properties*:
 * right password accepts, wrong rejects, salt makes hashes unique, and a
 * session token is matched exactly rather than by substring. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "sha256.h"

static int fails;
/* Evaluate the condition once, so a condition with side effects can't
 * behave differently between the print and the pass/fail count. */
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

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



/* ---- session cookie handling, mirroring provisioning.c ---- */
#define TOKLEN 32

static bool tok_eq(const char *a, const char *b)
{
    unsigned diff = 0;
    for (int i = 0; i < TOKLEN; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
        if (!a[i] || !b[i]) return false;
    }
    return diff == 0;
}

static bool cookie_token(const char *hdr, char out[TOKLEN + 1])
{
    const char *p = strstr(hdr, "sid=");
    while (p && p != hdr && !(p[-1] == ' ' || p[-1] == ';')) {
        p = strstr(p + 1, "sid=");
    }
    if (!p) return false;
    p += 4;
    size_t n = strspn(p, "0123456789abcdef");
    if (n != TOKLEN) return false;
    memcpy(out, p, TOKLEN);
    out[TOKLEN] = '\0';
    return true;
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

    /* ---- session cookies ----
     * Sign-in is now a form and a cookie, so what matters here is that a
     * token is extracted from the Cookie header exactly and compared
     * whole. The trap is a cookie whose name merely ends in "sid". */
    const char *T = "0123456789abcdef0123456789abcdef";
    char tok[TOKLEN + 1];

    OKF(cookie_token("sid=0123456789abcdef0123456789abcdef", tok) &&
        strcmp(tok, T) == 0, "a lone sid cookie is read");
    OKF(cookie_token("a=1; sid=0123456789abcdef0123456789abcdef; b=2", tok) &&
        strcmp(tok, T) == 0, "sid is found among other cookies");
    OKF(cookie_token("theme=dark; sid=0123456789abcdef0123456789abcdef", tok) &&
        strcmp(tok, T) == 0, "sid is found last in the header");

    /* "othersid=" contains "sid=", so a naive search matches the wrong
     * cookie and authenticates against an attacker-set value. */
    OKF(!cookie_token("othersid=0123456789abcdef0123456789abcdef", tok),
        "a cookie merely ending in 'sid' is not mistaken for ours");
    OKF(cookie_token("othersid=ffffffffffffffffffffffffffffffff; "
                     "sid=0123456789abcdef0123456789abcdef", tok) &&
        strcmp(tok, T) == 0, "the real sid wins over a lookalike before it");

    OKF(!cookie_token("sid=0123456789abcdef", tok), "a short token is rejected");
    OKF(!cookie_token("sid=0123456789abcdef0123456789abcdefff", tok),
        "an over-long token is rejected");
    OKF(!cookie_token("sid=zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", tok),
        "a non-hex token is rejected");
    OKF(!cookie_token("theme=dark", tok), "no sid cookie -> no token");
    OKF(!cookie_token("", tok), "an empty header -> no token");

    /* Whole-token comparison: a prefix must not pass. */
    OKF(tok_eq(T, "0123456789abcdef0123456789abcdef"), "identical tokens match");
    OKF(!tok_eq(T, "0123456789abcdef0123456789abcdee"),
        "a token differing in the last byte is rejected");
    OKF(!tok_eq(T, "1123456789abcdef0123456789abcdef"),
        "a token differing in the first byte is rejected");
    OKF(!tok_eq(T, ""), "an empty token is rejected");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
