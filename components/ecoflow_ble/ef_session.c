#include "ef_session.h"
#include "ef_crypto.h"
#include "ef_frame.h"
#include "ef_proto.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ef_session";

/* Embedded EcoFlow key table (see ef_keydata.bin). */
extern const uint8_t ef_keydata_start[] asm("_binary_ef_keydata_bin_start");
extern const uint8_t ef_keydata_end[]   asm("_binary_ef_keydata_bin_end");

#define STAGE_TIMEOUT_US (12 * 1000 * 1000LL)

/* CONFIG_ECOFLOW_BLE_TRACE dumps handshake material (including the derived
 * keys) at INFO level — for bringing the BLE link up on new hardware. */
#ifdef CONFIG_ECOFLOW_BLE_TRACE
#define TRACE(msg, buf, len) \
    ESP_LOG_BUFFER_HEXDUMP(TAG, (buf), (len), ESP_LOG_INFO)
#define TRACEF(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define TRACE(msg, buf, len) do { } while (0)
#define TRACEF(...) do { } while (0)
#endif

struct ef_session {
    ef_sess_state_t state;
    char            err[64];
    int64_t         stage_deadline;

    char            sn[20];
    char            user_id[40];
    int             encrypt_type;
    int             low_battery_pct;

    ef_write_fn     write;
    void           *write_user;
    ef_state_fn     on_state;
    void           *state_user;

    uint8_t         priv[EF_ECC_PRIV_LEN];
    uint8_t         pub[EF_ECC_PUB_LEN];
    uint8_t         key[16];        /* current AES key */
    uint8_t         iv[16];
    bool            have_key;

    ef_reasm_t      reasm;
    ecoflow_state_t st;

    int64_t         last_time_sync_us;

    /* scratch buffers kept off the (small) NimBLE host task stack;
     * used synchronously on that single task only. */
    uint8_t         sc_plain[EF_FRAME_MAX];
    uint8_t         sc_inner[EF_INNER_MAX];
    uint8_t         sc_frame[EF_FRAME_MAX];
    ef_packet_t     sc_pkt;
};

/* ------------------------------------------------------------------ */

static void fail(ef_session_t *s, const char *msg)
{
    if (s->state != EF_SESS_ERROR) {
        strlcpy(s->err, msg, sizeof(s->err));
        s->state = EF_SESS_ERROR;
        ESP_LOGW(TAG, "session error: %s", msg);
    }
}

static void set_stage(ef_session_t *s, ef_sess_state_t st)
{
    s->state = st;
    s->stage_deadline = esp_timer_get_time() + STAGE_TIMEOUT_US;
}

static int ecdh_type_size(uint8_t curve_num)
{
    switch (curve_num) {
    case 1:  return 52;
    case 2:  return 56;
    case 3:
    case 4:  return 64;
    default: return 40;
    }
}

static int send_frame(ef_session_t *s, const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return -1;
    }
    return s->write(buf, len, s->write_user);
}

/* Encrypt an inner Packet and send it. */
static int send_packet(ef_session_t *s, uint8_t src, uint8_t dst,
                       uint8_t cmd_set, uint8_t cmd_id, uint8_t version,
                       const uint8_t *payload, size_t plen)
{
    size_t ilen = ef_packet_build(src, dst, 0x01, 0x01, cmd_set, cmd_id,
                                  version, payload, plen, s->sc_inner);
    size_t flen = ef_enc_encode_encrypted(s->sc_inner, ilen, s->key, s->iv,
                                          s->sc_frame);
    return send_frame(s, s->sc_frame, flen);
}

/* ------------------------------------------------------------------ */
/* handshake steps                                                     */
/* ------------------------------------------------------------------ */

static int step_send_pubkey(ef_session_t *s)
{
    if (ef_ecdh_keygen(s->priv, s->pub) != 0) {
        fail(s, "ecdh keygen failed");
        return -1;
    }
    uint8_t payload[2 + EF_ECC_PUB_LEN];
    payload[0] = 0x01;
    payload[1] = 0x00;
    memcpy(&payload[2], s->pub, EF_ECC_PUB_LEN);

    uint8_t frame[64];
    size_t flen = ef_enc_simple_encode(payload, sizeof(payload), frame);
    TRACEF("-> pubkey exchange (%u B)", (unsigned)flen);
    TRACE("our pubkey", s->pub, EF_ECC_PUB_LEN);
    if (send_frame(s, frame, flen) != 0) {
        fail(s, "pubkey write failed");
        return -1;
    }
    set_stage(s, EF_SESS_PUBKEY);
    return 0;
}

