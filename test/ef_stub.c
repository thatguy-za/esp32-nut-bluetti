/* Host stubs for crypto bits ef_frame.c references but the tests don't use. */
#include "ef_crypto.h"
#include <string.h>
void ef_md5(const uint8_t *i, size_t n, uint8_t o[16]) { (void)i;(void)n; memset(o,0,16); }
int ef_aes128_cbc_encrypt(const uint8_t k[16], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out)
{ (void)k;(void)iv; if (out!=in) memmove(out,in,n); return 0; }
int ef_aes128_cbc_decrypt(const uint8_t k[16], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out)
{ (void)k;(void)iv; if (out!=in) memmove(out,in,n); return 0; }
void ef_crypto_init(void) {}
uint8_t ef_crc8(const uint8_t *d, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1); }
    return c;
}
uint16_t ef_crc16(const uint8_t *d, size_t n)
{
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1); }
    return c;
}
int ef_ecdh_keygen(uint8_t p[21], uint8_t q[40]) { (void)p;(void)q; return 0; }
int ef_ecdh_shared(const uint8_t p[21], const uint8_t q[40], uint8_t s[20]) { (void)p;(void)q; memset(s,0,20); return 0; }
