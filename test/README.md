# Host tests

Portable-logic tests that run without ESP-IDF or hardware. `make` builds and
runs all nine C suites:

| Suite | Covers |
| --- | --- |
| `bt_frame_test` | Bluetti wire format: CRC-16/Modbus against its known vector, the read (0x03) and write-single-register (0x06) request layouts, response validation and corruption detection, the framing decision for read/write/exception replies in the plain reassembler, the `2a2a` checksum and its routing rule, AES padding |
| `bt_regs_test` | compiles the real `bt_regs.c`: model identification and per-model control masks (`EL10`/`EL100V2` and `AC60`/`AC60P` told apart by the digit-tail rule), per-model battery capacity, the polling plan omitting controls a model lacks, per-field decode gated on the mask (an AC60 does not decode charging mode), `soc_min`/`soc_max` range decode and 0–100 bounds, and the control table — name→register, bool/enum/range validation |
| `nut_server_test` | drives the real `nut_server.c` over a loopback socket: `LIST UPS/VAR`, `GET VAR`, `GET UPSDESC/NUMLOGINS`, the `upsmon` primary handshake (`USERNAME`/`PASSWORD`/`LOGIN`/`PRIMARY`), empty `LIST CLIENT/RW`, error replies, `driver.version` carrying the firmware version; plus the optional NUT login — reads stay anonymous, wrong/missing credentials denied on `LOGIN`/`PRIMARY`, and credentials not leaking between connections |
| `auth_test` | admin password hashing + verification and Basic-auth header parsing: salted SHA-256 (checked against the SHA-256("abc") vector), wrong/empty/wrong-case rejection, salt uniqueness, no length or character restrictions, passwords containing `:` |
| `lb_test` | the low-battery decision: `LB` on the charge threshold **or** the runtime threshold, the heavy-load case percentage alone misses, unknown runtime, and a disabled runtime threshold |
| `ipv4_test` | the static-addressing dotted-quad validator: valid addresses, and rejection of the lenient forms `esp_ip4addr_aton()` accepts (`192.168.1`, `0xC0.0xA8.1.1`), whitespace, signs and out-of-range octets |
| `ota_gh_test` | GitHub update helpers: version ordering (a string compare would put `0.10.0` before `0.9.0`) and the streaming release scanner, fed at every chunk size from 1 to 40 so no token split is handled correctly only by luck |
| `config_compat_test` | the forward-compatible NVS loader: pins the append-only field order (`led_gpio`, `controls_enabled`, `battery_wh`, `log_level`, the fallback AP, the Proxmox block) and that the Proxmox block is last, that short v3/v6/v7-length blobs load with the stored fields intact and the newer ones at their defaults — a v7 device upgrading comes up with Proxmox shutdown **off and in dry run** — and that a v8 blob is accepted so its reshaped Proxmox block can be reset rather than misread |
| `pve_engine_test` | drives the real `pve_engine.c` through outages minute by minute: the countdown on the bridge's own clock, per-host thresholds off one shared edge (a 10-minute host fires while the 30-minute host keeps counting), per-host fire-once latches released together once the mains is back, a mains blip inside the latch not re-firing, fail-safe on an unknown reading (never starts a countdown, never releases a latch), a running countdown surviving a lost link, the charge trigger needing a live reading, disabled hosts and triggers, and `t = 0` being a valid start — the zero-sentinel bug this suite caught |

## Not covered here

Anything that needs the radio or a real unit: the BLE handshake timing, the
`ff02` write-type behaviour, and whether a control **write** is actually
honoured by the power station. The Proxmox side's runtime half — the pinned
TLS client, the API calls and the shutdown sequence — needs a real PVE host;
only the decision is tested. The register addresses and scaling for models
other than the Elite 10 Mini are also unproven — these tests pin the
arithmetic and the per-model gating, not the map itself.