static void on_pubkey_reply(ef_session_t *s, const uint8_t *body, size_t len)
{
    TRACEF("<- pubkey reply (%u B)", (unsigned)len);
    TRACE("pubkey reply", body, len);
    if (len < 3) {
        fail(s, "short pubkey reply");
        return;
    }
    int size = ecdh_type_size(body[2]);
    TRACEF("ecdh_type=0x%02x -> keysize %d", body[2], size);
    if ((size_t)(3 + size) > len || size != EF_ECC_PUB_LEN) {
        fail(s, "unexpected ecdh curve");
        return;
    }
    uint8_t shared[EF_ECC_SECRET_LEN];
    if (ef_ecdh_shared(s->priv, &body[3], shared) != 0) {
        fail(s, "ecdh shared failed");
        return;
    }
    ef_md5(shared, sizeof(shared), s->iv);
    memcpy(s->key, shared, 16);
    s->have_key = true;
    TRACE("device pubkey", &body[3], size);
    TRACE("shared secret", shared, sizeof(shared));
    TRACE("iv = md5(shared)", s->iv, 16);

    uint8_t req = 0x02;
    uint8_t frame[32];
    size_t flen = ef_enc_simple_encode(&req, 1, frame);
    if (send_frame(s, frame, flen) != 0) {
        fail(s, "keyinfo write failed");
        return;
    }
    set_stage(s, EF_SESS_KEYINFO);
}

static void gen_session_key(const uint8_t seed[2], const uint8_t srand[16],
                            uint8_t out[16])
{
    size_t klen = (size_t)(ef_keydata_end - ef_keydata_start);
    size_t pos = (size_t)seed[0] * 0x10 +
                 (size_t)((seed[1] - 1) & 0xFF) * 0x100;
    uint8_t buf[32];
    if (pos + 16 <= klen) {
        memcpy(buf, &ef_keydata_start[pos], 16);
    } else {
        memset(buf, 0, 16);
    }
    memcpy(&buf[16], srand, 16);
    ef_md5(buf, sizeof(buf), out);
}

static void on_keyinfo_reply(ef_session_t *s, const uint8_t *body, size_t len)
{
    if (len < 17 || body[0] != 0x02) {
        fail(s, "bad keyinfo reply");
        return;
    }
    uint8_t dec[64];
    size_t enc_len = (len - 1) - ((len - 1) % 16);   /* whole blocks only */
    if (enc_len < 32 || enc_len > sizeof(dec)) {
        fail(s, "keyinfo size");
        return;
    }
    if (ef_aes128_cbc_decrypt(s->key, s->iv, &body[1], enc_len, dec) != 0) {
        fail(s, "keyinfo decrypt");
        return;
    }
    /* plaintext: sRand[16] || seed[2] (+ PKCS7 padding) */
    const uint8_t *srand = &dec[0];
    const uint8_t *seed = &dec[16];

    uint8_t session_key[16];
    gen_session_key(seed, srand, session_key);
    memcpy(s->key, session_key, 16);
    TRACEF("<- keyinfo reply (%u B)", (unsigned)len);
    TRACE("keyinfo plaintext", dec, 18);
    TRACE("session key", session_key, 16);

    /* getAuthStatus: encrypted empty packet */
    if (send_packet(s, 0x21, 0x35, 0x35, 0x89, 0x03, NULL, 0) != 0) {
        fail(s, "authstatus write failed");
        return;
    }
    set_stage(s, EF_SESS_AUTHSTATUS);
}

static void hex_upper(const uint8_t *in, size_t n, char *out)
{
    static const char *H = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 0x0F];
    }
}

static void send_auto_auth(ef_session_t *s)
{
    char concat[64];
    int n = snprintf(concat, sizeof(concat), "%s%s", s->user_id, s->sn);
    if (n <= 0 || n >= (int)sizeof(concat)) {
        fail(s, "auth id too long");
        return;
    }
    uint8_t digest[16];
    ef_md5((const uint8_t *)concat, (size_t)n, digest);

    char token[32];
    hex_upper(digest, 16, token);
    TRACEF("-> auto-auth: md5(user_id[%d] + sn '%s')", (int)strlen(s->user_id), s->sn);
    TRACE("auth token", (const uint8_t *)token, sizeof(token));

    if (send_packet(s, 0x21, 0x35, 0x35, 0x86, 0x03,
                    (const uint8_t *)token, sizeof(token)) != 0) {
        fail(s, "auth write failed");
        return;
    }
    set_stage(s, EF_SESS_AUTH);
}

