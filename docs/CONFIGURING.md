# Configuring the bridge

Everything past the quick start in the [README](../README.md): the setup
walkthrough, the admin page, device controls, alerts, and the full NUT
variable list.

## First-run setup

### 1. Network (captive portal)

On first boot — or after a config wipe — the device brings up an **open Wi-Fi
access point** named `esp-nut-bluetti-XXXX` (`XXXX` = last two bytes of the
MAC). Join it from a phone or laptop; a captive-portal DNS server redirects
everything to the setup page, so it should pop up on its own. If not, browse to
`http://192.168.4.1/`.

**Network.** Either:

- **Join my Wi-Fi** (default) — pick your network from the scan and enter the
  password. On success the page shows the bridge's new LAN IP as a link;
  reconnect to your normal Wi-Fi and follow it. The setup AP shuts down.
- **Run its own AP** — name the network and set a password (8+ characters, or
  blank for open). The bridge reboots hosting that network at
  `http://192.168.4.1/`. Your NUT clients have to join it too.

**Admin login.** Choose a username (default `admin`) and password for the
bridge's own page. Any password is accepted, but one is required — the admin
page is what configures the unit, reads the logs and flashes firmware.

### 2. Point it at your Bluetti

Open the bridge's page and use the **Bluetti** tab: **Scan for devices** and
pick your unit — the list shows the Bluetooth name with the MAC in brackets,
and likely Bluetti units are marked ★. You can also type the address by hand.
There is no account or pairing step; the handshake uses fixed keys from the
vendor app.

The **NUT** tab sets the UPS name, TCP port, low-battery threshold, continuous
AC rating and battery capacity. Saving either tab reboots the bridge. All
settings live in NVS, so later boots go straight to serving NUT.

## Admin page

Served at `http://<device-ip>/` once the bridge is on your network.

| Tab | What's there |
| --- | --- |
| **Status** | Live power flow (sources → battery → AC/DC load), charge, the NUT variables, network details, and a tail of the device log (~12 KB ring buffer). |
| **Bluetti** | BLE target and the device-controls toggle. |
| **NUT** | UPS name, TCP port, low-battery %, AC rating, battery capacity, and the optional NUT login. |
| **Network** | Join a network or run an access point; hostname; DHCP or static IPv4; the fallback AP. Addressing is station-only — an AP always serves `192.168.4.1`. |
| **Alerts** | Telegram push notifications. |
| **Maintenance** | Firmware update, restart, admin login, factory reset, status-LED settings. |

### Log level

The **Level** selector in the Status tab's Logs box:

| Level | Effect |
| --- | --- |
| **Off** (default) | Device log silenced. |
| **Basic** | Normal operational logging. |
| **Verbose** | Keeps decoding, and hex-dumps every BLE frame plus a `reg <addr> = <value>` line per register read — for chasing a wrong reading without a serial cable. Prints key material, so turn it back off. |

### Firmware update

From GitHub (pick a release, nothing to download) or by uploading an
`esp32-nut-bluetti-<version>.bin`. Either way it's written to the spare OTA
slot and the device reboots, with bootloader rollback if the new build won't
come up. Disable the whole feature with `CONFIG_ENABLE_WEB_OTA=n`.

> The downloaded image is **not signed**. Trust rests on TLS and on GitHub:
> anyone who can intercept that connection or control the repository controls
> what gets installed. Secure boot is what fixes that properly, and this
> project does not enable it.

### Status LED

If the board has an addressable WS2812 LED it shows red while starting and
green once linked over Bluetooth. The Maintenance tab has an on/off toggle, a
**data GPIO** field, and a **Test** button that flashes red / green / blue on
that pin, so you can find the right GPIO without a rebuild. Common values are
48, 38 and 21; `-1` turns it off. `STATUS_LED_GPIO` sets the boot default. A
board with a plain single-colour LED, or none, stays dark.

### Reset

- **BOOT button** — hold GPIO0 to GND through a reset, keep it held ~3 s. The
  stored config is wiped and the device reboots into setup mode. This is also
  the way back in if you forget the admin password. Pin and hold time are
  configurable in `menuconfig`.
- **Web** — the Maintenance tab.

Setup mode is only entered on request — a wipe, or a device that has never been
provisioned. A bridge that simply can't reach its network keeps trying instead;
see below.

## When the network goes away

A UPS bridge is worth least at exactly the moment it's needed most, so the
behaviour here is deliberate.

**It keeps trying, forever.** A dropped station link is retried with backoff up
to 30 s between attempts, for as long as it takes. Nothing gives up, and losing
Wi-Fi never sends the device back to setup mode — after a power cut the bridge
boots in seconds while the router takes a minute or two, and a device that
surrendered at that point would sit in a setup portal serving nothing while a
perfectly good network came up around it. It boots, starts everything, and
joins when the network is there.

