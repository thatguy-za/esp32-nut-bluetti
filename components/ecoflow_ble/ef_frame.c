#include "ef_frame.h"
#include "ef_crypto.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "ef_frame";

/* ------------------------------------------------------------------ */
/* inner Packet                                                        */
/* ------------------------------------------------------------------ */

size_t ef_packet_build(uint8_t src, uint8_t dst, uint8_t dsrc, uint8_t ddst,
                       uint8_t cmd_set, uint8_t cmd_id, uint8_t version,
                       const uint8_t *payload, size_t payload_len,
                       uint8_t *out)
{
    size_t o = 0;
    out[o++] = 0xAA;
    out[o++] = version;
    out[o++] = (uint8_t)(payload_len & 0xFF);
    out[o++] = (uint8_t)(payload_len >> 8);
    out[o] = ef_crc8(out, 4);
    o++;
    out[o++] = 0x0D;                 /* product byte (product_id >= 0) */
    out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x00;  /* seq */
    out[o++] = 0x00; out[o++] = 0x00;                                    /* zeros */
    out[o++] = src;
    out[o++] = dst;
    if ((version & 0x0F) >= 0x03) {
        out[o++] = dsrc;
        out[o++] = ddst;
    }
    out[o++] = cmd_set;
    out[o++] = cmd_id;
    if (payload_len) {
        memcpy(&out[o], payload, payload_len);
        o += payload_len;
    }
    uint16_t crc = ef_crc16(out, o);
    out[o++] = (uint8_t)(crc & 0xFF);
    out[o++] = (uint8_t)(crc >> 8);
    return o;
}

