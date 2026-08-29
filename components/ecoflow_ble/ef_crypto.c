#include "ef_crypto.h"

#include <string.h>

#include "esp_random.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"

#include "uECC.h"

/* ------------------------------------------------------------------ */
/* CRC                                                                 */
/* ------------------------------------------------------------------ */

uint8_t ef_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t ef_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* MD5                                                                 */
/* ------------------------------------------------------------------ */

void ef_md5(const uint8_t *in, size_t len, uint8_t out[16])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    mbedtls_md(info, in, len, out);
}

/* ------------------------------------------------------------------ */
/* AES-128-CBC                                                         */
/* ------------------------------------------------------------------ */

int ef_aes128_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                          const uint8_t *in, size_t len, uint8_t *out)
{
    if (len % 16) {
        return -1;
    }
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    int rc = mbedtls_aes_setkey_enc(&ctx, key, 128);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, len, iv_copy, in, out);
    }
    mbedtls_aes_free(&ctx);
    return rc;
}

int ef_aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                          const uint8_t *in, size_t len, uint8_t *out)
{
    if (len % 16) {
        return -1;
    }
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    int rc = mbedtls_aes_setkey_dec(&ctx, key, 128);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len, iv_copy, in, out);
    }
    mbedtls_aes_free(&ctx);
    return rc;
}

/* ------------------------------------------------------------------ */
/* ECDH secp160r1                                                      */
/* ------------------------------------------------------------------ */

static int ef_rng(uint8_t *dest, unsigned size)
{
    esp_fill_random(dest, size);
    return 1;
}

void ef_crypto_init(void)
{
    uECC_set_rng(&ef_rng);
}

int ef_ecdh_keygen(uint8_t priv[EF_ECC_PRIV_LEN], uint8_t pub[EF_ECC_PUB_LEN])
{
    /* micro-ecc returns 1 on success. */
    return uECC_make_key(pub, priv, uECC_secp160r1()) == 1 ? 0 : -1;
}

int ef_ecdh_shared(const uint8_t priv[EF_ECC_PRIV_LEN],
                   const uint8_t peer_pub[EF_ECC_PUB_LEN],
                   uint8_t secret[EF_ECC_SECRET_LEN])
{
    return uECC_shared_secret(peer_pub, priv, secret, uECC_secp160r1()) == 1 ? 0 : -1;
}
