#pragma once
/*
 * Crypto primitives for the BLUETTI V2 link, all backed by mbedtls.
 *
 * The key exchange is ECDH on secp256r1 with ECDSA-signed public keys and
 * AES-CBC payloads. The signing keys are fixed constants lifted from the
 * vendor app (see docs/PROTOCOL.md) rather than per-device secrets, so no
 * pairing or capture step is needed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BT_AES_BLOCK   16
#define BT_P256_COORD  32          /* one coordinate                */
#define BT_P256_PUBLEN 64          /* uncompressed X||Y, no 0x04    */
#define BT_P256_SIGLEN 64          /* raw r||s                      */

void bt_crypto_init(void);

/* CRC-16/Modbus (poly 0xA001, init 0xFFFF), as the Modbus layer uses. */
uint16_t bt_crc16_modbus(const uint8_t *data, size_t len);

void bt_md5(const uint8_t *data, size_t len, uint8_t out[16]);

/* AES-CBC. `key_len` is 16 during the handshake and 32 afterwards.
 * `len` must be a multiple of BT_AES_BLOCK. */
int bt_aes_cbc_encrypt(const uint8_t *key, size_t key_len, const uint8_t iv[16],
                       const uint8_t *in, uint8_t *out, size_t len);
int bt_aes_cbc_decrypt(const uint8_t *key, size_t key_len, const uint8_t iv[16],
                       const uint8_t *in, uint8_t *out, size_t len);

/* Verify a raw r||s signature over `msg` against the vendor's fixed
 * public key K2. Returns 0 when the signature is good. */
int bt_ecdsa_verify_k2(const uint8_t *msg, size_t msg_len,
                       const uint8_t sig[BT_P256_SIGLEN]);

/* Sign `msg` with the vendor's fixed private key L1, emitting raw r||s. */
int bt_ecdsa_sign_l1(const uint8_t *msg, size_t msg_len,
                     uint8_t sig[BT_P256_SIGLEN]);

/* Ephemeral secp256r1 keypair for the session. */
typedef struct bt_keypair bt_keypair_t;
bt_keypair_t *bt_keypair_new(void);
void          bt_keypair_free(bt_keypair_t *kp);
int           bt_keypair_public(const bt_keypair_t *kp,
                                uint8_t out[BT_P256_PUBLEN]);

/* ECDH: shared secret is the X coordinate, 32 bytes, used as an AES-256 key. */
int bt_ecdh_shared(const bt_keypair_t *kp,
                   const uint8_t peer_pub[BT_P256_PUBLEN],
                   uint8_t out[BT_P256_COORD]);

void bt_random(uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif
