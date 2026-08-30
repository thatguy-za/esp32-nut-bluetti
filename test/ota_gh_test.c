/* GitHub update helpers.
 *
 * Two things here can be wrong in ways that are invisible until they
 * matter: version ordering (a string compare puts 0.10.0 before 0.9.0),
 * and the streaming scanner, which has to find "tag_name":" even when the
 * token is split across two reads of the socket. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* Mirrors ota_github.c. */
#define OTA_GH_HTTP_BUF 2048

static int fails;
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

/* Same as ota_github.c. */
static int vercmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        long x = strtol(a, (char **)&a, 10);
        long y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

typedef struct {
    const char *needle;
    size_t matched;
    bool capturing;
    char val[16];
    size_t val_len;
    char found[8][16];
    size_t n;
} scan_t;

static void scan_byte(scan_t *s, char c)
{
    if (s->capturing) {
        if (c == '"') {
            s->val[s->val_len] = '\0';
            if (s->n < 8) {
                const char *v = s->val;
                if (*v == 'v' || *v == 'V') v++;
                snprintf(s->found[s->n++], 16, "%s", v);
            }
            s->capturing = false;
            s->val_len = 0;
        } else if (s->val_len + 1 < sizeof(s->val)) {
            s->val[s->val_len++] = c;
        }
        return;
    }
    if (c == s->needle[s->matched]) {
        s->matched++;
        if (s->needle[s->matched] == '\0') {
            s->capturing = true;
            s->val_len = 0;
            s->matched = 0;
        }
    } else {
        s->matched = (c == s->needle[0]) ? 1 : 0;
    }
}

static void feed(scan_t *s, const char *data, size_t chunk)
{
    size_t len = strlen(data);
    for (size_t i = 0; i < len; i += chunk)
        for (size_t j = i; j < i + chunk && j < len; j++)
            scan_byte(s, data[j]);
}

int main(void)
{
    /* Ordering. The 0.10.0 case is the one a string compare gets wrong. */
    OKF(vercmp("0.3.0", "0.2.9") > 0, "0.3.0 is newer than 0.2.9");
    OKF(vercmp("0.10.0", "0.9.0") > 0, "0.10.0 is newer than 0.9.0");
    OKF(vercmp("1.0.0", "0.99.99") > 0, "1.0.0 is newer than 0.99.99");
    OKF(vercmp("0.3.0", "0.3.0") == 0, "equal versions compare equal");
    OKF(vercmp("0.3.0", "0.3.1") < 0, "0.3.0 is older than 0.3.1");
    OKF(vercmp("0.3", "0.3.0") == 0, "a missing patch reads as zero");

    const char *body =
        "[{\"url\":\"x\",\"tag_name\":\"v0.3.0\",\"name\":\"a\"},"
        "{\"tag_name\":\"v0.2.0\",\"prerelease\":true},"
        "{\"tag_name\":\"v0.1.0\"}]";

    /* Whole thing at once. */
    scan_t s = { .needle = "\"tag_name\":\"" };
    feed(&s, body, strlen(body));
    OKF(s.n == 3, "found %u releases in one read", (unsigned)s.n);
    OKF(strcmp(s.found[0], "0.3.0") == 0, "first is 0.3.0 with the v stripped");
    OKF(strcmp(s.found[2], "0.1.0") == 0, "last is 0.1.0");

    /* One byte at a time — the worst possible split of every token. */
    scan_t s1 = { .needle = "\"tag_name\":\"" };
    feed(&s1, body, 1);
    OKF(s1.n == 3 && strcmp(s1.found[0], "0.3.0") == 0,
        "single-byte reads find the same 3 releases");

    /* Every chunk size up to 40, so no boundary is special-cased by luck. */
    bool all = true;
    for (size_t c = 2; c <= 40; c++) {
        scan_t sc = { .needle = "\"tag_name\":\"" };
        feed(&sc, body, c);
        if (sc.n != 3 || strcmp(sc.found[1], "0.2.0") != 0) all = false;
    }
    OKF(all, "every chunk size from 2 to 40 finds the same releases");

    /* A near-miss must not leave the matcher stuck part-way. */
    scan_t s2 = { .needle = "\"tag_name\":\"" };
    feed(&s2, "\"tag_nam\":\"nope\",\"tag_name\":\"v9.9.9\"", 3);
    OKF(s2.n == 1 && strcmp(s2.found[0], "9.9.9") == 0,
        "a partial key does not swallow the real one");

    /* Other fields must not be picked up. */
    scan_t s3 = { .needle = "\"tag_name\":\"" };
    feed(&s3, "{\"name\":\"v1.2.3\",\"target_commitish\":\"main\"}", 5);
    OKF(s3.n == 0, "other string fields are ignored");

    /* An absurd tag must not run off the end of the buffer. */
    scan_t s4 = { .needle = "\"tag_name\":\"" };
    char big[128];
    snprintf(big, sizeof(big), "\"tag_name\":\"%s\"",
             "1234567890123456789012345678901234567890");
    feed(&s4, big, 7);
    OKF(s4.n == 1 && strlen(s4.found[0]) < 16, "an over-long tag is truncated, not overflowed");

    /* Regression guard for "Out of buffer".
     *
     * github.com redirects a release asset to a signed URL on another
     * host. esp_http_client composes "GET <path>?<query>" into a buffer of
     * buffer_size_tx, whose default is 512 — far too small, so the
     * redirected request could not be written and the download failed
     * after both TLS handshakes had already succeeded.
     *
     * A real one measured 933 characters, 888 of it path and query. The
     * signature inside is a JWT and can grow, so the configured buffer
     * must keep real headroom over that, not just clear it. */
    const int OBSERVED_URL = 933;
    const int TX_BUF = OTA_GH_HTTP_BUF;
    OKF(TX_BUF > OBSERVED_URL, "tx buffer %d clears the observed %d-char URL",
        TX_BUF, OBSERVED_URL);
    OKF(TX_BUF >= OBSERVED_URL * 2, "tx buffer leaves room for a longer token");
    OKF(512 < OBSERVED_URL, "the IDF default of 512 would not have fitted");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