/* ------------------------------------------------------------------ */
/* post-auth: time sync + telemetry                                    */
/* ------------------------------------------------------------------ */

static void tz_payload(uint8_t out[6])
{
    uint32_t now = (uint32_t)time(NULL);
    out[0] = (uint8_t)(now & 0xFF);
    out[1] = (uint8_t)((now >> 8) & 0xFF);
    out[2] = (uint8_t)((now >> 16) & 0xFF);
    out[3] = (uint8_t)((now >> 24) & 0xFF);
    out[4] = 0;   /* tz hours  (UTC) */
    out[5] = 0;   /* tz minutes */
}

static void send_time_sync(ef_session_t *s)
{
    s->last_time_sync_us = esp_timer_get_time();

    /* SysUTCSync protobuf: field 1 (varint) = unix seconds */
    uint8_t pb[8];
    size_t o = 0;
    pb[o++] = 0x08;
    uint32_t t = (uint32_t)time(NULL);
    do {
        uint8_t b = t & 0x7F;
        t >>= 7;
        if (t) b |= 0x80;
        pb[o++] = b;
    } while (t);
    send_packet(s, 0x21, 0x0B, 0x01, 0x55, 0x13, pb, o);

    uint8_t rtc[6];
    tz_payload(rtc);
    send_packet(s, 0x21, 0x35, 0x01, 0x52, 0x03, rtc, sizeof(rtc)); /* RET_TIME */
    send_packet(s, 0x21, 0x35, 0x01, 0x53, 0x03, rtc, sizeof(rtc)); /* CHECK_TIME */
}

static void on_inner_packet(ef_session_t *s, const uint8_t *body, size_t len)
{
    ef_packet_t *pkt = &s->sc_pkt;
    if (ef_packet_parse(body, len, pkt) != 0) {
        TRACEF("inner packet parse failed (%u B)", (unsigned)len);
        TRACE("bad inner", body, len < 32 ? len : 32);
        return;   /* not a packet we can read; ignore */
    }
    TRACEF("<- pkt src=0x%02x dst=0x%02x set=0x%02x id=0x%02x plen=%u",
           pkt->src, pkt->dst, pkt->cmd_set, pkt->cmd_id,
           (unsigned)pkt->payload_len);

    if (s->state == EF_SESS_AUTH) {
        bool auth_reply = (pkt->src == 0x35 && pkt->cmd_set == 0x35 &&
                           pkt->cmd_id == 0x86);
        if (auth_reply) {
            if (pkt->payload_len >= 1 && pkt->payload[0] != 0x00) {
                char m[48];
                snprintf(m, sizeof(m), "auth rejected (code %u)", pkt->payload[0]);
                fail(s, m);
                return;
            }
            ESP_LOGI(TAG, "authenticated");
        } else {
            ESP_LOGI(TAG, "authenticated (first data packet)");
        }
        s->state = EF_SESS_READY;
        send_time_sync(s);
        /* fall through to also process a data packet */
    }

    if (pkt->src == 0x02 && pkt->cmd_set == 0xFE && pkt->cmd_id == 0x15) {
        int m = ef_proto_apply_display(pkt->payload, pkt->payload_len, &s->st);
        if (m > 0 && s->on_state) {
            s->on_state(&s->st, s->state_user);
        }
    } else if (pkt->src == 0x35 && pkt->cmd_set == 0x01 && pkt->cmd_id == 0x52 &&
               pkt->payload_len == 0) {
        ESP_LOGI(TAG, "device requested time");
        send_time_sync(s);
    }
}

/* ------------------------------------------------------------------ */
/* frame dispatch                                                      */
/* ------------------------------------------------------------------ */

