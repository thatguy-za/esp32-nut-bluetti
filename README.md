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
`esp32-nut-ecoflow-factory.bin` from the
[latest release](https://github.com/thatguy-za/esp32-nut-ecoflow/releases).

Only this first flash needs a cable. After that, updates go over the network from
the [admin page](#admin-page). Offline / Linux / `esptool` instructions are in
[`dist/FLASHING.md`](dist/FLASHING.md).

You also need a Wi-Fi network the monitoring hosts can reach, and — on most dev
boards — the BOOT button on GPIO0 for the config wipe.

## First-run provisioning

On first boot (or after a config wipe) the device has no Wi-Fi credentials, so
it starts a **captive-portal setup**:

1. It brings up an **open Wi-Fi access point** named `esp-nut-ecoflow-XXXX`
   (`XXXX` = last 2 bytes of the MAC).
2. Join that network with a phone or laptop. A captive-portal DNS server
   redirects everything to the setup page, so it should pop up automatically;
   if not, browse to `http://192.168.4.1/`.
3. On the page:
   - **Scan for EcoFlow** — Bluetooth-scans for nearby devices; pick your unit
     (EcoFlow units are marked ★). Or type its MAC manually.
   - **EcoFlow account** — the River 3 only grants Bluetooth access to its own
     account, so the bridge needs your EcoFlow **account user id**. Enter your
     EcoFlow email + password (used once, over HTTPS, to fetch the id — the
     password is not stored) or paste the user id directly if you have it.
   - **Wi-Fi** — pick your network and enter the password.
   - **Advanced** (optional) — UPS name, NUT port, low-battery %.
4. **Save & connect.** The ESP32 joins your Wi-Fi. On success the page shows the
   device's new LAN IP and the setup AP shuts down.

All settings are stored in NVS (flash), so subsequent boots connect straight to
your Wi-Fi and start serving NUT.

### Admin page

In normal operation the device serves a small page at `http://<device-ip>/` with
four tabs:

- **Status** — Wi-Fi IP, BLE link state, battery %, model.
- **Config** — change the EcoFlow BLE target (re-scan and pick, or type a
  MAC / name prefix), the EcoFlow account, and the NUT settings (UPS name, port,
  low-battery %) without a full re-provision. Saving reboots the device.
- **Logs** — a live tail of the device log (~12 KB ring buffer), so you can watch
  the BLE handshake without a serial cable. Turn on `CONFIG_ECOFLOW_BLE_TRACE`
  for the full dump.
- **Maintenance** —
  - **Firmware update**: upload a new `esp32-nut-ecoflow.bin` (or the app-only
    `*-app.bin`); it's written to the spare OTA slot and the device reboots,
    with bootloader rollback if the new build won't come up. Gated by a typed
    `FLASH` confirmation. Disable with `CONFIG_ENABLE_WEB_OTA=n`.
  - **Forget config & reboot** (re-provision).

There is **no authentication** — anyone on the network can read the logs and
(with OTA enabled) flash firmware. Keep it on a trusted LAN.

### Re-provisioning

- **BOOT button:** hold GPIO0 to GND while resetting, keep it held ~3 s. The
  stored config is wiped and the device reboots into setup mode. (Pin and hold
  time are configurable in `menuconfig`.)
- **Web:** the Maintenance tab of the admin page.

If stored Wi-Fi credentials ever stop working, the device falls back to setup
mode on its own after a failed connect.

## Using it with NUT

```bash
upsc -l <device-ip>            # lists the UPS name (default: ecoflow)
upsc ecoflow@<device-ip>       # dumps all variables
```

`upsmon` config (`upsmon.conf`):

```
MONITOR ecoflow@<device-ip> 1 monuser somepass slave
```

The server is read-only and does not enforce auth (`LOGIN` is accepted from
anyone); keep the device on a trusted LAN.

## What's implemented

- **NUT server** — upsd-compatible, read-only; verified against a third-party
  NUT client (`LIST UPS/VAR`, `GET VAR`, `upsmon` primary handshake, …).
- **Provisioning** — captive-portal Wi-Fi + EcoFlow-account setup, config in NVS,
  BOOT-button / web re-provision.
- **EcoFlow BLE "V2" stack** — ECDH (secp160r1) key agreement, AES-128-CBC
  session, keydata session-key derivation, both framing layers, `MD5(user_id +
  serial)` account auth, and a protobuf reader for the `pr705` telemetry
  message. Ported from [`rabits/ha-ef-ble`](https://github.com/rabits/ha-ef-ble).
- **Web OTA** — upload firmware from the admin page; spare-slot write with
  bootloader rollback.

### Verified without hardware ([`test/`](test/), runs in CI)

- **Telemetry decode** — 5 real `DisplayPropertyUpload` packets captured from a
  River 3 UPS decode through the actual C code to the exact values ha-ef-ble
  documents (SOC, AC-in/out, load, discharge, temperature, backup mode, runtime).
- **Crypto** — micro-ecc secp160r1 pubkey + ECDH shared secret, `md5` IV /
  session-key / auth-token derivation, all byte-for-byte against `python-ecdsa`
  (which is what the device interoperates with).
- **NUT server** — full protocol conformance driven over a socket by a test
  client, plus the `upsmon` primary handshake.
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
| `components/wifi_mgr/` | STA + SoftAP + scan |
| `components/provisioning/` | captive portal (`portal.html`), DNS server, admin page (`admin.html`), web log tail (`log_ring.c`) |
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

The NUT server is **read-only and unauthenticated** — `LOGIN` is accepted from
anyone on the network, and there is no TLS. Keep the bridge on a trusted LAN and
do not expose port 3493 (or the setup portal) to the internet. During first-run
setup the Wi-Fi AP is open; it shuts down as soon as the device joins your
network.

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
