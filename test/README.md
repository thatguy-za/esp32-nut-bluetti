# Host tests

Portable-logic tests that run without ESP-IDF or hardware. `make` builds and
runs all eight C suites:

| Suite | Covers |
| --- | --- |
| `ef_logic_test` | CRC-8 / CRC-16, inner-packet build+parse, XOR payload deobfuscation, protobuf field scanner, frame reassembly across split notifications |
| `ef_river3_test` | **5 real `DisplayPropertyUpload` packets captured from a River 3 UPS** (via [`rabits/ha-ef-ble`](https://github.com/rabits/ha-ef-ble)), decoded through the actual C code and checked against ha-ef-ble's documented values (SOC 75 %, AC-in 43.76 W, load 56 W, discharging 2 W, 33 °C, backup mode, runtime 3807/3827 min) |
| `auth_test` | admin password hashing + verification and Basic-auth header parsing: salted SHA-256 (checked against the SHA-256("abc") vector), wrong/empty/wrong-case rejection, salt uniqueness, no length or character restrictions, passwords containing `:` |
| `ipv4_test` | the static-addressing dotted-quad validator: valid addresses, and rejection of the lenient forms `esp_ip4addr_aton()` accepts (`192.168.1`, `0xC0.0xA8.1.1`), whitespace, signs and out-of-range octets |
| `lb_test` | the low-battery decision: `LB` on the charge threshold **or** the runtime threshold, the heavy-load case percentage alone misses, unknown runtime, and a disabled runtime threshold |
| `bt_frame_test` | BLUETTI wire format: CRC-16/Modbus against its known vector, the read (0x03) and write-single-register (0x06) request layouts, response validation and corruption detection, the framing decision for read/write/exception replies in the plain reassembler, the `2a2a` checksum, AES padding |
| `ota_gh_test` | GitHub update helpers: version ordering (a string compare would put `0.10.0` before `0.9.0`) and the streaming release scanner, fed at every chunk size from 1 to 40 so no token split is handled correctly only by luck |
| `bt_regs_test` | compiles the real `bt_regs.c`: model identification, the per-field polling plan (with and without the control registers), decode of individual field responses (SOC bounds, power summed across separate reads, mains/charging inference, zero-runtime as unknown, switch fields strictly 0/1, the little-endian-word serial), and the device-control table — name→register lookup and value validation (`charging_mode` allows 0/1/2/4 not 3, ECO timeout 1..4) |
| `config_compat_test` | the forward-compatible NVS loader: pins that `led_gpio` is the last field of `app_config_t` and `led_enabled` sits right before it, and that a v3-length blob loads with the stored fields intact and `led_gpio` at its default — a future field reorder that would silently wipe or misread everyone's config fails here |
| `nut_server_test` | drives the real `nut_server.c` over a loopback socket: `LIST UPS/VAR`, `GET VAR`, `GET UPSDESC/NUMLOGINS`, the `upsmon` primary handshake (`USERNAME`/`PASSWORD`/`LOGIN`/`PRIMARY`), empty `LIST CLIENT/RW`, error replies; plus the optional NUT login — reads stay anonymous, wrong/missing credentials denied on `LOGIN`/`PRIMARY`, and credentials not leaking between connections |

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
