#include "bt_session.h"
#include "bt_crypto.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"

static const char *TAG = "bt_session";

/* The static AES key the handshake starts from, from the vendor app. */
static const uint8_t LOCAL_AES_KEY[16] = {
    0x45,0x9F,0xC5,0x35,0x80,0x89,0x41,0xF1,0x70,0x91,0xE0,0x99,0x3E,0xE3,0xE9,0x3D,
};

#define KEX_MAGIC0 0x2A
#define KEX_MAGIC1 0x2A

/* Message types carried in body[0] of a 2a2a frame. */
#define MSG_CHALLENGE          1
#define MSG_CHALLENGE_ACCEPTED 3
#define MSG_PEER_PUBKEY        4
#define MSG_PUBKEY_ACCEPTED    6

#define RX_MAX   512
#define MODBUS_SLAVE 0x01
#define MODBUS_READ  0x03

struct bt_session {
    bt_sess_tx_t    tx;
    bt_sess_regs_t  on_regs;
    void           *user;

    bt_sess_state_t state;
    bool            plain;      /* device does not encrypt; raw Modbus */

    uint8_t         unsecure_key[16];
    uint8_t         unsecure_iv[16];
    bool            have_unsecure;

    uint8_t         secure_key[32];
    bool            have_secure;

    uint8_t         peer_pub[BT_P256_PUBLEN];
    bt_keypair_t   *kp;

    /* Encrypted-frame reassembly: notifications are MTU-sized fragments. */
    uint8_t         rx[RX_MAX];
    size_t          rx_len;

    /* Address of the read currently in flight, to label the response. */
    uint16_t        pending_addr;
    uint16_t        pending_count;
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* The frame checksum is a plain byte sum rendered big-endian. */
static void hexsum(const uint8_t *data, size_t len, uint8_t out[2])
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    out[0] = (sum >> 8) & 0xFF;
    out[1] = sum & 0xFF;
}

/* Build "2a2a || body || checksum". */
static size_t build_kex(const uint8_t *body, size_t body_len,
                        uint8_t *out, size_t out_sz)
{
    if (out_sz < body_len + 4) {
        return 0;
    }
    out[0] = KEX_MAGIC0;
    out[1] = KEX_MAGIC1;
    memcpy(out + 2, body, body_len);
    hexsum(body, body_len, out + 2 + body_len);
    return body_len + 4;
}

static void session_fail(bt_session_t *s, const char *why)
{
    ESP_LOGE(TAG, "handshake failed: %s", why);
    s->state = BT_SESS_FAILED;
}

/* ------------------------------------------------------------------ */
/* AES wrapper                                                         */
/* ------------------------------------------------------------------ */

/*
 * Wire format:
 *   with a caller-supplied IV (handshake phase):
 *     [len_be16][ciphertext...]
 *   with a per-message IV (once the secure key is up):
 *     [len_be16][iv_seed:4][ciphertext...]      iv = MD5(iv_seed)
 * `len` is the plaintext length before padding.
 */
static size_t aes_wrap(const uint8_t *key, size_t key_len, const uint8_t *iv,
                       const uint8_t *plain, size_t plain_len,
                       uint8_t *out, size_t out_sz)
{
    uint8_t iv_buf[16];
    size_t header = 2;

    out[0] = (plain_len >> 8) & 0xFF;
    out[1] = plain_len & 0xFF;

    if (iv == NULL) {
        uint8_t seed[4];
        bt_random(seed, sizeof(seed));
        memcpy(out + 2, seed, sizeof(seed));
        bt_md5(seed, sizeof(seed), iv_buf);
        iv = iv_buf;
        header = 6;
    }

    size_t padded = ((plain_len + BT_AES_BLOCK - 1) / BT_AES_BLOCK) * BT_AES_BLOCK;
    if (header + padded > out_sz || padded > RX_MAX) {
        return 0;
    }

    uint8_t tmp[RX_MAX];
    memcpy(tmp, plain, plain_len);
    memset(tmp + plain_len, 0, padded - plain_len);   /* zero padding */

    if (bt_aes_cbc_encrypt(key, key_len, iv, tmp, out + header, padded) != 0) {
        return 0;
    }
    return header + padded;
}

/* Total wire length of the frame `buf` starts, or 0 if more is needed. */
static size_t framed_len(const bt_session_t *s, const uint8_t *buf, size_t len)
{
    size_t header = s->have_secure ? 6 : 2;
    if (len < header) {
        return 0;
    }
    size_t plain_len = ((size_t)buf[0] << 8) | buf[1];
    size_t padded = ((plain_len + BT_AES_BLOCK - 1) / BT_AES_BLOCK) * BT_AES_BLOCK;
    return header + padded;
}

