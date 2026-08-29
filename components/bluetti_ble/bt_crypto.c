#include "bt_crypto.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "mbedtls/aes.h"
#include "mbedtls/md5.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/bignum.h"

static const char *TAG = "bt_crypto";

/* Fixed vendor keys. These live in the BLUETTI app, are the same for every
 * device, and are what make a pairing-free connection possible. */
static const uint8_t PRIVATE_KEY_L1[32] = {
    0x4F,0x19,0xA1,0x6E,0x3E,0x87,0xBD,0xD9,0xBD,0x24,0xD3,0xE5,0x49,0x5B,0x88,0x04,
    0x15,0x11,0x94,0x3C,0xBC,0x8B,0x96,0x9A,0xDE,0x96,0x41,0xD0,0xF5,0x6A,0xF3,0x37,
};
/* K2 as the raw uncompressed point (X||Y), i.e. the DER minus its prefix. */
static const uint8_t PUBLIC_KEY_K2[64] = {
    0xA7,0x3A,0xBF,0x5D,0x22,0x32,0xC8,0xC1,0xC7,0x2E,0x68,0x30,0x43,0x43,0xC2,0x72,
    0x49,0x5E,0x3A,0x8F,0xD6,0xF3,0x0E,0xA9,0x6D,0xE2,0xF4,0xB3,0xCE,0x60,0xB2,0x51,
    0xEE,0x21,0xAC,0x66,0x7C,0xF8,0xA7,0x1E,0x18,0xB4,0x6B,0x66,0x4E,0xAE,0xFF,0xE3,
    0xC4,0x89,0xF2,0x4F,0x69,0x5B,0x64,0x11,0xDB,0x7E,0x22,0xCC,0xC8,0x5A,0x85,0x94,
};

struct bt_keypair {
    mbedtls_ecp_group grp;
    mbedtls_mpi       d;      /* private scalar */
    mbedtls_ecp_point Q;      /* public point   */
};

/* mbedtls wants an RNG callback; the hardware RNG is fine here. */
static int rng_cb(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

void bt_random(uint8_t *out, size_t len)
{
    esp_fill_random(out, len);
}

void bt_crypto_init(void) { /* nothing global to set up */ }

/* ------------------------------------------------------------------ */

uint16_t bt_crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

void bt_md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    mbedtls_md5_context c;
    mbedtls_md5_init(&c);
    mbedtls_md5_starts(&c);
    mbedtls_md5_update(&c, data, len);
    mbedtls_md5_finish(&c, out);
    mbedtls_md5_free(&c);
}

static int aes_cbc(const uint8_t *key, size_t key_len, const uint8_t iv[16],
                   const uint8_t *in, uint8_t *out, size_t len, int mode)
{
    if (len % BT_AES_BLOCK != 0) {
        ESP_LOGE(TAG, "AES length %u not block aligned", (unsigned)len);
        return -1;
    }
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    int rc = (mode == MBEDTLS_AES_ENCRYPT)
        ? mbedtls_aes_setkey_enc(&a, key, key_len * 8)
        : mbedtls_aes_setkey_dec(&a, key, key_len * 8);
    if (rc == 0) {
        uint8_t iv_copy[16];               /* mbedtls mutates the IV */
        memcpy(iv_copy, iv, sizeof(iv_copy));
        rc = mbedtls_aes_crypt_cbc(&a, mode, len, iv_copy, in, out);
    }
    mbedtls_aes_free(&a);
    return rc;
}

int bt_aes_cbc_encrypt(const uint8_t *key, size_t key_len, const uint8_t iv[16],
                       const uint8_t *in, uint8_t *out, size_t len)
{
    return aes_cbc(key, key_len, iv, in, out, len, MBEDTLS_AES_ENCRYPT);
}

int bt_aes_cbc_decrypt(const uint8_t *key, size_t key_len, const uint8_t iv[16],
                       const uint8_t *in, uint8_t *out, size_t len)
{
    return aes_cbc(key, key_len, iv, in, out, len, MBEDTLS_AES_DECRYPT);
}

/* ------------------------------------------------------------------ */
/* ECDSA                                                               */
/* ------------------------------------------------------------------ */

static void sha256(const uint8_t *msg, size_t len, uint8_t out[32]);

int bt_ecdsa_verify_k2(const uint8_t *msg, size_t msg_len,
                       const uint8_t sig[BT_P256_SIGLEN])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi r, s;
    uint8_t digest[32];
    int rc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    sha256(msg, msg_len, digest);

    /* Load K2 as an uncompressed point: mbedtls expects the 0x04 prefix. */
    uint8_t pub[1 + BT_P256_PUBLEN];
    pub[0] = 0x04;
    memcpy(pub + 1, PUBLIC_KEY_K2, BT_P256_PUBLEN);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &Q, pub, sizeof(pub));
    if (rc == 0) rc = mbedtls_mpi_read_binary(&r, sig, 32);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&s, sig + 32, 32);
    if (rc == 0) rc = mbedtls_ecdsa_verify(&grp, digest, sizeof(digest), &Q, &r, &s);

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    if (rc != 0) {
        ESP_LOGE(TAG, "ECDSA verify failed: -0x%04x", -rc);
    }
    return rc;
}

