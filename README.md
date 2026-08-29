# esp32-nut-ecoflow

ESP32 firmware that connects to an **EcoFlow** portable power station over
**Bluetooth LE**, reads its battery / AC-input / load state, and exposes it on the
network as a UPS using the **NUT** (Network UPS Tools) TCP protocol.

Point your NAS, server, or Raspberry Pi's `upsmon` at the ESP32's IP on port
`3493` and it will see the EcoFlow as a normal UPS — including `OB`
(on-battery) / `OL` (online) status and battery charge, so hosts can shut
themselves down cleanly on a mains failure.

```
 ┌───────────┐   BLE    ┌─────────┐   TCP/3493 (NUT)   ┌──────────────┐
 │  EcoFlow  │ ───────► │  ESP32  │ ─────────────────► │ upsmon /     │
 │  station  │ ◄─────── │         │ ◄───────────────── │ upsc clients │
 └───────────┘          └─────────┘                    └──────────────┘
```

> ## 🚧 Work in progress
>
> **The end-to-end BLE handshake has never run against a real River 3.** A lot
> *is* verified without hardware (see below) — the telemetry decoder is checked
> against real captured River 3 packets and the crypto against the reference
> implementation — but the live handshake (frame timing, the `ecdh_type` byte,
> the keyinfo reply shape, write-with-response) is unproven and will likely need
> on-device debugging (`idf.py monitor`, `info` level). Treat every release as
> pre-release. Logs / BLE captures very welcome.

## Compatibility

| EcoFlow model | Serial prefix | Status |
| --- | --- | --- |
| **River 3**, **River 3 UPS** | `R651` `R653` `R654` `R655` | 🟡 **Target** — implemented, needs hardware testing |
| River 3 Plus | `R633`-family | 🟡 Same protocol; likely works, model string / add-on battery not handled |
| Delta 3 / 3 Plus / 3 Max, Delta Pro 3, Delta Pro Ultra, Smart Home Panel 2, Stream, PowerOcean | — | 🟠 Shares the BLE handshake, but a **different protobuf** — connects & authenticates, telemetry won't decode until its field map is added |
| Delta 2 / 2 Max, River 2 / 2 Max / 2 Pro, Delta Max, Delta Pro, RIVER Pro (2nd gen) | — | 🔴 Older `encrypt_type 0/1` — different handshake and framing, **not implemented** |
| Original DELTA / RIVER (1st gen) | — | 🔴 Different "v1" wire format |
| PowerStream, smart plugs, most GLACIER / WAVE data | — | 🔴 No local BLE telemetry (Wi-Fi / cloud only) |

Units bound to a **different EcoFlow account** than the `user_id` you provision
will also refuse the BLE auth.

## Flash it