static int aes_unwrap(const bt_session_t *s, const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_sz, size_t *out_len)
{
    const uint8_t *key = s->have_secure ? s->secure_key : s->unsecure_key;
    size_t key_len = s->have_secure ? 32 : 16;

    size_t plain_len = ((size_t)in[0] << 8) | in[1];
    uint8_t iv_buf[16];
    const uint8_t *iv;
    const uint8_t *ct;
    size_t ct_len;

    if (s->have_secure) {
        if (in_len < 6) return -1;
        bt_md5(in + 2, 4, iv_buf);
        iv = iv_buf;
        ct = in + 6;
        ct_len = in_len - 6;
    } else {
        iv = s->unsecure_iv;
        ct = in + 2;
        ct_len = in_len - 2;
    }

    if (ct_len % BT_AES_BLOCK != 0 || plain_len > ct_len || plain_len > out_sz) {
        return -1;
    }
    uint8_t tmp[RX_MAX];
    if (ct_len > sizeof(tmp)) {
        return -1;
    }
    if (bt_aes_cbc_decrypt(key, key_len, iv, ct, tmp, ct_len) != 0) {
        return -1;
    }
    memcpy(out, tmp, plain_len);
    *out_len = plain_len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Handshake steps                                                     */
/* ------------------------------------------------------------------ */

/* Challenge: 4 bytes of data. The IV is MD5 over those bytes *reversed*,
 * and the first key is that IV XOR the app's static key. */
static void on_challenge(bt_session_t *s, const uint8_t *data, size_t len)
{
    if (len != 4) {
        session_fail(s, "challenge length");
        return;
    }
    uint8_t rev[4] = { data[3], data[2], data[1], data[0] };
    bt_md5(rev, sizeof(rev), s->unsecure_iv);
    for (int i = 0; i < 16; i++) {
        s->unsecure_key[i] = s->unsecure_iv[i] ^ LOCAL_AES_KEY[i];
    }
    s->have_unsecure = true;

    uint8_t body[6] = { 0x02, 0x04 };
    memcpy(body + 2, s->unsecure_iv + 8, 4);

    uint8_t frame[16];
    size_t n = build_kex(body, sizeof(body), frame, sizeof(frame));
    if (n == 0 || s->tx(frame, n, s->user) != 0) {
        session_fail(s, "challenge reply write");
        return;
    }
    s->state = BT_SESS_CHALLENGE;
    ESP_LOGI(TAG, "challenge answered");
}

/* Peer public key: 64 bytes of key followed by a 64-byte signature over
 * (key || unsecure_iv), made with the vendor's K2 key. */
static void on_peer_pubkey(bt_session_t *s, const uint8_t *data, size_t len)
{
    if (len != 128) {
        session_fail(s, "pubkey length");
        return;
    }
    uint8_t signed_blob[BT_P256_PUBLEN + 16];
    memcpy(signed_blob, data, BT_P256_PUBLEN);
    memcpy(signed_blob + BT_P256_PUBLEN, s->unsecure_iv, 16);

    if (bt_ecdsa_verify_k2(signed_blob, sizeof(signed_blob),
                           data + BT_P256_PUBLEN) != 0) {
        session_fail(s, "peer key signature");
        return;
    }
    memcpy(s->peer_pub, data, BT_P256_PUBLEN);

    bt_keypair_free(s->kp);
    s->kp = bt_keypair_new();
    if (!s->kp) {
        session_fail(s, "keypair");
        return;
    }
    uint8_t my_pub[BT_P256_PUBLEN];
    if (bt_keypair_public(s->kp, my_pub) != 0) {
        session_fail(s, "public key export");
        return;
    }

    /* Sign our key the same way, with L1. */
    uint8_t to_sign[BT_P256_PUBLEN + 16];
    memcpy(to_sign, my_pub, BT_P256_PUBLEN);
    memcpy(to_sign + BT_P256_PUBLEN, s->unsecure_iv, 16);

    uint8_t sig[BT_P256_SIGLEN];
    if (bt_ecdsa_sign_l1(to_sign, sizeof(to_sign), sig) != 0) {
        session_fail(s, "signing");
        return;
    }

    uint8_t body[2 + BT_P256_PUBLEN + BT_P256_SIGLEN];
    body[0] = 0x05;
    body[1] = 0x80;
    memcpy(body + 2, my_pub, BT_P256_PUBLEN);
    memcpy(body + 2 + BT_P256_PUBLEN, sig, BT_P256_SIGLEN);

    uint8_t frame[sizeof(body) + 4];
    size_t n = build_kex(body, sizeof(body), frame, sizeof(frame));
    if (n == 0) {
        session_fail(s, "frame build");
        return;
    }
    /* This reply is itself encrypted under the handshake key. */
    uint8_t wrapped[RX_MAX];
    size_t wn = aes_wrap(s->unsecure_key, 16, s->unsecure_iv, frame, n,
                         wrapped, sizeof(wrapped));
    if (wn == 0 || s->tx(wrapped, wn, s->user) != 0) {
        session_fail(s, "pubkey reply write");
        return;
    }
    s->state = BT_SESS_PUBKEY;
    ESP_LOGI(TAG, "public key sent");
}

static void on_pubkey_accepted(bt_session_t *s, const uint8_t *data, size_t len)
{
    if (len != 1 || data[0] != 0) {
        session_fail(s, "key rejected");
        return;
    }
    if (bt_ecdh_shared(s->kp, s->peer_pub, s->secure_key) != 0) {
        session_fail(s, "ECDH");
        return;
    }
    s->have_secure = true;
    s->state = BT_SESS_READY;
    ESP_LOGW(TAG, "secure session established");
}

/* Dispatch a decoded 2a2a frame. */
static void handle_kex(bt_session_t *s, const uint8_t *frame, size_t len)
{
    if (len < 5) {
        return;
    }
    const uint8_t *body = frame + 2;
    size_t body_len = len - 4;

    uint8_t want[2];
    hexsum(body, body_len, want);
    if (memcmp(want, frame + 2 + body_len, 2) != 0) {
        ESP_LOGW(TAG, "kex checksum mismatch, ignoring frame");
        return;
    }

    const uint8_t type = body[0];
    const uint8_t *data = body + 2;      /* body[1] is a length/flag byte */
    size_t data_len = body_len - 2;

    switch (type) {
    case MSG_CHALLENGE:          on_challenge(s, data, data_len); break;
    case MSG_CHALLENGE_ACCEPTED: ESP_LOGI(TAG, "challenge accepted"); break;
    case MSG_PEER_PUBKEY:        on_peer_pubkey(s, data, data_len); break;
    case MSG_PUBKEY_ACCEPTED:    on_pubkey_accepted(s, data, data_len); break;
    default:
        ESP_LOGW(TAG, "unknown kex message type %u", type);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Modbus                                                              */
/* ------------------------------------------------------------------ */

static void handle_modbus(bt_session_t *s, const uint8_t *f, size_t len)
{
    if (len < 5) {
        return;
    }
    if (f[1] == (MODBUS_READ | 0x80)) {
        ESP_LOGW(TAG, "modbus exception %u for address %u", f[2], s->pending_addr);
        return;
    }
    /* Accept any slave id — the reference does not check it, and a unit
     * answering as something other than 0x01 would otherwise be silently
     * dropped. Only the function code and CRC have to hold. */
    if (f[1] != MODBUS_READ) {
        return;
    }
    size_t count = f[2];
    if (3 + count + 2 > len) {
        return;
    }
    uint16_t want = bt_crc16_modbus(f, 3 + count);
    uint16_t got = (uint16_t)f[3 + count] | ((uint16_t)f[4 + count] << 8);
    if (want != got) {
        ESP_LOGW(TAG, "modbus CRC mismatch");
        return;
    }
    if (s->on_regs) {
        s->on_regs(s->pending_addr, f + 3, count, s->user);
    }
}

int bt_session_read_regs(bt_session_t *s, uint16_t addr, uint16_t count)
{
    if (!bt_session_ready(s)) {
        return -1;
    }
    uint8_t cmd[8];
    cmd[0] = MODBUS_SLAVE;
    cmd[1] = MODBUS_READ;
    cmd[2] = addr >> 8;      /* big-endian on the wire */
    cmd[3] = addr & 0xFF;
    cmd[4] = count >> 8;
    cmd[5] = count & 0xFF;
    uint16_t crc = bt_crc16_modbus(cmd, 6);
    cmd[6] = crc & 0xFF;     /* CRC is little-endian */
    cmd[7] = crc >> 8;

    s->pending_addr = addr;
    s->pending_count = count;
    s->rx_len = 0;           /* a fresh response is coming */

    if (s->plain) {
        return s->tx(cmd, sizeof(cmd), s->user);
    }

    uint8_t wrapped[RX_MAX];
    size_t n = aes_wrap(s->secure_key, 32, NULL, cmd, sizeof(cmd),
                        wrapped, sizeof(wrapped));
    if (n == 0) {
        return -1;
    }
    return s->tx(wrapped, n, s->user);
}

/* ------------------------------------------------------------------ */
/* Receive path                                                        */
/* ------------------------------------------------------------------ */

void bt_session_feed(bt_session_t *s, const uint8_t *data, size_t len)
{
    /*
     * Plain mode: no crypto, the notifications are raw Modbus. Buffer
     * fragments until a whole response is in hand — [id][fn][bytecount]
     * [data...][crc:2] — then hand it straight to the Modbus parser.
     */
    if (s->plain) {
        if (s->rx_len + len > sizeof(s->rx)) {
            s->rx_len = 0;
            return;
        }
        memcpy(s->rx + s->rx_len, data, len);
        s->rx_len += len;

        while (s->rx_len >= 3) {
            size_t frame = 3 + s->rx[2] + 2;
            if (s->rx_len < frame) {
                return;                        /* wait for the rest */
            }
            handle_modbus(s, s->rx, frame);
            memmove(s->rx, s->rx + frame, s->rx_len - frame);
            s->rx_len -= frame;
        }
        return;
    }

    /* Plaintext key-exchange frames arrive whole and unencrypted. */
    if (len >= 2 && data[0] == KEX_MAGIC0 && data[1] == KEX_MAGIC1 &&
        !s->have_unsecure) {
        handle_kex(s, data, len);
        return;
    }
    if (!s->have_unsecure) {
        ESP_LOGW(TAG, "encrypted data before the key exchange, ignoring");
        return;
    }

    if (s->rx_len + len > sizeof(s->rx)) {
        ESP_LOGW(TAG, "reassembly overflow, dropping");
        s->rx_len = 0;
        return;
    }
    memcpy(s->rx + s->rx_len, data, len);
    s->rx_len += len;

    for (;;) {
        size_t need = framed_len(s, s->rx, s->rx_len);
        if (need == 0 || s->rx_len < need) {
            return;                    /* wait for the rest */
        }
        uint8_t plain[RX_MAX];
        size_t plain_len = 0;
        if (aes_unwrap(s, s->rx, need, plain, sizeof(plain), &plain_len) != 0) {
            ESP_LOGW(TAG, "decrypt failed, resetting buffer");
            s->rx_len = 0;
            return;
        }
        /* Consume the frame before dispatching: a handler may transmit. */
        memmove(s->rx, s->rx + need, s->rx_len - need);
        s->rx_len -= need;

        if (plain_len >= 2 && plain[0] == KEX_MAGIC0 && plain[1] == KEX_MAGIC1) {
            handle_kex(s, plain, plain_len);
        } else {
            handle_modbus(s, plain, plain_len);
        }
        if (s->rx_len == 0) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ */

bt_session_t *bt_session_new(bt_sess_tx_t tx, bt_sess_regs_t on_regs, void *user)
{
    bt_session_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->tx = tx;
    s->on_regs = on_regs;
    s->user = user;
    bt_crypto_init();
    return s;
}

void bt_session_free(bt_session_t *s)
{
    if (!s) {
        return;
    }
    bt_keypair_free(s->kp);
    free(s);
}

void bt_session_reset(bt_session_t *s)
{
    if (!s) {
        return;
    }
    bt_keypair_free(s->kp);
    s->kp = NULL;
    s->state = BT_SESS_IDLE;
    s->plain = false;
    s->have_unsecure = false;
    s->have_secure = false;
    s->rx_len = 0;
    memset(s->secure_key, 0, sizeof(s->secure_key));
    memset(s->unsecure_key, 0, sizeof(s->unsecure_key));
}

void bt_session_use_plain(bt_session_t *s)
{
    if (!s || s->state != BT_SESS_IDLE) {
        return;                    /* a handshake is already under way */
    }
    s->plain = true;
    s->rx_len = 0;
    s->state = BT_SESS_READY;
    ESP_LOGW(TAG, "no challenge seen; falling back to unencrypted Modbus");
}

bool bt_session_is_plain(const bt_session_t *s)
{
    return s && s->plain;
}

bt_sess_state_t bt_session_state(const bt_session_t *s)
{
    return s ? s->state : BT_SESS_IDLE;
}

bool bt_session_ready(const bt_session_t *s)
{
    return s && s->state == BT_SESS_READY && (s->have_secure || s->plain);
}
