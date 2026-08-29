#pragma once
/*
 * Crypto primitives for the EcoFlow BLE "V2" (encrypt_type 7) handshake:
 *   - CRC-8 (poly 0x07) and CRC-16/ARC, as used in the wire framing
 *   - MD5 (IV / session-key / auth-token derivation)
 *   - AES-128-CBC (session encryption)
 *   - ECDH on secp160r1 (session key agreement)
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t  ef_crc8(const uint8_t *data, size_t len);      /* poly 0x07, init 0x00 */
uint16_t ef_crc16(const uint8_t *data, size_t len);     /* CRC-16/ARC, init 0x0000 */

void ef_md5(const uint8_t *in, size_t len, uint8_t out[16]);

/* CBC, PKCS#7-agnostic: caller supplies block-aligned buffers. `iv` is not
 * modified. `out` may equal `in`. Returns 0 on success. */
int ef_aes128_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                          const uint8_t *in, size_t len, uint8_t *out);
int ef_aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                          const uint8_t *in, size_t len, uint8_t *out);

#define EF_ECC_PUB_LEN    40   /* X||Y, 20 bytes each */
#define EF_ECC_PRIV_LEN   21   /* secp160r1 order is 161 bits */
#define EF_ECC_SECRET_LEN 20   /* shared X coordinate */

/* Register the RNG micro-ecc uses. Call once at startup. */
void ef_crypto_init(void);

int ef_ecdh_keygen(uint8_t priv[EF_ECC_PRIV_LEN], uint8_t pub[EF_ECC_PUB_LEN]);
int ef_ecdh_shared(const uint8_t priv[EF_ECC_PRIV_LEN],
                   const uint8_t peer_pub[EF_ECC_PUB_LEN],
                   uint8_t secret[EF_ECC_SECRET_LEN]);

#ifdef __cplusplus
}
#endif
