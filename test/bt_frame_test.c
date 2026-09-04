/* Framing arithmetic for the BLUETTI link: Modbus CRC, the request the
 * device expects, response validation, and the "2a2a" checksum.
 *
 * These mirror bt_session.c / bt_crypto.c rather than linking them, since
 * those pull in mbedtls and NimBLE. The point is that the wire format
 * matches what bluetti-bt-lib produces. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static int fails;
#define OKF(c, ...) do { bool _ok = (c); printf(_ok ? "ok:   " : "FAIL: "); \
                         printf(__VA_ARGS__); printf("\n"); \
                         if (!_ok) fails++; } while (0)

static uint16_t crc16_modbus(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

static void hexsum(const uint8_t *d, size_t n, uint8_t out[2])
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += d[i];
    out[0] = (s >> 8) & 0xFF;
    out[1] = s & 0xFF;
}

/* Same layout as bt_session_read_regs(). */
static void build_read(uint16_t addr, uint16_t count, uint8_t out[8])
{
    out[0] = 0x01;
    out[1] = 0x03;
    out[2] = addr >> 8;
    out[3] = addr & 0xFF;
    out[4] = count >> 8;
    out[5] = count & 0xFF;
    uint16_t c = crc16_modbus(out, 6);
    out[6] = c & 0xFF;          /* CRC little-endian */
    out[7] = c >> 8;
}

