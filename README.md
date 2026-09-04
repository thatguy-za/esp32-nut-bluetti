# esp32-nut-bluetti

ESP32 firmware that connects to an **BLUETTI** portable power station over
**Bluetooth LE**, reads its battery / AC-input / load state, and exposes it on the
network as a UPS using the **NUT** (Network UPS Tools) TCP protocol.

Point your NAS, server, or Raspberry Pi's `upsmon` at the ESP32's IP on port
`3493` and it will see the BLUETTI as a normal UPS — including `OB`
(on-battery) / `OL` (online) status and battery charge, so hosts can shut
themselves down cleanly on a mains failure.

```
 ┌───────────┐   BLE    ┌─────────┐   TCP/3493 (NUT)   ┌──────────────┐
 │  BLUETTI  │ ───────► │  ESP32  │ ─────────────────► │ upsmon /     │
 │  station  │ ◄─────── │         │ ◄───────────────── │ upsc clients │
 └───────────┘          └─────────┘                    └──────────────┘
```

> ## 🚧 Work in progress — never run against an Elite 10
>
> A fork of [`esp32-nut-ecoflow`](https://github.com/thatguy-za/esp32-nut-ecoflow),
> retargeted to BLUETTI. Releases here start at v0.1.0; anything earlier belongs
> to the EcoFlow project and lives in that repo. Everything vendor-independent — NUT server, Wi-Fi
> setup, admin page, OTA, alerts — carries over and works.
>
> The BLUETTI BLE layer is **fully implemented but unverified**: the key
> exchange, the AES channel, Modbus polling and the Elite 10 register decode
> are all written, and none of it has touched real hardware. Two caveats worth
> knowing before you trust a reading:
>
> - The Elite 10 register map matches
>   [bluetti-bt-lib#89](https://github.com/Patrick762/bluetti-bt-lib/pull/89),
>   merged upstream on 2026-08-29 — but nobody has run it against a unit either.
> - Some NUT values are **inferred**, not measured — see below.
>
> Probe mode is still there for when something does not line up.

## Supported models

**BLUETTI Elite 10**, and the byte-identical **EL100V2**. These two share the
same register list and the same scaling, so one decoder covers both.

Any other **V2**-protocol BLUETTI unit that you point the bridge at will still
connect and report the fields that are identical across every V2 model — **state
of charge** and the **AC/DC power** readings. That is enough for a working UPS:
`ups.status`, `battery.charge`, `ups.realpower`, `ups.load`, and the
`OL`/`OB`/`LB` transitions that drive a shutdown. Model-specific fields
(runtime, line voltage/current, the output switches) stay off, because
[`bluetti-bt-lib`](https://github.com/Patrick762/bluetti-bt-lib) scales several
of them differently per model — an AC60 reports AC-input voltage as a plain
integer where the Elite 10 divides by ten, an AC70's runtime register is in
6-minute units where the Elite 10's is in minutes — and reproducing every
model's quirks unverified would publish confident wrong numbers.

The bridge reads the model from the unit itself (register 110); it is not
something you configure.

### Not supported

| Models | Why |
| --- | --- |
| `EP600` `EP760` `EP800` `EP2000` | V2, but grid/PV systems — three-phase grid and PV-string registers, sharing none of the addresses the portable units use |
| `AC200L` `AC200M` `AC200PL` `AC300` `AC500` `EB3A` `EP500` `EP500P` | The older **V1** protocol, a different framing this firmware does not speak |

### A caveat on all of it

This firmware has not been run against any BLUETTI unit. What it *has* been
checked against, field by field, is
[`bluetti-bt-lib`](https://github.com/Patrick762/bluetti-bt-lib) — the library
behind the Home Assistant integration: the `2a2a` key exchange, the AES framing,
the one-request-at-a-time polling, and the Elite 10 register decode and scaling
all match it. That library's device table lists the Elite 10 as
contributor-validated for charge and the four power readings, so the register
map is not purely theoretical. A faithful port of working Python is still not a
tested build.

If a unit turns out not to encrypt, the bridge notices no key exchange starting
and drops to plain Modbus after ~15 seconds — the same fallback the reference
library makes. If a poll wedges with the link still up, it drops the connection
and reconnects, the way the reference recovers by reconnecting every cycle.

### How it talks

Full detail in [`docs/PROTOCOL.md`](docs/PROTOCOL.md). In short: a `ff00`
service with `ff01` notify / `ff02` write, carrying Modbus RTU. Newer firmware
opens with an encrypted handshake — ECDH on secp256r1, AES-CBC, ECDSA-signed
keys — which is implemented here. The signing keys are fixed constants from
the vendor app rather than per-device secrets, so no pairing or packet capture
is needed. mbedtls provides all of it.

### What is measured and what is inferred

| NUT variable | Source |
| --- | --- |
| `battery.charge` | register 102, measured |
| `battery.runtime` | register 104, measured (0 treated as unknown; absent on some models) |
| `ups.realpower` | AC + DC output power, measured |
| `input.realpower.ac` | register 146, measured |
| `input.voltage`, `input.current`, `output.voltage` | measured, on models that have them |
| `ups.status` `OL`/`OB` | **inferred** from AC input power and line voltage |
| `CHRG` | **inferred** from mains present and charge below 100% |
| `battery.voltage`, `battery.temperature` | **absent** — no register in the map |

The absent fields are the honest gap: the upstream map does not include pack
voltage or temperature for any of these models, so those NUT variables are
simply not published rather than guessed.

## Updating

Once it is on the network, the **Maintenance** tab checks GitHub for releases,
lists them, and installs the one you pick — no file to download. Older releases
are listed too, since going back is the right move when a new build misbehaves.

The image is written to the spare OTA slot; if it will not boot, the bootloader
rolls back to the running one. Uploading a `.bin` by hand still works and is
the fallback when the bridge has no route to the internet.

> The downloaded image is **not signed**. Trust rests on TLS and on GitHub:
> anyone who can intercept that connection or control the repository controls
> what gets installed. Secure boot is what fixes that properly, and this
> project does not enable it.

## Flash it

Download `esp32-nut-bluetti-<version>-factory.bin` from the
[latest release](https://github.com/thatguy-za/esp32-nut-bluetti/releases), then
open **[web.esphome.io](https://web.esphome.io)** in Chrome, Edge, or Opera on a
desktop, plug in an **ESP32-S3 (≥4 MB flash)**, hit *Connect* and choose that
file. It flashes at offset `0`.

Only this first flash needs a cable. After that, updates go over the network from
the [admin page](#admin-page). Offline / Linux / `esptool` instructions are in
[`dist/FLASHING.md`](dist/FLASHING.md).

You also need a Wi-Fi network the monitoring hosts can reach, and — on most dev
boards — the BOOT button on GPIO0 for the config wipe.

**Status LED.** If the board has an addressable WS2812 LED, it shows red while
the bridge is starting and green once it is connected to the BLUETTI unit over
Bluetooth. The **Maintenance** tab has an on/off toggle, a **data GPIO** field,
and a **Test** button that flashes red / green / blue on that pin — so you can
find which GPIO your board wired the LED to without a rebuild. Common values are
48, 38 and 21; `-1` turns it off. `STATUS_LED_GPIO` sets the boot default. A
board with a plain single-colour LED, or none, stays dark.

## Setting it up

Setup is two stages: get the bridge on a network, then point it at your BLUETTI.

### 1. Network (captive portal)

On first boot (or after a config wipe) the device brings up an **open Wi-Fi
access point** named `esp-nut-bluetti-XXXX` (`XXXX` = last 2 bytes of the MAC).
Join it from a phone or laptop — a captive-portal DNS server redirects
everything to the setup page, so it should pop up automatically; if not, browse
to `http://192.168.4.1/`.

**Step 1 — network.** Pick one:

- **Join my Wi-Fi** (default) — choose your network from the scanned list and
  enter the password. On success the page shows the bridge's new LAN IP as a
  link; reconnect your phone/laptop to your normal Wi-Fi and follow it. The
  setup AP shuts down.
- **Run its own AP** — name the network and set a password (8+ chars, or leave
  blank for open). The bridge reboots hosting that network at
  `http://192.168.4.1/`. Your NUT clients have to join it too.

**Step 2 — admin login.** Choose the username (default `admin`) and a password
for the bridge's own page. Any password is accepted, but you have to set one —
the admin page is what configures the BLUETTI unit, reads the logs and flashes
firmware.

### 2. BLUETTI + NUT (admin page)

Open the bridge's page and use the **BLUETTI** tab:

- **Scan for devices** and pick your unit — the list shows the Bluetooth name
  with the MAC in brackets, e.g. `EF-R3xxxx [AA:BB:CC:DD:EE:FF]`; likely BLUETTI
  units are marked ★. You can also type the address by hand.
- **BLUETTI account** — the River 3 only grants Bluetooth access to its own
  account, so the bridge needs your BLUETTI **account user id**. Enter your
  BLUETTI email + password (used once, over HTTPS, to fetch the id — the
  password is not stored) or paste the user id directly.

The **NUT** tab sets the UPS name, TCP port and low-battery threshold. Saving
either reboots the bridge.

All settings live in NVS (flash), so later boots go straight to serving NUT.

### Admin page

In normal operation the device serves a page at `http://<device-ip>/`:

- **Status** — the current state, with a live tail of the device log below it
  (~12 KB ring buffer) so you can watch the BLE handshake without a serial
  cable. Turn on `CONFIG_BLUETTI_BLE_TRACE` for the full dump.
- **BLUETTI** — BLE target, probe mode, and the optional device-controls toggle
  (see below).
- **NUT** — UPS name, TCP port, low-battery %.
- **Wi-Fi** — switch between joining a network and running as an access point;
  hostname; and DHCP or a static IPv4 address (address, mask, gateway, DNS).
  Addressing is station-only — the access point always serves `192.168.4.1`.
- **Alerts** — Telegram push notifications for power events.
- **Maintenance** —
  - **Firmware update**: upload a newer
    `esp32-nut-bluetti-<version>.bin`; it's written to the spare OTA slot and
    the device reboots, with bootloader rollback if the new build won't come up.
    Gated by a typed `FLASH` confirmation. Disable with
    `CONFIG_ENABLE_WEB_OTA=n`.
  - **Restart** — reboot, keeping settings.
  - **Admin login** — change the username / password (the current password is
    required).
  - **Reset** — forget everything and reboot into the setup AP.

The admin page is protected by the username and password you set during setup.
Signing in is a normal login form; the browser then carries a session cookie,
which expires after eight hours idle and is cleared by a reboot. There is **no
TLS**, so the password crosses the network in clear on the way in and the cookie
in clear thereafter — this keeps other people on the LAN out of the admin page,
it does not defend against someone capturing your traffic. Keep the
bridge on a trusted network.

### Reset

- **BOOT button:** hold GPIO0 to GND while resetting, keep it held ~3 s. The
  stored config is wiped and the device reboots into setup mode. (Pin and hold
  time are configurable in `menuconfig`.) This is also the way back in if you
  forget the admin password.
- **Web:** the Maintenance tab of the admin page.

If stored Wi-Fi credentials ever stop working, the device falls back to setup
mode on its own after a failed connect.

### Device controls (Elite 10, web UI only)

> **NUT stays read-only.** These controls live on the admin page and nowhere
> else — nothing is exposed as a writable NUT variable, so a misconfigured
> `upsmon` can never toggle the power station.

Off by default. Enable it on the **BLUETTI** tab and a **Controls** box appears
that can set, over the encrypted channel:

| Control | Values |
| --- | --- |
| AC output, DC output | on / off |
| AC ECO mode, DC ECO mode | on / off |
| AC ECO timeout, DC ECO timeout | 1–4 hours |
| Charging mode | Standard / Silent / Turbo / Custom |
| Power lifting | on / off |
| Screen timeout | 30 s / 1 min / 5 min / Never |

A change is written as a Modbus *write single register* wrapped in the same AES
layer as the reads, then confirmed by the next poll — so a switch takes a few
seconds to settle. Turning **AC output off while the unit is on battery** asks
for confirmation first, since it cuts power to whatever the unit is running.

**This has not been tested against hardware.** Writing to the power station
relies on register addresses that `bluetti-bt-lib` assumes but — because its own
integration disables writes on encrypted units — nobody appears to have
confirmed. Leave it off unless you are ready to verify it with probe mode. Only
the Elite 10 (and EL100V2) expose these.

## Telegram alerts

The **Alerts** tab sends a Telegram message when something happens to the power:

| Event | Default |
| --- | --- |
| Mains lost / restored | on |
| Battery low (crosses the NUT low-battery threshold) | on |
| BLUETTI unit unreachable / back | off |

Setup:

1. Message [@BotFather](https://t.me/BotFather), `/newbot`, and copy the token.
2. Message [@userinfobot](https://t.me/userinfobot) to get your numeric chat ID
   (group IDs start with `-`).
3. **Send your new bot a message first** — a bot cannot start a conversation, so
   without this Telegram rejects the send with "chat not found".
4. Paste both into the Alerts tab and hit **Send test message** to check before
   saving.

Repeats of the same event within a minute are suppressed, so a flapping supply
won't fill the chat. Messages are queued: if Telegram is unreachable the bridge
keeps serving NUT and drops the message rather than stalling.

## Using it with NUT

```bash
upsc -l <device-ip>            # lists the UPS name (default: bluetti)
upsc bluetti@<device-ip>       # dumps all variables
```

`upsmon` config (`upsmon.conf`):

```
MONITOR bluetti@<device-ip> 1 monuser somepass slave
```

### Variables

| Variable | Meaning |
| --- | --- |
| `ups.status` | `OL` / `OB` / `LB` / `CHRG` / `DISCHRG`; `OFF` or `OL WAIT` when telemetry is stale. The one `upsmon` acts on. |
| `battery.charge` | state of charge, % |
| `battery.charge.low` | the `LB` threshold (your setting) |
| `battery.runtime` | seconds left on battery; cleared while on mains |
| `battery.runtime.low` | the runtime `LB` threshold (your setting) |
| `battery.voltage` | pack voltage |
| `battery.temperature` | pack temperature, °C |
| `battery.capacity` | design capacity, Wh |
| `ups.load` | % of the configured continuous AC rating |
| `ups.realpower` / `ups.realpower.nominal` | output W / the configured AC rating |
| `input.realpower` / `input.realpower.ac` | total input W / mains input W |
| `output.realpower` | AC output W |
| `ups.type` | `online` when BLUETTI's backup mode is on, else `offline` |
| `ups.alarm` | device fault code, when non-zero |
| `ups.mfr` / `ups.model` / `ups.serial` | and the `device.*` equivalents |
| `driver.name` / `driver.version` / `driver.state` | bridge health |

`ups.status` gains `LB` when **either** `battery.charge` drops to
`battery.charge.low` **or** `battery.runtime` falls to `battery.runtime.low`.
The percentage alone is a poor guide under load: 20 % of a 245 Wh pack is
minutes at 300 W but hours at 20 W.

Both thresholds, and the **continuous AC rating** that backs `ups.load` and
`ups.realpower.nominal`, are set on the admin page's NUT tab — nothing about the
unit is hardcoded.

### Login

Optional, and follows standard NUT semantics: the username and password gate
`LOGIN` and `PRIMARY` (what `upsmon` uses to coordinate shutdown). Reading
values stays anonymous, because `upsc` has no way to send credentials. The
password is stored as a salted SHA-256.

There is no TLS, and no login at all until you set one, so keep the device on a
trusted LAN.

### Not implemented

No `SET VAR` or `INSTCMD`, so nothing can be changed on the BLUETTI through NUT
and there is no shutdown command — which is also why `ups.delay.shutdown` is not
published: nothing would honour it. The device controls, when enabled, are on
the admin page only and never touch NUT.

## What's implemented

- **NUT server** — upsd-compatible, read-only; verified against a third-party
  NUT client (`LIST UPS/VAR`, `GET VAR`, `upsmon` primary handshake, …).
- **Status LED** — red while starting, green once linked to the unit over BLE;
  a Maintenance-tab toggle turns it off. Addressable WS2812 only.
- **Provisioning** — two-step captive portal (network, then admin login), with
  BLUETTI + NUT set from the admin page afterwards; config in NVS, BOOT-button /
  web reset.
- **Admin auth** — a login form and a session cookie on every admin route; the
  password is stored as a salted SHA-256, never in the clear.
- **BLUETTI BLE "V2" stack** — ECDH (secp160r1) key agreement, AES-128-CBC
  session, keydata session-key derivation, both framing layers, `MD5(user_id +
  serial)` account auth, and a protobuf reader for the `pr705` telemetry
  message. Ported from [`rabits/ha-ef-ble`](https://github.com/rabits/ha-ef-ble).
- **Web OTA** — upload firmware from the admin page; spare-slot write with
  bootloader rollback.
- **Telegram alerts** — mains lost/restored, battery low, BLUETTI unreachable.
  Sent from a worker task so HTTPS never blocks the BLE or NUT paths.
- **Addressing** — DHCP by default, or a static IPv4 address with gateway and
  DNS; settable hostname, sent as the DHCP client name.

### Verified without hardware ([`test/`](test/), runs in CI)

- **Telemetry decode** — 5 real `DisplayPropertyUpload` packets captured from a
  River 3 UPS decode through the actual C code to the exact values ha-ef-ble
  documents (SOC, AC-in/out, load, discharge, temperature, backup mode, runtime).
- **Crypto** — micro-ecc secp160r1 pubkey + ECDH shared secret, `md5` IV /
  session-key / auth-token derivation, all byte-for-byte against `python-ecdsa`
  (which is what the device interoperates with).
- **NUT server** — full protocol conformance driven over a socket by a test
  client, plus the `upsmon` primary handshake.
- **Admin auth** — password hashing/verification and Basic-header parsing:
  salt uniqueness, wrong / empty / wrong-case passwords rejected, and no
  length or character restrictions.
- **IPv4 validation** — the static-addressing validator, including the lenient
  forms `esp_ip4addr_aton()` would wrongly accept (`192.168.1`, hex octets),
  which would otherwise strand the device on an unreachable address.
- **Framing** — CRC-8/16, inner-packet build↔parse, XOR deobfuscation, frame
  reassembly across split BLE notifications.

## Build from source

ESP32-S3, ≥4 MB flash. Requires
[ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.1 or newer.

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

Configuration is all runtime (the setup portal). `menuconfig` only sets
compile-time defaults, the config-wipe GPIO, the trace flag, and
`CONFIG_ENABLE_WEB_OTA`, under **`BLUETTI NUT Bridge`**. The version comes from
[`version.txt`](version.txt) — see [`RELEASING.md`](RELEASING.md).

### Debugging the BLE handshake

Enable **`Trace the BLUETTI BLE handshake`** in `menuconfig` (or
`CONFIG_BLUETTI_BLE_TRACE=y`). It hex-dumps every stage — our/device public keys,
shared secret, IV, session key, auth token, and every decoded inner packet — so a
failed handshake shows exactly where it broke. Watch it on the admin page's
**Logs** tab or `idf.py monitor`. It prints key material, so turn it off
afterwards.

## Layout

| Path | Role |
| --- | --- |
| `main/` | boot flow / BLUETTI→NUT variable mapping |
| `components/app_config/` | NVS-backed runtime config |
| `components/wifi_mgr/` | station + SoftAP (open or WPA2), scan, DHCP/static IPv4 |
| `components/notify/` | Telegram alerts (queue + worker, edge detection) |
| `components/provisioning/` | Wi-Fi setup portal (`portal.html`), DNS server, admin page (`admin.html`), web log tail (`log_ring.c`) |
| `components/nut_server/` | upsd-compatible TCP protocol server |
| `components/micro_ecc/` | vendored micro-ecc (secp160r1 for the BLE handshake) |
| `components/bluetti_ble/` | NimBLE transport + BLUETTI V2 stack: |
| &nbsp;&nbsp;`bluetti_ble.c` | scan / connect / GATT / notify / TX queue |
| &nbsp;&nbsp;`ef_crypto.c` | CRC, MD5, AES-128-CBC, ECDH secp160r1 |
| &nbsp;&nbsp;`ef_frame.c` | outer `5A5A` + inner `AA` framing, reassembly |
| &nbsp;&nbsp;`ef_session.c` | handshake state machine + telemetry dispatch |
| &nbsp;&nbsp;`ef_proto.c` | `pr705` `DisplayPropertyUpload` field reader |
| &nbsp;&nbsp;`ef_cloud.c` | BLUETTI account login → user id |
| &nbsp;&nbsp;`ef_keydata.bin` | BLUETTI key table (session-key derivation) |

## Security

- **Admin page** — a login form with the credentials chosen during setup, then
  an opaque session cookie (`HttpOnly`, `SameSite=Strict`, 8-hour idle timeout,
  cleared on reboot). The password is stored only as a salted SHA-256, and both
  it and the session token are compared in constant time. There is no TLS, so
  the password and the cookie are in clear on the wire: this keeps
  other people on your LAN out, it is not protection against traffic capture.
- **NUT server** — read-only and **unauthenticated**: `LOGIN` is accepted from
  anyone that can reach port 3493. That is deliberate (NUT clients expect it),
  so treat the port as public on your LAN.
- Don't expose either port to the internet. During first-run setup the Wi-Fi AP
  is open; it shuts down as soon as the device joins your network.
- Forgot the password, or set an unreachable static IP? Hold the BOOT button
  through a reset to wipe the config and return to the setup AP.
- The Telegram bot token is stored in NVS and is readable by anyone who can
  reach the admin page; the token only grants access to that bot.

## Protocol notes

- NUT protocol: <https://networkupstools.org/docs/developer-guide.chunked/ar01s09.html>
- BLUETTI BLE ("V2" / `encrypt_type 7`): ECDH on secp160r1 → AES-128-CBC session,
  a keydata-table + MD5 session-key derivation, then `MD5(user_id + serial)`
  account auth. Framing is `5A5A` EncPacket (AES body, CRC16) wrapping an `AA`
  Packet V2/V3 (CRC8/CRC16, XOR-obfuscated payload). Telemetry is the protobuf
  `DisplayPropertyUpload` message (`pr705` for River 3).

## Credits

The BLUETTI BLE protocol implementation is a C port of the reverse-engineering
work in:

- [`rabits/ha-ef-ble`](https://github.com/rabits/ha-ef-ble) — the Home Assistant
  integration this borrows the handshake, framing, key table, and protobuf
  field layout from.
- [`rabits/ef-ble-reverse`](https://github.com/rabits/ef-ble-reverse),
  [`nielsole/bluetti-bt-reverse-engineering`](https://github.com/nielsole/bluetti-bt-reverse-engineering)
  — earlier protocol notes.

Bundled third-party code: [micro-ecc](https://github.com/kmackay/micro-ecc)
(Kenneth MacKay, BSD-2-Clause) under `components/micro_ecc/`.

## License

MIT — see [`LICENSE`](LICENSE). Not affiliated with or endorsed by BLUETTI.
