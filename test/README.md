# Host tests

Portable-logic tests that run without ESP-IDF or hardware. `make` builds and
runs all eight C suites:

| Suite | Covers |
| --- | --- |
| `bt_frame_test` | Bluetti wire format: CRC-16/Modbus against its known vector, the read (0x03) and write-single-register (0x06) request layouts, response validation and corruption detection, the framing decision for read/write/exception replies in the plain reassembler, the `2a2a` checksum and its routing rule, AES padding |
| `bt_regs_test` | compiles the real `bt_regs.c`: model identification and per-model control masks (`EL10`/`EL100V2` and `AC60`/`AC60P` told apart by the digit-tail rule), per-model battery capacity, the polling plan omitting controls a model lacks, per-field decode gated on the mask (an AC60 does not decode charging mode), `soc_min`/`soc_max` range decode and 0–100 bounds, and the control table — name→register, bool/enum/range validation |
| `nut_server_test` | drives the real `nut_server.c` over a loopback socket: `LIST UPS/VAR`, `GET VAR`, `GET UPSDESC/NUMLOGINS`, the `upsmon` primary handshake (`USERNAME`/`PASSWORD`/`LOGIN`/`PRIMARY`), empty `LIST CLIENT/RW`, error replies, `driver.version` carrying the firmware version; plus the optional NUT login — reads stay anonymous, wrong/missing credentials denied on `LOGIN`/`PRIMARY`, and credentials not leaking between connections |
| `auth_test` | admin password hashing + verification and Basic-auth header parsing: salted SHA-256 (checked against the SHA-256("abc") vector), wrong/empty/wrong-case rejection, salt uniqueness, no length or character restrictions, passwords containing `:` |
| `lb_test` | the low-battery decision: `LB` on the charge threshold **or** the runtime threshold, the heavy-load case percentage alone misses, unknown runtime, and a disabled runtime threshold |
| `ipv4_test` | the static-addressing dotted-quad validator: valid addresses, and rejection of the lenient forms `esp_ip4addr_aton()` accepts (`192.168.1`, `0xC0.0xA8.1.1`), whitespace, signs and out-of-range octets |
| `ota_gh_test` | GitHub update helpers: version ordering (a string compare would put `0.10.0` before `0.9.0`) and the streaming release scanner, fed at every chunk size from 1 to 40 so no token split is handled correctly only by luck |
| `config_compat_test` | the forward-compatible NVS loader: pins the append-only field order (`led_gpio`, `controls_enabled`, `battery_wh`, `log_level`) and that `log_level` is last, and that a short v3-length blob loads with the stored fields intact and the newer ones at their defaults — a future field reorder that would silently wipe or misread everyone's config fails here |

## Not covered here

Anything that needs the radio or a real unit: the BLE handshake timing, the
`ff02` write-type behaviour, and whether a control **write** is actually
honoured by the power station. The register addresses and scaling for models
other than the Elite 10 Mini are also unproven — these tests pin the
arithmetic and the per-model gating, not the map itself.