int main(void)
{
    /* CRC-16/Modbus known-answer. */
    OKF(crc16_modbus((const uint8_t *)"123456789", 9) == 0x4B37,
        "CRC-16/Modbus(\"123456789\") = 0x4B37");

    /* A read of the SOC block, as bluetti-bt-lib would frame it:
     * slave 01, fn 03, addr 0x0066 (102), qty 0x0010 (16). */
    uint8_t cmd[8];
    build_read(102, 16, cmd);
    OKF(cmd[0] == 0x01 && cmd[1] == 0x03, "request starts 01 03");
    OKF(cmd[2] == 0x00 && cmd[3] == 0x66, "address 102 big-endian -> 00 66");
    OKF(cmd[4] == 0x00 && cmd[5] == 0x10, "quantity 16 big-endian -> 00 10");
    uint16_t c = crc16_modbus(cmd, 6);
    OKF(cmd[6] == (c & 0xFF) && cmd[7] == (c >> 8), "CRC appended little-endian");

    /* Response validation: slave, fn, byte count, data, CRC. */
    uint8_t rsp[9] = { 0x01, 0x03, 0x04, 0x00, 0x4B, 0x00, 0x2D };
    uint16_t rc = crc16_modbus(rsp, 7);
    rsp[7] = rc & 0xFF;
    rsp[8] = rc >> 8;
    OKF(rsp[2] == 4 && (size_t)(3 + rsp[2] + 2) == sizeof(rsp),
        "response length matches its byte count");
    OKF(crc16_modbus(rsp, 3 + rsp[2]) ==
        ((uint16_t)rsp[3 + rsp[2]] | ((uint16_t)rsp[4 + rsp[2]] << 8)),
        "response CRC validates");
    /* Registers are big-endian words: 0x004B = 75, 0x002D = 45. */
    OKF((((int)rsp[3] << 8) | rsp[4]) == 75, "first register decodes to 75");
    OKF((((int)rsp[5] << 8) | rsp[6]) == 45, "second register decodes to 45");

    /* A corrupted byte must fail the CRC. */
    rsp[4] ^= 0xFF;
    OKF(crc16_modbus(rsp, 3 + rsp[2]) !=
        ((uint16_t)rsp[3 + rsp[2]] | ((uint16_t)rsp[4 + rsp[2]] << 8)),
        "a flipped data byte fails the CRC");

    /* Key-exchange checksum: plain byte sum, big-endian. */
    uint8_t body[6] = { 0x02, 0x04, 0xDE, 0xAD, 0xBE, 0xEF };
    uint8_t sum[2];
    hexsum(body, sizeof(body), sum);
    unsigned expect = 0x02 + 0x04 + 0xDE + 0xAD + 0xBE + 0xEF;
    OKF(((unsigned)sum[0] << 8 | sum[1]) == expect,
        "kex checksum = byte sum (0x%04X)", expect);

    /* The challenge reply is 2a2a + 6-byte body + 2-byte checksum. */
    uint8_t frame[10];
    frame[0] = frame[1] = 0x2A;
    memcpy(frame + 2, body, sizeof(body));
    memcpy(frame + 8, sum, 2);
    OKF(frame[0] == 0x2A && frame[1] == 0x2A, "frame carries the 2a2a magic");
    OKF(sizeof(frame) == 2 + sizeof(body) + 2, "frame is magic+body+checksum");

    /* AES padding: plaintext is zero-padded up to the block size, and the
     * header carries the true length so the receiver can trim. */
    for (size_t plain = 1; plain <= 64; plain++) {
        size_t padded = ((plain + 15) / 16) * 16;
        if (padded < plain || padded % 16 != 0 || padded - plain >= 16) {
            OKF(false, "padding wrong for %u", (unsigned)plain);
            break;
        }
    }
    OKF(true, "padding rounds up to the AES block for 1..64 bytes");
    OKF(((8 + 15) / 16) * 16 == 16, "an 8-byte Modbus read pads to one block");

    /* Plain-Modbus reassembly (the unencrypted fallback path): a response
     * frame is 3 + bytecount + 2, and it can arrive split across several
     * notifications. Reconstruct it the way bt_session_feed does. */
    uint8_t full[9] = { 0x01, 0x03, 0x04, 0x00, 0x4B, 0x00, 0x2D, 0x00, 0x00 };
    {
        uint16_t fc = crc16_modbus(full, 3 + full[2]);
        full[3 + full[2]] = fc & 0xFF;
        full[4 + full[2]] = fc >> 8;
    }
    uint8_t buf[32];
    size_t buf_len = 0, consumed = 0;
    const size_t chunks[] = { 1, 2, 6 };   /* 1 + 2 + 6 == 9 */
    for (size_t i = 0, off = 0; i < 3; i++) {
        memcpy(buf + buf_len, full + off, chunks[i]);
        buf_len += chunks[i];
        off += chunks[i];
        while (buf_len >= 3) {
            size_t frame_len = 3 + buf[2] + 2;
            if (buf_len < frame_len) break;
            OKF(crc16_modbus(buf, 3 + buf[2]) ==
                ((uint16_t)buf[3 + buf[2]] | ((uint16_t)buf[4 + buf[2]] << 8)),
                "reassembled plain frame passes CRC");
            OKF(buf[2] == 4 && (((int)buf[3] << 8) | buf[4]) == 75,
                "reassembled plain frame decodes register 0 as 75");
            memmove(buf, buf + frame_len, buf_len - frame_len);
            buf_len -= frame_len;
            consumed++;
        }
    }
    OKF(consumed == 1 && buf_len == 0,
        "exactly one frame recovered, nothing left over");

    /* Write single register (0x06). Request and echo are both 8 bytes:
     * [id][06][addr_hi][addr_lo][val_hi][val_lo][crc_lo][crc_hi]. The
     * plain reassembler must frame it as 8, not by a byte-count field. */
    uint8_t wr[8] = { 0x01, 0x06, 0x07, 0xDB, 0x00, 0x02 };  /* reg 2011 <- 2 */
    {
        uint16_t wc = crc16_modbus(wr, 6);
        wr[6] = wc & 0xFF;
        wr[7] = wc >> 8;
    }
    OKF(wr[1] == 0x06, "write frame carries function 0x06");
    OKF((((int)wr[2] << 8) | wr[3]) == 2011, "address 2011 big-endian");
    OKF((((int)wr[4] << 8) | wr[5]) == 2, "value 2 big-endian");
    OKF(crc16_modbus(wr, 6) == ((uint16_t)wr[6] | ((uint16_t)wr[7] << 8)),
        "write frame CRC little-endian over the first 6 bytes");

    /* Framing decision, mirroring bt_session_feed's plain path. */
    uint8_t exc[5] = { 0x01, 0x86, 0x02, 0x00, 0x00 };   /* write exception */
    #define FRAME_LEN(f) ((f)[1] == 0x06 ? 8u \
                          : ((f)[1] & 0x80) ? 5u \
                          : (unsigned)(3 + (f)[2] + 2))
    OKF(FRAME_LEN(wr) == 8, "a 0x06 echo frames as 8 bytes");
    OKF(FRAME_LEN(exc) == 5, "an exception reply frames as 5 bytes");
    OKF(FRAME_LEN(full) == 9, "a read reply still frames by byte count");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
