# BLUETTI BLE protocol notes

Everything here comes from reading
[`Patrick762/bluetti-bt-lib`](https://github.com/Patrick762/bluetti-bt-lib)
(which in turn builds on
[`nhurman/bluetti_mqtt`](https://github.com/nhurman/bluetti_mqtt) and
[`warhammerkid/bluetti_mqtt`](https://github.com/warhammerkid/bluetti_mqtt)).
The register map is from
[PR #89](https://github.com/Patrick762/bluetti-bt-lib/pull/89), merged upstream
on 2026-08-29 and verified line-by-line against what is implemented here: every
address below, the 6-word swapped model string, SOC bounded 0-100, and register
104 carrying **minutes** (upstream scales it by 1/60 to display hours). All of
it is implemented here — `bt_crypto.c`, `bt_session.c`,
`bt_regs.c` — and **none of it has been confirmed against an Elite 10**. If a
reading looks wrong, probe mode (below) shows the raw frames.

## Transport

| | |
| --- | --- |
| Notify characteristic | `0000ff01-0000-1000-8000-00805f9b34fb` |
| Write characteristic | `0000ff02-0000-1000-8000-00805f9b34fb` |
| Payload | Modbus RTU |
| CRC | CRC-16/Modbus, appended **little-endian** |
| Read | function `0x03`, then `!HH` (big-endian start address, quantity) |

Note these are **not** the Nordic UART UUIDs used by EcoFlow — they're
`ff01`/`ff02` in the 16-bit space.

`bluetti-bt-lib` writes to `ff02` with `bleak`'s default (`response=None`), which
picks **write-with-response** whenever the characteristic advertises the `WRITE`
property and falls back to write-without-response otherwise. This firmware reads
`ff02`'s properties during discovery and does the same. It matters because a unit
that accepts only one write type on `ff02` drops the other silently — and the
first thing written is the challenge reply, so getting it wrong stalls the
handshake before it visibly starts. Probe mode's characteristic dump shows which
properties `ff02` has (`W` = write, `w` = write-no-response).

## Encryption (V2 devices, which includes the Elite series)

Newer firmware negotiates an encrypted channel before it will answer Modbus.
This is the `2A 2A` traffic people saw and assumed was unsolved — it isn't.

Handshake frame, before a session key exists:

```
2a 2a | body | checksum
|       |      |> 2-byte sum of body (hexsum)
|       |------->
|--------------> KEX magic ("**")
```

`body[0]` is the message type:

| Value | Meaning |
| --- | --- |
| 1 | CHALLENGE |
| 3 | CHALLENGE_ACCEPTED |
| 4 | PEER_PUBKEY |
| 6 | PUBKEY_ACCEPTED |

Crypto used:

- **ECDH on secp256r1 (P-256)** for the session key
- **AES-128-CBC** for the payload
- **ECDSA** signature verification on the device's messages
- SHA-256 for the digests

Critically, the keys are **fixed constants baked into the app**, not per-device
secrets — so no Bluetooth HCI capture is needed:

```
LOCAL_AES_KEY  459FC535808941F17091E0993EE3E93D
PRIVATE_KEY_L1 4F19A16E3E87BDD9BD24D3E5495B88041511943CBC8B969ADE9641D0F56AF337
PUBLIC_KEY_K2  3059...0004A73ABF5D2232C8C1C72E68304343C272495E3A8FD6F30EA96DE2F4B3CE60B251
               EE21AC667CF8A71E18B46B664EAEFFE3C489F24F695B6411DB7E22CCC85A8594
```

Once the exchange completes, ordinary Modbus frames are simply wrapped in the
AES layer.

ESP-IDF already ships everything required — mbedtls provides secp256r1 ECDH,
AES-CBC, SHA-256 and ECDSA verification. (The EcoFlow build vendored micro-ecc
only because it needed secp160r1, which mbedtls omits. That's not a problem
here.)

## Register map

The addresses below are the **V2 portable** map, shared by every supported
model. From [Patrick762/bluetti-bt-lib#89](https://github.com/Patrick762/bluetti-bt-lib/pull/89)
("Add EL10 device"), merged upstream on 2026-08-29, plus the `BaseDeviceV2`
common fields — and cross-checked against the other V2 device definitions,
which use the same addresses.

**Only the Elite 10 (and the byte-identical EL100V2) are decoded in full.**
Other V2 models put the same *fields* at these addresses but scale a few
differently — `bluetti-bt-lib` gives each model its own `DecimalField`
parameters. Register 104 (runtime) is in minutes on the EL10 (`scale 0,
×1/60`), tenths of an hour on the AC70 and Handsfree 1 (`scale 1`), and
something else again on the EL30V2 (`scale 4, ×167`). Register 1314 (AC input
voltage) is `÷10` on the EL10 but a plain integer on the AC60. Rather than
reproduce every model's quirks unverified, `bt_regs.c` decodes the Elite 10
scaling for an Elite 10, and for anything else publishes only the fields that
are identical across every V2 model — SOC (102) and the four power registers
(140/142/144/146), all plain `uint`.

The model comes from register 110, not from configuration.

Four V2 models are **not** supported at all: `EP600`, `EP760`, `EP800` and
`EP2000` are grid/PV systems whose fields (three-phase grid, PV strings) share
none of these addresses. The V1-protocol models are a different framing
altogether.

| Register | Field | EL10 type / scale |
| --- | --- | --- |
| 102 | Battery SOC | uint, 0..100 |
| 104 | Time remaining | uint minutes (upstream ×1/60 → hours) |
| 110 | Device type | swapped string, 6 words |
| 116 | Device serial | 4 words, least-significant first |
| 140 | DC output power | uint, W |
| 142 | AC output power | uint, W |
| 144 | DC input power | uint, W |
| 146 | AC input power | uint, W |
| 1314 | AC input voltage | uint ÷10, V |
| 1315 | AC input current | uint ÷10, A |
| 1511 | AC output voltage | uint ÷10, V |
| 2011 | AC output switch | bool — **only** 0 or 1; anything else = absent |
| 2012 | DC output switch | bool — as above |

The remaining EL10 registers upstream lists (2014–6175: ECO settings, charging
mode, display timeout, BMS version) are controls this bridge does not surface.

Advertised name matches `^EL10(\d+)$` or `^EL100V2(\d+)$`. `EL10` is a prefix
of `EL100V2`; the digit-after-the-name check keeps them apart (both resolve to
the same decoder here, but the check still matters). Connecting is by BLE
address only, which sidesteps the ambiguity for targeting.

### Mapping onto NUT

Enough is here for a useful UPS. `battery.charge` ← 102, `battery.runtime` ←
104, `ups.realpower` ← 142, `input.realpower.ac` ← 146, and `ups.status`
`OL`/`OB` from whether AC input power (146) or input voltage (1314) is
non-zero.

What the map does not give, and what was done about it:

- **battery voltage** and **pack temperature** — no register listed, so those
  NUT variables are not published at all rather than guessed
- **explicit charging flag** — inferred in `bt_regs.c` from mains present and
  charge below 100%
- **mains presence** — inferred from AC input power (146) or line voltage
  (1314) being non-zero; voltage is checked as well because a plugged-in but
  idle unit reads 0 W
- **design capacity** — the Elite 10 is 128 Wh, a constant
- **continuous AC rating** — 200 W for this unit; a UI setting, not hardcoded

## Encryption is not assumed

Not every V2 unit encrypts. `bluetti-bt-lib`'s `recognize_device` tries an
encrypted connection first and, when it times out, retries in plain Modbus —
the "encrypted" flag it ends up storing is just whichever attempt answered.

This firmware does the same in one connection rather than two: it subscribes,
waits for the device to open the `2a2a` exchange, and if nothing arrives within
15 seconds (the reference's own encrypted-attempt timeout), `bt_session_use_plain()`
switches the session to unencrypted Modbus — register reads then go out as bare
PDUs and responses are parsed straight off the wire. A single stray handshake
byte cancels the fallback.

## Polling

`bluetti-bt-lib` reads **one field per Modbus transaction** — `get_polling_registers`
returns one `ReadableRegisters(addr, size)` per field, and the reader loops
`write → wait for one notification → next`. This firmware matches that: a
per-field plan (`bt_regs_plan`), one request outstanding at a time, each field's
response decoded on its own. Reading fields individually rather than in ranges
avoids any question of whether the unit answers a range spanning registers it
does not implement.

If five reads in a row get no reply while the BLE link is still up, the session
has wedged; the bridge drops the connection and reconnects, which re-runs the
handshake. The reference gets this for free by reconnecting every poll cycle.

## Cross-check against the reference

`bt_session.c` / `bt_crypto.c` were audited line-by-line against
`bluetti_bt_lib/bluetooth/encryption.py` at 0.1.8:

- the fixed `LOCAL_AES_KEY`, `PRIVATE_KEY_L1` and `PUBLIC_KEY_K2` bytes match
- challenge: IV is `MD5(nonce reversed)`, first key is `IV XOR LOCAL_AES_KEY`,
  reply body `0204 || IV[8:12]`, sent **unencrypted**
- peer key: verify `sig` over `pubkey || IV` against K2; reply `0580 || our
  pubkey || our sig(pubkey || IV) under L1`, AES-wrapped with the handshake key
- session key: raw ECDH X coordinate on secp256r1, used directly as AES-256
- frame wrapping: `len_be16` then, when secure, a 4-byte seed whose `MD5` is the
  per-message IV; zero padding to the AES block

## Probe mode

Enable it when a value looks wrong or the link never reaches `READY`.
In the Logs tab:

- `ff01`/`ff02` present → the transport above is right.
- Notifications starting `2a 2a` → the encrypted handshake, which
  `bt_session.c` answers. If it repeats without ever reaching `READY`, the
  fixed keys or the challenge derivation are wrong for this firmware.
- Plain Modbus responses (any slave id, `0x03`, byte count, data, CRC) → the
  unit is unencrypted; the fallback path handles these.
- Connect then immediate disconnect with no traffic → the device gave up
  waiting for a handshake response.
