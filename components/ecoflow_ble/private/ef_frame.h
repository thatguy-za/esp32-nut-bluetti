#pragma once
/*
 * EcoFlow BLE wire framing (encrypt_type 7).
 *
 * Two nested layers:
 *   outer  EncPacket   0x5A 0x5A | ft<<4 | 0x01 | len16 | body | crc16
 *          - handshake steps 1-2: ft=0 (COMMAND), body = raw payload
 *          - everything after:    ft=1 (PROTOCOL), body = AES-128-CBC(inner)
 *   inner  Packet V2/V3  0xAA | ver | plen16 | crc8 | 0x0d | seq[4] | 00 00 |
 *                        src | dst | [dsrc dst]V3 | cmd_set | cmd_id |
 *                        payload | crc16
 *          - received payloads are XOR-obfuscated with seq[0]
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EF_FRAME_MAX      1200
#define EF_INNER_MAX      1024
#define EF_PAYLOAD_MAX    900

/* ---- inner Packet ------------------------------------------------- */

typedef struct {
    uint8_t  src, dst, cmd_set, cmd_id;
    uint8_t  version;          /* raw version byte, e.g. 0x03 / 0x13 */
    uint8_t  seq[4];
    uint8_t  payload[EF_PAYLOAD_MAX];
    size_t   payload_len;
} ef_packet_t;

/* Serialize an inner Packet into out[] (>= EF_INNER_MAX). Returns length. */
size_t ef_packet_build(uint8_t src, uint8_t dst, uint8_t dsrc, uint8_t ddst,
                       uint8_t cmd_set, uint8_t cmd_id, uint8_t version,
                       const uint8_t *payload, size_t payload_len,
                       uint8_t *out);

/* Parse an inner Packet (starts at 0xAA). De-obfuscates the payload.
 * Returns 0 on success. */
int ef_packet_parse(const uint8_t *data, size_t len, ef_packet_t *pkt);

/* ---- outer EncPacket -------------------------------------------- */

/* Wrap `payload` in an unencrypted COMMAND frame. Returns length. */
size_t ef_enc_simple_encode(const uint8_t *payload, size_t len, uint8_t *out);

/* Wrap `inner` (already an inner Packet) in an encrypted PROTOCOL frame. */
size_t ef_enc_encode_encrypted(const uint8_t *inner, size_t inner_len,
                               const uint8_t key[16], const uint8_t iv[16],
                               uint8_t *out);

/* Streaming reassembly of inbound BLE bytes. */
typedef struct {
    uint8_t buf[EF_FRAME_MAX * 2];
    size_t  len;
} ef_reasm_t;

void ef_reasm_reset(ef_reasm_t *r);

/* Called for each complete, CRC-valid outer frame. `frame_type` is 0 for
 * an unencrypted COMMAND frame; non-zero for a PROTOCOL frame whose
 * `body` is still AES-CBC ciphertext (the caller decrypts). */
typedef void (*ef_frame_cb_t)(const uint8_t *body, size_t len,
                              uint8_t frame_type, void *user);

void ef_reasm_feed(ef_reasm_t *r, const uint8_t *chunk, size_t len,
                   ef_frame_cb_t cb, void *user);

/* Decrypt a PROTOCOL frame body: full AES blocks only, PKCS#7 trimmed if
 * valid. Returns plaintext length, or 0 on failure. */
size_t ef_frame_decrypt(const uint8_t *body, size_t body_len,
                        const uint8_t key[16], const uint8_t iv[16],
                        uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