int bt_ecdsa_sign_l1(const uint8_t *msg, size_t msg_len,
                     uint8_t sig[BT_P256_SIGLEN])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, s;
    uint8_t digest[32];
    int rc;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    sha256(msg, msg_len, digest);

    rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&d, PRIVATE_KEY_L1,
                                              sizeof(PRIVATE_KEY_L1));
    if (rc == 0) rc = mbedtls_ecdsa_sign(&grp, &r, &s, &d, digest,
                                         sizeof(digest), rng_cb, NULL);
    /* Raw r||s, each left-padded to 32 bytes. */
    if (rc == 0) rc = mbedtls_mpi_write_binary(&r, sig, 32);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&s, sig + 32, 32);

    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_group_free(&grp);
    if (rc != 0) {
        ESP_LOGE(TAG, "ECDSA sign failed: -0x%04x", -rc);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Ephemeral keypair + ECDH                                            */
/* ------------------------------------------------------------------ */

bt_keypair_t *bt_keypair_new(void)
{
    bt_keypair_t *kp = calloc(1, sizeof(*kp));
    if (!kp) {
        return NULL;
    }
    mbedtls_ecp_group_init(&kp->grp);
    mbedtls_mpi_init(&kp->d);
    mbedtls_ecp_point_init(&kp->Q);

    int rc = mbedtls_ecp_group_load(&kp->grp, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) {
        rc = mbedtls_ecp_gen_keypair(&kp->grp, &kp->d, &kp->Q, rng_cb, NULL);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "keypair generation failed: -0x%04x", -rc);
        bt_keypair_free(kp);
        return NULL;
    }
    return kp;
}

void bt_keypair_free(bt_keypair_t *kp)
{
    if (!kp) {
        return;
    }
    mbedtls_ecp_point_free(&kp->Q);
    mbedtls_mpi_free(&kp->d);
    mbedtls_ecp_group_free(&kp->grp);
    free(kp);
}

int bt_keypair_public(const bt_keypair_t *kp, uint8_t out[BT_P256_PUBLEN])
{
    uint8_t buf[1 + BT_P256_PUBLEN];
    size_t olen = 0;
    int rc = mbedtls_ecp_point_write_binary(&kp->grp, &kp->Q,
                                            MBEDTLS_ECP_PF_UNCOMPRESSED,
                                            &olen, buf, sizeof(buf));
    if (rc != 0 || olen != sizeof(buf) || buf[0] != 0x04) {
        ESP_LOGE(TAG, "public key export failed (rc=-0x%04x olen=%u)",
                 -rc, (unsigned)olen);
        return -1;
    }
    memcpy(out, buf + 1, BT_P256_PUBLEN);   /* drop the 0x04 prefix */
    return 0;
}

int bt_ecdh_shared(const bt_keypair_t *kp,
                   const uint8_t peer_pub[BT_P256_PUBLEN],
                   uint8_t out[BT_P256_COORD])
{
    mbedtls_ecp_point peer;
    mbedtls_mpi z;
    uint8_t buf[1 + BT_P256_PUBLEN];
    int rc;

    mbedtls_ecp_point_init(&peer);
    mbedtls_mpi_init(&z);

    buf[0] = 0x04;
    memcpy(buf + 1, peer_pub, BT_P256_PUBLEN);

    /* The group is const in practice but mbedtls takes it non-const. */
    mbedtls_ecp_group *grp = (mbedtls_ecp_group *)&kp->grp;

    rc = mbedtls_ecp_point_read_binary(grp, &peer, buf, sizeof(buf));
    if (rc == 0) rc = mbedtls_ecp_check_pubkey(grp, &peer);
    if (rc == 0) rc = mbedtls_ecdh_compute_shared(grp, &z, &peer, &kp->d,
                                                  rng_cb, NULL);
    /* The shared secret is the X coordinate, and doubles as an AES-256 key. */
    if (rc == 0) rc = mbedtls_mpi_write_binary(&z, out, BT_P256_COORD);

    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&peer);
    if (rc != 0) {
        ESP_LOGE(TAG, "ECDH failed: -0x%04x", -rc);
    }
    return rc;
}

/* ------------------------------------------------------------------ */

#include "mbedtls/sha256.h"

static void sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, msg, len);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}
