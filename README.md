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

## Status

Targets the **EcoFlow River 3 / River 3 UPS** (`EF-R3…`, serial `R651` / `R653`
/ `R654` / `R655`). The full EcoFlow BLE "V2" stack is implemented — ECDH
(secp160r1) key agreement, AES-128-CBC session, the keydata session-key
derivation, both framing layers, account auth, and a protobuf reader for the
`pr705` telemetry message. Wi-Fi provisioning and the NUT server are done.

**Untested on hardware** — written against the `rabits/ha-ef-ble` reference and
verified with host unit tests for the framing / CRC / protobuf / packet paths,
but not yet run against a real River 3. Expect to debug the handshake on-device
(serial log at `info`).

Other EcoFlow models are not wired up; the transport is generic but the
telemetry decode and model table are River-3-specific.

## Hardware

- Any ESP32 with BLE (ESP32, ESP32-S3, ESP32-C3, …).
- A Wi-Fi network reachable by the hosts that will monitor the UPS.
- A button to GND on GPIO0 (the BOOT button on most dev boards) for config wipe.

## Build & flash (ESP-IDF)

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.1 or newer.

```bash
idf.py set-target esp32
idf.py build flash monitor
```

You normally do **not** need `menuconfig` — configuration happens at runtime
through the setup portal (below). `menuconfig` only sets compile-time defaults
and the config-wipe GPIO, under **`EcoFlow NUT Bridge`**.

## First-run provisioning

On first boot (or after a config wipe) the device has no Wi-Fi credentials, so
it starts a **captive-portal setup**:

1. It brings up an **open Wi-Fi access point** named `ecoflow-setup-XXXX`
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

### Re-provisioning

- **BOOT button:** hold GPIO0 to GND while resetting, keep it held ~3 s. The
  stored config is wiped and the device reboots into setup mode. (Pin and hold
  time are configurable in `menuconfig`.)
- **Web:** browse to `http://<device-ip>/` on your LAN and use
  **Forget config & reboot**.

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

## Layout

| Path | Role |
| --- | --- |
| `main/` | boot flow / EcoFlow→NUT variable mapping |
| `components/app_config/` | NVS-backed runtime config |
| `components/wifi_mgr/` | STA + SoftAP + scan |
| `components/provisioning/` | captive portal (`portal.html`), DNS server, admin page |
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