**The Bluetti link is unaffected.** Bluetooth is a separate radio and a separate
task: polling continues, telemetry stays current, and the NUT variables are
right the instant a client can reach them again. The NUT listener isn't torn
down either, so `upsc` works again with no restart.

### Fallback AP

**Network** tab, off by default. If the bridge can't reach your Wi-Fi for 60
seconds it brings up its own access point, so a device with nowhere to be
reached is still reachable — join that network and the admin page is on
`http://192.168.4.1/`.

Set the SSID (blank = the board's default name) and a password of 8+ characters,
or tick **Open network**. While it's up, the Status page says so, and the
station keeps retrying underneath: when your network returns the bridge rejoins
it and drops the AP on its own. Nothing to undo.

It is a way back in, not a way to run: NUT clients on your normal network cannot
see the bridge while it is hosting the fallback. If you want the bridge to
*live* on its own network, use **Run its own AP** instead.

### Alerts raised while offline

The event most worth alerting on — the mains failing — is the one likeliest to
take your router with it. A Telegram message that can't be sent is therefore
held and retried (5 s, 15 s, 45 s, then every 5 minutes for about an hour)
rather than dropped. Anything that lands more than 90 seconds late arrives with
`(N min ago)` appended, so a "Mains lost" that waited out the blackout doesn't
read as happening now.

## Device controls

> **NUT stays read-only.** These live on the admin page and nowhere else —
> nothing is exposed as a writable NUT variable, so a misconfigured `upsmon`
> can never toggle the power station.

Off by default. Tick "Allow controlling the unit" on the **Bluetti** tab — it
applies immediately, no reboot — and the controls the unit reports appear.

| Control | Values | Models |
| --- | --- | --- |
| AC output, DC output | on / off | all controllable models |
| Power lifting | on / off | most models |
| AC / DC ECO mode | on / off | EL10, EL100V2, AC70, AC180, EL30V2 |
| AC / DC ECO timeout | 1–4 hours | " |
| Charging mode | Standard / Silent / Turbo / Custom | + AC180P |
| Screen timeout | 30 s / 1 min / 5 min / Never | EL10, EL100V2 |
| Discharge floor / charge limit | 0–100 % | EL100V2 only |

Controllable models: **EL10, EL100V2, AC70, AC180, EL30V2, AC180P, AC2P, AC60,
AC60P, Handsfree 2.** The bridge shows only the controls whose registers the
unit actually answers, so one that doesn't exist simply never appears.

The *telemetry* decode is restricted to the Elite-10 family because
`bluetti-bt-lib` scales a few readings differently per model. The *controls*
have no such problem — a switch register is 0/1 and the modes use shared enums
— so any V2 model that declares them can use them.

The Elite 10 has **no SOC-range register**: 2022/2023 read 0 on hardware and
`bluetti-bt-lib` has no such field for it. Set the charge limit in the Bluetti
app instead. The EL100V2 does have it.

A change is a Modbus *write single register* wrapped in the same AES layer as
the reads, confirmed by the next poll — so a switch takes a few seconds to
settle. Turning **AC output off while on battery** asks for confirmation first,
since it cuts power to whatever the unit is running.

**Control writes are not verified against hardware.** The registers come from
`bluetti-bt-lib`'s field definitions, but its own integration disables writes
on encrypted units, so nobody has confirmed a write lands. Reads are solid on
the Elite 10 Mini; the writes are the untested half.

## Telegram alerts

| Event | Default |
| --- | --- |
| Mains lost / restored | on |
| Battery low (crosses the NUT low-battery threshold) | on |
| Bluetti unit unreachable / back | off |

1. Message [@BotFather](https://t.me/BotFather), `/newbot`, copy the token.
2. Message [@userinfobot](https://t.me/userinfobot) for your numeric chat ID
   (group IDs start with `-`).
3. **Send your new bot a message first** — a bot can't start a conversation, so
   without this Telegram rejects the send with "chat not found".
4. Paste both into the Alerts tab and hit **Send test message**.

Repeats of the same event within a minute are suppressed, so a flapping supply
won't fill the chat. Messages are queued: if Telegram is unreachable the bridge
keeps serving NUT and drops the message rather than stalling.

Mains lost/restored fire only on genuine on-line ↔ on-battery transitions —
coming back from a dropped link (a reboot, say) is silent unless the
unreachable/back alert is on.

## NUT variables

| Variable | Meaning |
| --- | --- |
| `ups.status` | `OL` / `OB` / `LB` / `CHRG` / `DISCHRG`; `OFF` or `OL WAIT` when telemetry is stale. The one `upsmon` acts on. |
| `battery.charge` | state of charge, % |
| `battery.charge.low` | the `LB` threshold (your setting) |
| `battery.runtime` | seconds the battery would last at the current load — published on mains too |
| `battery.runtime.low` | the runtime `LB` threshold (your setting) |
| `battery.capacity` | the model's spec capacity, or your NUT-tab override |
| `ups.load` | % of the configured continuous AC rating |
| `ups.realpower` / `ups.realpower.nominal` | AC + DC output W / the configured AC rating |
| `input.realpower` / `input.realpower.ac` | total input W / mains input W |
| `output.realpower` / `output.realpower.dc` | AC output W / DC output W |
| `outlet.1.*` / `outlet.2.*` | AC and DC output banks — `desc`, `status`, `realpower` |
| `input.voltage`, `input.current`, `output.voltage` | measured, Elite-10 family only |
| `ups.alarm` | device fault code, when non-zero |
| `ups.mfr` / `ups.model` / `ups.serial` | and the `device.*` equivalents |
| `driver.name` / `driver.version` / `driver.state` | bridge health |

`battery.voltage` and `battery.temperature` are **not published** — no register
exists for them in the upstream map, so they're omitted rather than guessed.

`ups.status` gains `LB` when **either** `battery.charge` drops to
`battery.charge.low` **or** `battery.runtime` falls to `battery.runtime.low`.
The percentage alone is a poor guide under load: 20 % of a 245 Wh pack is
minutes at 300 W but hours at 20 W.

### What's measured, what's inferred

| Value | Source |
| --- | --- |
| charge, AC/DC input & output power | measured registers |
| line voltage / current | measured, Elite-10 family only |
| `battery.runtime` | the unit's own estimate while discharging, else capacity × charge ÷ load |
| `OL` / `OB` | **inferred** from AC input power and line voltage |
| `CHRG` | **inferred** from net battery flow (any source, including solar) |

### NUT login

Optional, and standard NUT semantics: the username and password gate `LOGIN`
and `PRIMARY` (what `upsmon` uses to coordinate shutdown). Reading values stays
anonymous, because `upsc` has no way to send credentials. The password is
stored as a salted SHA-256. A fresh device ships with a per-unit password,
`bluetti<XXXX>` (last two MAC bytes), shown on the NUT tab.

### Not implemented

No `SET VAR` or `INSTCMD`, so nothing can be changed on the Bluetti through NUT
and there's no shutdown command — which is also why `ups.delay.shutdown` isn't
published: nothing would honour it.

## Source layout

| Path | Role |
| --- | --- |
| `main/` | boot flow, Bluetti → NUT variable mapping |
| `components/app_config/` | NVS-backed runtime config |
| `components/wifi_mgr/` | station + SoftAP, scan, DHCP/static IPv4 |
| `components/notify/` | Telegram alerts (queue, worker, edge detection) |
| `components/provisioning/` | setup portal, DNS server, admin page, web log tail |
| `components/nut_server/` | upsd-compatible TCP server |
| `components/ota_github/` | release listing + streaming OTA install |
| `components/led_status/` | addressable status LED |
| `components/bluetti_ble/` | NimBLE transport + the Bluetti V2 stack: |
| &nbsp;&nbsp;`bluetti_ble.c` | scan / connect / GATT / notify / poll loop |
| &nbsp;&nbsp;`bt_crypto.c` | AES-CBC, MD5, SHA-256, ECDH + ECDSA on secp256r1 |
| &nbsp;&nbsp;`bt_session.c` | `2a2a` handshake, AES framing, Modbus request/response |
| &nbsp;&nbsp;`bt_regs.c` | model table, polling plan, register decode, controls |

## Build from source

ESP32-S3, ≥4 MB flash. Requires
[ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.1 or newer.

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

Configuration is all runtime. `menuconfig` only sets compile-time defaults, the
config-wipe GPIO, the boot default for the BLE trace, and
`CONFIG_ENABLE_WEB_OTA`, under **`BLUETTI NUT Bridge`**. The version comes from
[`version.txt`](../version.txt) — see [`RELEASING.md`](../RELEASING.md).

Host-side tests (no hardware, run in CI):

```bash
make -C test
```

They cover the Modbus/CRC framing and the `2a2a` frame checksum, the model
table and register decode, NUT protocol conformance over a real socket, admin
password hashing, static-IPv4 validation, the low-battery logic, the GitHub
release parser, and config-blob forward compatibility.