Open the **[web installer](https://thatguy-za.github.io/esp32-nut-ecoflow/)** in
Chrome, Edge, or Opera on a desktop, plug in an **ESP32-S3 (≥4 MB flash)**, and
click **Install**. It's an [ESP Web Tools](https://esphome.github.io/esp-web-tools/)
button pointing at the latest release — the same one-click flasher ESPHome uses.

Or on **[web.esphome.io](https://web.esphome.io)**: *Connect*, then choose
`esp32-nut-ecoflow-<version>-factory.bin` from the
[latest release](https://github.com/thatguy-za/esp32-nut-ecoflow/releases).

Only this first flash needs a cable. After that, updates go over the network from
the [admin page](#admin-page). Offline / Linux / `esptool` instructions are in
[`dist/FLASHING.md`](dist/FLASHING.md).

You also need a Wi-Fi network the monitoring hosts can reach, and — on most dev
boards — the BOOT button on GPIO0 for the config wipe.

## Setting it up

Setup is two stages: get the bridge on a network, then point it at your EcoFlow.

### 1. Network (captive portal)

On first boot (or after a config wipe) the device brings up an **open Wi-Fi
access point** named `esp-nut-ecoflow-XXXX` (`XXXX` = last 2 bytes of the MAC).
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
the admin page is what configures the EcoFlow unit, reads the logs and flashes
firmware.

### 2. EcoFlow + NUT (admin page)

Open the bridge's page and use the **EcoFlow** tab:

- **Scan for devices** and pick your unit — the list shows the Bluetooth name
  with the MAC in brackets, e.g. `EF-R3xxxx [AA:BB:CC:DD:EE:FF]`; likely EcoFlow
  units are marked ★. You can also type an address or name prefix.
- **EcoFlow account** — the River 3 only grants Bluetooth access to its own
  account, so the bridge needs your EcoFlow **account user id**. Enter your
  EcoFlow email + password (used once, over HTTPS, to fetch the id — the
  password is not stored) or paste the user id directly.

The **NUT** tab sets the UPS name, TCP port and low-battery threshold. Saving
either reboots the bridge.

All settings live in NVS (flash), so later boots go straight to serving NUT.

### Admin page

In normal operation the device serves a page at `http://<device-ip>/`:

- **Status** — the current state, with a live tail of the device log below it
  (~12 KB ring buffer) so you can watch the BLE handshake without a serial
  cable. Turn on `CONFIG_ECOFLOW_BLE_TRACE` for the full dump.
- **EcoFlow** — BLE target and EcoFlow account.
- **NUT** — UPS name, TCP port, low-battery %.
- **Wi-Fi** — switch between joining a network and running as an access point;
  hostname; and DHCP or a static IPv4 address (address, mask, gateway, DNS).
  Addressing is station-only — the access point always serves `192.168.4.1`.
- **Alerts** — Telegram push notifications for power events.
- **Maintenance** —
  - **Firmware update**: upload a newer
    `esp32-nut-ecoflow-<version>.bin`; it's written to the spare OTA slot and
    the device reboots, with bootloader rollback if the new build won't come up.
    Gated by a typed `FLASH` confirmation. Disable with
    `CONFIG_ENABLE_WEB_OTA=n`.
  - **Restart** — reboot, keeping settings.
  - **Admin login** — change the username / password (the current password is
    required).
  - **Reset** — forget everything and reboot into the setup AP.

The admin page is protected by the username and password you set during setup
(HTTP Basic). There is **no TLS**, so credentials cross the network
base64-encoded, not encrypted — this keeps other people on the LAN out of the
admin page, it does not defend against someone capturing your traffic. Keep the
bridge on a trusted network.

### Reset

- **BOOT button:** hold GPIO0 to GND while resetting, keep it held ~3 s. The
  stored config is wiped and the device reboots into setup mode. (Pin and hold
  time are configurable in `menuconfig`.) This is also the way back in if you
  forget the admin password.
- **Web:** the Maintenance tab of the admin page.

If stored Wi-Fi credentials ever stop working, the device falls back to setup
mode on its own after a failed connect.

## Telegram alerts

The **Alerts** tab sends a Telegram message when something happens to the power:

| Event | Default |
| --- | --- |
| Mains lost / restored | on |
| Battery low (crosses the NUT low-battery threshold) | on |
| EcoFlow unit unreachable / back | off |

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
upsc -l <device-ip>            # lists the UPS name (default: ecoflow)
upsc ecoflow@<device-ip>       # dumps all variables
```

`upsmon` config (`upsmon.conf`):

```
MONITOR ecoflow@<device-ip> 1 monuser somepass slave
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
| `ups.type` | `online` when EcoFlow's backup mode is on, else `offline` |
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

No `SET VAR` or `INSTCMD`, so nothing can be changed on the EcoFlow through NUT
and there is no shutdown command — which is also why `ups.delay.shutdown` is not
published: nothing would honour it.

## What's implemented

- **NUT server** — upsd-compatible, read-only; verified against a third-party
  NUT client (`LIST UPS/VAR`, `GET VAR`, `upsmon` primary handshake, …).
- **Provisioning** — two-step captive portal (network, then admin login), with
  EcoFlow + NUT set from the admin page afterwards; config in NVS, BOOT-button /
  web reset.
- **Admin auth** — HTTP Basic on every admin route; the password is stored as a
  salted SHA-256, never in the clear.
- **EcoFlow BLE "V2" stack** — ECDH (secp160r1) key agreement, AES-128-CBC
  session, keydata session-key derivation, both framing layers, `MD5(user_id +
  serial)` account auth, and a protobuf reader for the `pr705` telemetry
  message. Ported from [`rabits/ha-ef-ble`](https://github.com/rabits/ha-ef-ble).
- **Web OTA** — upload firmware from the admin page; spare-slot write with
  bootloader rollback.
- **Telegram alerts** — mains lost/restored, battery low, EcoFlow unreachable.
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
`CONFIG_ENABLE_WEB_OTA`, under **`EcoFlow NUT Bridge`**. The version comes from
[`version.txt`](version.txt) — see [`RELEASING.md`](RELEASING.md).

### Debugging the BLE handshake

Enable **`Trace the EcoFlow BLE handshake`** in `menuconfig` (or
`CONFIG_ECOFLOW_BLE_TRACE=y`). It hex-dumps every stage — our/device public keys,
shared secret, IV, session key, auth token, and every decoded inner packet — so a
failed handshake shows exactly where it broke. Watch it on the admin page's
**Logs** tab or `idf.py monitor`. It prints key material, so turn it off
afterwards.

## Layout

| Path | Role |
| --- | --- |
| `main/` | boot flow / EcoFlow→NUT variable mapping |
| `components/app_config/` | NVS-backed runtime config |
| `components/wifi_mgr/` | station + SoftAP (open or WPA2), scan, DHCP/static IPv4 |
| `components/notify/` | Telegram alerts (queue + worker, edge detection) |
| `components/provisioning/` | Wi-Fi setup portal (`portal.html`), DNS server, admin page (`admin.html`), web log tail (`log_ring.c`) |
| `components/nut_server/` | upsd-compatible TCP protocol server |
| `components/micro_ecc/` | vendored micro-ecc (secp160r1 for the BLE handshake) |
| `components/ecoflow_ble/` | NimBLE transport + EcoFlow V2 stack: |
| &nbsp;&nbsp;`ecoflow_ble.c` | scan / connect / GATT / notify / TX queue |
| &nbsp;&nbsp;`ef_crypto.c` | CRC, MD5, AES-128-CBC, ECDH secp160r1 |
| &nbsp;&nbsp;`ef_frame.c` | outer `5A5A` + inner `AA` framing, reassembly |
| &nbsp;&nbsp;`ef_session.c` | handshake state machine + telemetry dispatch |
| &nbsp;&nbsp;`ef_proto.c` | `pr705` `DisplayPropertyUpload` field reader |
| &nbsp;&nbsp;`ef_cloud.c` | EcoFlow account login → user id |
| &nbsp;&nbsp;`ef_keydata.bin` | EcoFlow key table (session-key derivation) |

## Security

- **Admin page** — HTTP Basic auth with the credentials chosen during setup.
  The password is stored only as a salted SHA-256, and compared in constant
  time. There is no TLS, so the credential is base64 on the wire: this keeps
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
- EcoFlow BLE ("V2" / `encrypt_type 7`): ECDH on secp160r1 → AES-128-CBC session,
  a keydata-table + MD5 session-key derivation, then `MD5(user_id + serial)`
  account auth. Framing is `5A5A` EncPacket (AES body, CRC16) wrapping an `AA`
  Packet V2/V3 (CRC8/CRC16, XOR-obfuscated payload). Telemetry is the protobuf
  `DisplayPropertyUpload` message (`pr705` for River 3).

## Credits

The EcoFlow BLE protocol implementation is a C port of the reverse-engineering
work in:

- [`rabits/ha-ef-ble`](https://github.com/rabits/ha-ef-ble) — the Home Assistant
  integration this borrows the handshake, framing, key table, and protobuf
  field layout from.
- [`rabits/ef-ble-reverse`](https://github.com/rabits/ef-ble-reverse),
  [`nielsole/ecoflow-bt-reverse-engineering`](https://github.com/nielsole/ecoflow-bt-reverse-engineering)
  — earlier protocol notes.

Bundled third-party code: [micro-ecc](https://github.com/kmackay/micro-ecc)
(Kenneth MacKay, BSD-2-Clause) under `components/micro_ecc/`.

## License

MIT — see [`LICENSE`](LICENSE). Not affiliated with or endorsed by EcoFlow.
