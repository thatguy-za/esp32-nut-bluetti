# Flashing esp32-nut-ecoflow

`esp32-nut-ecoflow-factory.bin` is a **merged factory image** for **ESP32-S3**:
bootloader + partition table + app in one file, written at flash offset `0x0`.

- size: 1,362,736 bytes
- sha256: `5039869b8f046d35ed166d765dfaa8ab956019a3571dd5cf69646e01fb840ed3`
- chip: ESP32-S3, flash mode DIO, 80 MHz, 2 MB

Use a Chromium browser (Chrome / Edge / Opera) on a desktop — flashing needs
Web Serial, which Firefox and Safari don't support.

## Option A — ESP Web Tools install button (this folder)

Drop `esp32-nut-ecoflow-factory.bin` from the
[GitHub release](https://github.com/thatguy-za/esp32-nut-ecoflow/releases) into
this `dist/` folder (it is not checked into git), host the folder on any static
host (GitHub Pages, Netlify, `python3 -m http.server`, …), and open `index.html`.
It renders an **Install** button backed by
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) reading `manifest.json`.
Click it, pick the serial port, done — it erases flash and writes the image.

## Option B — Espressif's web flasher (no hosting needed)

1. Go to <https://espressif.github.io/esptool-js/>.
2. **Connect**, pick the board's serial port.
3. Add file `esp32-nut-ecoflow-factory.bin` at **Flash Address `0x0`**.
4. **Program**.

## Option C — esptool on the command line

```bash
esptool --chip esp32s3 -b 460800 write-flash 0x0 esp32-nut-ecoflow-factory.bin
```

## Note on web.esphome.io

<https://web.esphome.io> is specific to ESPHome-generated firmware and won't
flash this image through its normal UI. Use Option A or B instead — both use the
same underlying flashing engine.

## First boot

The device has no Wi-Fi credentials yet, so it starts an open AP
`ecoflow-setup-XXXX`. Join it, the captive portal opens (or browse to
`http://192.168.4.1/`), then:

1. **Scan for EcoFlow** and pick your River 3 (`EF-R3…`).
2. **EcoFlow account** — enter your EcoFlow email + password (used once to
   fetch your account id; not stored) or paste your user id directly.
3. Pick your **Wi-Fi** and save.

See the project README for details.

## Individual images (Option C, manual offsets)

If you'd rather flash the parts separately:

| offset | file |
| --- | --- |
| `0x0` | `bootloader.bin` |
| `0x8000` | `partition-table.bin` |
| `0x10000` | `esp32-nut-ecoflow-app.bin` |
