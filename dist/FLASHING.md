# Flashing esp32-nut-ecoflow

`esp32-nut-ecoflow-factory.bin` (from the
[latest release](https://github.com/thatguy-za/esp32-nut-ecoflow/releases/latest))
is a **merged factory image** for **ESP32-S3, ≥4 MB flash**: bootloader +
partition table + app in one file, written at flash offset `0x0` (DIO, 80 MHz).

The release notes list the exact size and SHA-256 for that build — verify against
them after downloading.

Use a Chromium browser (Chrome / Edge / Opera) on a desktop — flashing needs
Web Serial, which Firefox and Safari don't support.

## Option A — hosted web installer (easiest)

<https://thatguy-za.github.io/esp32-nut-ecoflow/> — a one-click
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) **Install** button that
pulls the current release. Nothing to download or host.

## Option B — web.esphome.io

**Connect**, pick the serial port, then choose `esp32-nut-ecoflow-factory.bin`
from the [release](https://github.com/thatguy-za/esp32-nut-ecoflow/releases).

## Option C — Espressif's esptool-js (no hosting)

1. <https://espressif.github.io/esptool-js/> → **Connect**, pick the port.
2. Add `esp32-nut-ecoflow-factory.bin` at **Flash Address `0x0`**, **Program**.

## Option D — esptool on the command line

```bash
esptool --chip esp32s3 -b 460800 write-flash 0x0 esp32-nut-ecoflow-factory.bin
```

## Self-hosting the install button

The [`dist/`](.) folder has `index.html` + `manifest.json`. Drop the release
`esp32-nut-ecoflow-factory.bin` beside them and serve the folder from any static
host to get your own copy of Option A.

## First boot

The device has no Wi-Fi credentials yet, so it starts an open AP
`ecoflow-setup-XXXX`. Join it, the captive portal opens (or browse to
`http://192.168.4.1/`), then:

1. **Scan for EcoFlow** and pick your River 3 (`EF-R3…`).
2. **EcoFlow account** — enter your EcoFlow email + password (used once to
   fetch your account id; not stored) or paste your user id directly.
3. Pick your **Wi-Fi** and save.

See the project README for details.

## Updating later

After this first USB flash, new versions go over the network: the admin page at
`http://<device-ip>/` → **Maintenance** → **Firmware update**. Upload
`esp32-nut-ecoflow.bin` from a later release; it's written to the spare OTA slot
and the device reboots (bootloader rolls back if it won't come up).

## Individual images (Option C, manual offsets)

If you'd rather flash the parts separately (ESP32-S3, ≥4 MB flash):

| offset | file |
| --- | --- |
| `0x0` | `bootloader.bin` |
| `0x8000` | `partition-table.bin` |
| `0x20000` | `esp32-nut-ecoflow-app.bin` |
