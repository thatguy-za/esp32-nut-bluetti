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

## Elite 10 register map

From [Patrick762/bluetti-bt-lib#89](https://github.com/Patrick762/bluetti-bt-lib/pull/89)
("Add EL10 device"), merged upstream on 2026-08-29, plus the `BaseDeviceV2`
common fields. Review is not hardware confirmation: as far as is known nobody
has run it against a unit either, so treat the addresses as a well-reviewed
hypothesis rather than fact.

Upstream matches advertised names as `^(…|EL10|EL100V2|…)(\d+)$` — a model name
followed by digits. Note that `EL10` is a prefix of `EL100V2`, a different unit
with a different map, which is why the scanner here requires digits after the
prefix rather than matching on the prefix alone.

| Register | Field | Type / scale |
| --- | --- | --- |
| 102 | Battery SOC | uint, 0..100 |
| 104 | Time remaining | decimal, ×1/60 (→ hours) |
| 110 | Device type | swapped string, 6 words |
| 116 | Device serial | serial-number field |
| 140 | DC output power | uint, W |
| 142 | AC output power | uint, W |
| 144 | DC input power | uint, W |
| 146 | AC input power | uint, W |
| 1314 | AC input voltage | decimal, 1 dp |
| 1315 | AC input current | decimal, 1 dp |
| 1511 | AC output voltage | decimal, 1 dp |
| 2011 | AC output switch | bool |
| 2012 | DC output switch | bool |
| 2014 | DC ECO enable | bool |
| 2015 | DC ECO time mode | enum |
| 2016 | DC ECO minimum power | uint |
| 2017 | AC ECO enable | bool |
| 2018 | AC ECO time mode | enum |
| 2019 | AC ECO minimum power | uint |
| 2020 | Charging mode | enum |
| 2021 | Power lifting | bool |
| 2067 | Display timeout | enum |
| 6175 | BMS version | version field |

Advertised name matches `^EL10(\d+)$`.

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

## Probe mode

The decoder is written, so probe mode is now a diagnostic rather than a first
step: enable it when a value looks wrong or the link never reaches `READY`.
In the Logs tab:

- `ff01`/`ff02` present → the transport above is right.
- Notifications starting `2a 2a` → the encrypted handshake, which
  `bt_session.c` answers. If it repeats without ever reaching `READY`, the
  fixed keys or the challenge derivation are wrong for this firmware.
- Plain Modbus responses (slave id, `0x03`, byte count, data, CRC) → the unit
  is unencrypted; the session layer passes those straight through.
- Connect then immediate disconnect with no traffic → the device gave up
  waiting for a handshake response.
