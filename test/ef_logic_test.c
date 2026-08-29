/* Host-side sanity tests for the EcoFlow pure-logic code (no mbedtls/uecc).
 * cc -I<comp>/private ef_test.c ef_frame.c ef_proto.c ef_stub.c -o ef_test */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ef_frame.h"
#include "ef_proto.h"
#include "ef_crypto.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
                           else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

static void test_crc(void)
{
    const uint8_t s[] = "123456789";
    CHECK(ef_crc16(s, 9) == 0xBB3D, "CRC-16/ARC(\"123456789\")=0x%04X exp 0xBB3D", ef_crc16(s, 9));
    CHECK(ef_crc8(s, 9) == 0xF4, "CRC-8(\"123456789\")=0x%02X exp 0xF4", ef_crc8(s, 9));
}

static void test_packet_roundtrip(void)
{
    uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01 };
    uint8_t buf[EF_INNER_MAX];
    size_t n = ef_packet_build(0x21, 0x35, 1, 1, 0x35, 0x86, 0x03, payload, sizeof payload, buf);
    ef_packet_t p;
    int rc = ef_packet_parse(buf, n, &p);
    CHECK(rc == 0, "packet parse rc=%d", rc);
    CHECK(p.src == 0x21 && p.dst == 0x35 && p.cmd_set == 0x35 && p.cmd_id == 0x86,
          "hdr src=%02X dst=%02X set=%02X id=%02X", p.src, p.dst, p.cmd_set, p.cmd_id);
    CHECK(p.payload_len == sizeof payload && memcmp(p.payload, payload, sizeof payload) == 0,
          "payload len=%zu", p.payload_len);
}

/* XOR-obfuscated payload path (seq[0] != 0) is only on RX; build a frame by
 * hand with seq[0]=0x5A and a XOR'd payload, expect parse to undo it. */
static void test_packet_xor(void)
{
    uint8_t clear[] = { 0x11, 0x22, 0x33 };
    uint8_t buf[64];
    size_t o = 0;
    buf[o++] = 0xAA; buf[o++] = 0x03;
    buf[o++] = sizeof clear; buf[o++] = 0x00;
    buf[o] = ef_crc8(buf, 4); o++;
    buf[o++] = 0x0D;
    buf[o++] = 0x5A; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;  /* seq, seq[0]=0x5A */
    buf[o++] = 0; buf[o++] = 0;
    buf[o++] = 0x02; buf[o++] = 0x11;  /* src, dst */
    buf[o++] = 1; buf[o++] = 1;        /* dsrc ddst */
    buf[o++] = 0xFE; buf[o++] = 0x15;  /* cmd_set cmd_id */
    for (unsigned i = 0; i < sizeof clear; i++) buf[o++] = clear[i] ^ 0x5A;
    uint16_t crc = ef_crc16(buf, o);
    buf[o++] = crc & 0xFF; buf[o++] = crc >> 8;

    ef_packet_t p;
    int rc = ef_packet_parse(buf, o, &p);
    CHECK(rc == 0, "xor packet parse rc=%d", rc);
    CHECK(p.payload_len == 3 && memcmp(p.payload, clear, 3) == 0,
          "xor undone: %02X %02X %02X", p.payload[0], p.payload[1], p.payload[2]);
}

static size_t put_varint(uint8_t *b, uint64_t v)
{
    size_t n = 0;
    do { uint8_t x = v & 0x7F; v >>= 7; if (v) x |= 0x80; b[n++] = x; } while (v);
    return n;
}
static size_t put_tag(uint8_t *b, uint32_t field, uint8_t wire)
{
    return put_varint(b, ((uint64_t)field << 3) | wire);
}

static void test_proto(void)
{
    uint8_t pb[64];
    size_t o = 0;
    /* field 262 (cms_batt_soc) float 87.0 */
    o += put_tag(pb + o, 262, 5);
    float soc = 87.0f; memcpy(pb + o, &soc, 4); o += 4;
    /* field 202 (ac_charger_flag) bool true */
    o += put_tag(pb + o, 202, 0); o += put_varint(pb + o, 1);
    /* field 268 (dsg_rem_time) varint 120 */
    o += put_tag(pb + o, 268, 0); o += put_varint(pb + o, 120);
    /* field 158 (pow_get_bms) float +45.0 => charging into battery */
    o += put_tag(pb + o, 158, 5);
    float bms = 45.0f; memcpy(pb + o, &bms, 4); o += 4;
    /* unknown length-delimited field 99 to exercise skip */
    o += put_tag(pb + o, 99, 2); o += put_varint(pb + o, 3); pb[o++] = 1; pb[o++] = 2; pb[o++] = 3;

    ecoflow_state_t st;
    memset(&st, 0, sizeof st);
    st.minutes_remaining = -1;
    int m = ef_proto_apply_display(pb, o, &st);
    CHECK(m == 4, "proto matched %d fields (exp 4)", m);
    CHECK(st.soc_pct == 87, "soc=%d", st.soc_pct);
    CHECK(st.ac_input_present, "ac_input_present=%d", st.ac_input_present);
    CHECK(st.minutes_remaining == 120, "rem=%d", st.minutes_remaining);
    CHECK(st.charging == 1, "charging=%d (ac present + bms>+1 => charging)", st.charging);
    CHECK(st.valid, "valid=%d", st.valid);
}

static int cb_hits;
static uint8_t cb_last[256];
static size_t cb_last_len;
static void frame_cb(const uint8_t *body, size_t len, uint8_t ft, void *u)
{
    (void)u; cb_hits++; cb_last_len = len;
    if (len <= sizeof cb_last) memcpy(cb_last, body, len);
    printf("      frame_cb ft=%u len=%zu\n", ft, len);
}

static void test_reasm_split(void)
{
    uint8_t payload[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    uint8_t frame[64];
    size_t n = ef_enc_simple_encode(payload, sizeof payload, frame);

    ef_reasm_t r; ef_reasm_reset(&r);
    cb_hits = 0;
    ef_reasm_feed(&r, frame, 5, frame_cb, NULL);          /* partial */
    CHECK(cb_hits == 0, "no callback on partial frame");
    ef_reasm_feed(&r, frame + 5, n - 5, frame_cb, NULL);  /* rest */
    CHECK(cb_hits == 1, "one callback after full frame");
    CHECK(cb_last_len == sizeof payload && memcmp(cb_last, payload, sizeof payload) == 0,
          "reassembled payload matches");
}

int main(void)
{
    test_crc();
    test_packet_roundtrip();
    test_packet_xor();
    test_proto();
    test_reasm_split();
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
