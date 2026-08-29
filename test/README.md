# Host tests

Portable-logic tests that run without ESP-IDF or hardware. `make` builds and
runs all five C suites:

| Suite | Covers |
| --- | --- |
| `ef_logic_test` | CRC-8 / CRC-16, inner-packet build+parse, XOR payload deobfuscation, protobuf field scanner, frame reassembly across split notifications |
| `ef_river3_test` | **5 real `DisplayPropertyUpload` packets captured from a River 3 UPS** (via [`rabits/ha-ef-ble`](https://github.com/rabits/ha-ef-ble)), decoded through the actual C code and checked against ha-ef-ble's documented values (SOC 75 %, AC-in 43.76 W, load 56 W, discharging 2 W, 33 °C, backup mode, runtime 3807/3827 min) |
| `auth_test` | admin password hashing + verification and Basic-auth header parsing: salted SHA-256 (checked against the SHA-256("abc") vector), wrong/empty/wrong-case rejection, salt uniqueness, no length or character restrictions, passwords containing `:` |
| `ipv4_test` | the static-addressing dotted-quad validator: valid addresses, and rejection of the lenient forms `esp_ip4addr_aton()` accepts (`192.168.1`, `0xC0.0xA8.1.1`), whitespace, signs and out-of-range octets |
| `nut_server_test` | drives the real `nut_server.c` over a loopback socket: `LIST UPS/VAR`, `GET VAR`, `GET UPSDESC/NUMLOGINS`, the `upsmon` primary handshake (`USERNAME`/`PASSWORD`/`LOGIN`/`PRIMARY`), empty `LIST CLIENT/RW`, error replies |

## Crypto cross-check (optional)

`make xcheck && pip install ecdsa && python xcheck.py` compares the C crypto
against the python-ecdsa reference the real device interoperates with:

- micro-ecc secp160r1 pubkey + ECDH shared secret == python-ecdsa
- `iv = md5(shared)`, `session_key = md5(keydata16 || srand16)`
- `token = md5(user_id + serial).upper()`

## Not covered here

The end-to-end BLE handshake against a real River 3 (frame timing, the exact
`ecdh_type` byte, keyinfo reply shape, write-with-response behaviour). That
needs hardware.