static void frame_cb(const uint8_t *body, size_t len, uint8_t frame_type,
                     void *user)
{
    ef_session_t *s = user;
    if (s->state == EF_SESS_ERROR) {
        return;
    }

    if (frame_type == 0) {
        /* unencrypted COMMAND frame: only used in the first two steps */
        if (s->state == EF_SESS_PUBKEY) {
            on_pubkey_reply(s, body, len);
        } else if (s->state == EF_SESS_KEYINFO) {
            on_keyinfo_reply(s, body, len);
        }
        return;
    }

    /* encrypted PROTOCOL frame */
    if (!s->have_key) {
        return;
    }
    size_t plen = ef_frame_decrypt(body, len, s->key, s->iv,
                                   s->sc_plain, sizeof(s->sc_plain));
    if (plen == 0) {
        return;
    }
    if (s->state == EF_SESS_AUTHSTATUS) {
        /* content not needed; advance to auth */
        send_auto_auth(s);
        return;
    }
    on_inner_packet(s, s->sc_plain, plen);
}

/* ------------------------------------------------------------------ */
/* public                                                             */
/* ------------------------------------------------------------------ */

ef_session_t *ef_session_create(void)
{
    return calloc(1, sizeof(ef_session_t));
}

int ef_session_begin(ef_session_t *s,
                     const char *sn, const char *user_id, int encrypt_type,
                     int low_battery_pct,
                     ef_write_fn write, void *write_user,
                     ef_state_fn on_state, void *state_user)
{
    memset(s, 0, sizeof(*s));
    strlcpy(s->sn, sn ? sn : "", sizeof(s->sn));
    strlcpy(s->user_id, user_id ? user_id : "", sizeof(s->user_id));
    s->encrypt_type = encrypt_type;
    s->low_battery_pct = low_battery_pct;
    s->write = write;
    s->write_user = write_user;
    s->on_state = on_state;
    s->state_user = state_user;

    s->st.soc_low_pct = low_battery_pct;
    s->st.minutes_remaining = ECOFLOW_UNKNOWN_I;
    s->st.minutes_to_full = ECOFLOW_UNKNOWN_I;
    s->st.ac_in_watts = ECOFLOW_UNKNOWN_F;
    s->st.ac_out_watts = ECOFLOW_UNKNOWN_F;
    strlcpy(s->st.serial, s->sn, sizeof(s->st.serial));

    /* River 3 model / capacity from the serial prefix. */
    if (!strncmp(s->sn, "R651", 4)) {
        strlcpy(s->st.model, "River 3 (245Wh)", sizeof(s->st.model));
        s->st.design_capacity_wh = 245;
    } else if (!strncmp(s->sn, "R653", 4)) {
        strlcpy(s->st.model, "River 3 (230Wh)", sizeof(s->st.model));
        s->st.design_capacity_wh = 230;
    } else if (!strncmp(s->sn, "R654", 4)) {
        strlcpy(s->st.model, "River 3 UPS (230Wh)", sizeof(s->st.model));
        s->st.design_capacity_wh = 230;
    } else if (!strncmp(s->sn, "R655", 4)) {
        strlcpy(s->st.model, "River 3 UPS (245Wh)", sizeof(s->st.model));
        s->st.design_capacity_wh = 245;
    }

    ef_reasm_reset(&s->reasm);

    if (encrypt_type != 7) {
        ESP_LOGW(TAG, "encrypt_type %d not supported (expected 7)", encrypt_type);
    }
    if (s->user_id[0] == '\0') {
        fail(s, "no EcoFlow user_id configured");
        return -1;
    }
    return step_send_pubkey(s);
}

void ef_session_feed(ef_session_t *s, const uint8_t *data, size_t len)
{
    if (s->state == EF_SESS_ERROR) {
        return;
    }
    ef_reasm_feed(&s->reasm, data, len, frame_cb, s);
}

void ef_session_tick(ef_session_t *s)
{
    if (s->state == EF_SESS_ERROR) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s->state != EF_SESS_READY && now > s->stage_deadline) {
        fail(s, "handshake stage timeout");
        return;
    }
    if (s->state == EF_SESS_READY &&
        now - s->last_time_sync_us > 30 * 60 * 1000000LL) {
        send_time_sync(s);
    }
}

void ef_session_reset(ef_session_t *s)
{
    ef_sess_state_t keep_err = s->state;
    const char *tag = (keep_err == EF_SESS_ERROR) ? s->err : "reset";
    ESP_LOGI(TAG, "session reset (%s)", tag);
    ef_reasm_reset(&s->reasm);
    s->state = EF_SESS_IDLE;
    s->have_key = false;
}

ef_sess_state_t ef_session_state(ef_session_t *s) { return s->state; }
const char *ef_session_error(ef_session_t *s) { return s->err; }