int ef_packet_parse(const uint8_t *data, size_t len, ef_packet_t *pkt)
{
    if (len < 5 || data[0] != 0xAA) {
        return -1;
    }
    uint8_t vb = data[1];
    uint8_t version = vb & 0x0F;
    bool sentinel = (vb & 0x10) != 0;

    if (version == 4) {
        return -1;               /* V4 not supported here */
    }
    if ((version == 2 && len < 18) || (version == 3 && len < 20)) {
        return -1;
    }
    if (ef_crc8(data, 4) != data[4]) {
        return -1;
    }
    uint16_t plen = (uint16_t)data[2] | ((uint16_t)data[3] << 8);

    bool ends_bbbb = (len >= 2 && data[len - 2] == 0xBB && data[len - 1] == 0xBB);
    if ((version == 2 || version == 3) && !sentinel && !ends_bbbb) {
        uint16_t want = (uint16_t)data[len - 2] | ((uint16_t)data[len - 1] << 8);
        if (ef_crc16(data, len - 2) != want) {
            return -1;
        }
    }

    size_t payload_start = (version == 2) ? 16 : 18;
    if (payload_start + plen > len || plen > EF_PAYLOAD_MAX) {
        return -1;
    }

    memcpy(pkt->seq, &data[6], 4);
    pkt->src = data[12];
    pkt->dst = data[13];
    if (version == 2) {
        pkt->cmd_set = data[14];
        pkt->cmd_id = data[15];
    } else {
        pkt->cmd_set = data[16];
        pkt->cmd_id = data[17];
    }
    pkt->version = vb;
    pkt->payload_len = plen;
    memcpy(pkt->payload, &data[payload_start], plen);

    if (pkt->seq[0] != 0) {
        for (size_t i = 0; i < pkt->payload_len; i++) {
            pkt->payload[i] ^= pkt->seq[0];
        }
    }
    if (sentinel && pkt->payload_len >= 2 &&
        pkt->payload[pkt->payload_len - 2] == 0xBB &&
        pkt->payload[pkt->payload_len - 1] == 0xBB) {
        pkt->payload_len -= 2;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* outer EncPacket                                                     */
/* ------------------------------------------------------------------ */

size_t ef_enc_simple_encode(const uint8_t *payload, size_t len, uint8_t *out)
{
    size_t o = 0;
    out[o++] = 0x5A; out[o++] = 0x5A;
    out[o++] = 0x00;                 /* FRAME_TYPE_COMMAND << 4 */
    out[o++] = 0x01;
    uint16_t bl = (uint16_t)(len + 2);
    out[o++] = (uint8_t)(bl & 0xFF);
    out[o++] = (uint8_t)(bl >> 8);
    memcpy(&out[o], payload, len);
    o += len;
    uint16_t crc = ef_crc16(out, o);
    out[o++] = (uint8_t)(crc & 0xFF);
    out[o++] = (uint8_t)(crc >> 8);
    return o;
}

size_t ef_enc_encode_encrypted(const uint8_t *inner, size_t inner_len,
                               const uint8_t key[16], const uint8_t iv[16],
                               uint8_t *out)
{
    uint8_t plain[EF_INNER_MAX + 16];
    size_t pad = 16 - (inner_len % 16);   /* PKCS#7: full block when aligned */
    if (inner_len + pad > sizeof(plain)) {
        return 0;
    }
    memcpy(plain, inner, inner_len);
    memset(plain + inner_len, (int)pad, pad);
    size_t enc_len = inner_len + pad;

    size_t o = 0;
    out[o++] = 0x5A; out[o++] = 0x5A;
    out[o++] = 0x10;                 /* FRAME_TYPE_PROTOCOL << 4 */
    out[o++] = 0x01;
    uint16_t bl = (uint16_t)(enc_len + 2);
    out[o++] = (uint8_t)(bl & 0xFF);
    out[o++] = (uint8_t)(bl >> 8);
    if (ef_aes128_cbc_encrypt(key, iv, plain, enc_len, &out[o]) != 0) {
        return 0;
    }
    o += enc_len;
    uint16_t crc = ef_crc16(out, o);
    out[o++] = (uint8_t)(crc & 0xFF);
    out[o++] = (uint8_t)(crc >> 8);
    return o;
}

/* ------------------------------------------------------------------ */
/* reassembly                                                          */
/* ------------------------------------------------------------------ */

void ef_reasm_reset(ef_reasm_t *r)
{
    r->len = 0;
}

static size_t find_prefix(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++) {
        if (b[i] == 0x5A && b[i + 1] == 0x5A) {
            return i;
        }
    }
    return n;   /* not found */
}

static size_t pkcs7_trim(uint8_t *buf, size_t len)
{
    if (len == 0 || len % 16) {
        return len;
    }
    uint8_t n = buf[len - 1];
    if (n == 0 || n > 16 || n > len) {
        return len;
    }
    for (size_t i = len - n; i < len; i++) {
        if (buf[i] != n) {
            return len;
        }
    }
    return len - n;
}

size_t ef_frame_decrypt(const uint8_t *body, size_t body_len,
                        const uint8_t key[16], const uint8_t iv[16],
                        uint8_t *out, size_t out_cap)
{
    size_t aligned = body_len - (body_len % 16);
    if (aligned == 0 || aligned > out_cap) {
        return 0;
    }
    if (ef_aes128_cbc_decrypt(key, iv, body, aligned, out) != 0) {
        return 0;
    }
    return pkcs7_trim(out, aligned);
}

void ef_reasm_feed(ef_reasm_t *r, const uint8_t *chunk, size_t len,
                   ef_frame_cb_t cb, void *user)
{
    if (r->len + len > sizeof(r->buf)) {
        ESP_LOGW(TAG, "reasm overflow, dropping %u buffered", (unsigned)r->len);
        r->len = 0;
        if (len > sizeof(r->buf)) {
            len = sizeof(r->buf);
        }
    }
    memcpy(&r->buf[r->len], chunk, len);
    r->len += len;

    for (;;) {
        size_t start = find_prefix(r->buf, r->len);
        if (start >= r->len) {
            r->len = 0;
            break;
        }
        if (start > 0) {
            memmove(r->buf, &r->buf[start], r->len - start);
            r->len -= start;
        }
        if (r->len < 6) {
            break;
        }
        uint16_t bl = (uint16_t)r->buf[4] | ((uint16_t)r->buf[5] << 8);
        if (bl < 2 || bl > EF_FRAME_MAX) {
            memmove(r->buf, &r->buf[2], r->len - 2);
            r->len -= 2;
            continue;
        }
        size_t frame_end = 6 + bl;
        if (r->len < frame_end) {
            break;                          /* wait for more */
        }
        size_t body_len = bl - 2;
        uint16_t want = (uint16_t)r->buf[frame_end - 2] |
                        ((uint16_t)r->buf[frame_end - 1] << 8);
        if (ef_crc16(r->buf, frame_end - 2) != want) {
            memmove(r->buf, &r->buf[2], r->len - 2);
            r->len -= 2;
            continue;
        }

        uint8_t ft = r->buf[2] >> 4;
        cb(&r->buf[6], body_len, ft, user);

        memmove(r->buf, &r->buf[frame_end], r->len - frame_end);
        r->len -= frame_end;
    }
}
