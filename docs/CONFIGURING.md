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

NUT, Proxmox and Telegram are all off on a fresh device — turn on what you
want from the gear icon's **Integrations** page first; each tab appears once
its switch is on. Turn on **NUT server**, then use the **NUT** tab to set the
UPS name, TCP port, low-battery threshold, continuous AC rating and battery
capacity. Saving reboots the bridge. All settings live in NVS, so later boots
go straight to serving NUT.

## Admin page

Served at `http://<device-ip>/` once the bridge is on your network.

Two icons sit at the right of the nav bar: a **gear** for device-wide
configuration, and your **avatar** (your username's initial) for your own
login. Everything else is a tab across the top.

| Tab | What's there |
| --- | --- |
| **Status** | Live power flow (sources → battery → AC/DC load), charge, the NUT variables, network details, and a tail of the device log (~12 KB ring buffer). |
| **Bluetti** | BLE target and the device-controls toggle. |
| **NUT** | UPS name, TCP port, low-battery %, AC rating, battery capacity, and the optional NUT login. |
| **Proxmox** | Shut Proxmox VE hosts (and guests) down through their API when the battery runs low. Off by default, dry-run until armed. |
| **Telegram** | Telegram push notifications. |

**Gear icon:**

| Page | What's there |
| --- | --- |
| **Device Settings** | Firmware update, status-LED settings, restart, factory reset. |
| **Integrations** | The three on/off switches — see below. |
| **Network** | Join a network or run an access point; hostname; DHCP or static IPv4; the fallback AP. Addressing is station-only — an AP always serves `192.168.4.1`. |

**Avatar:**

| Page | What's there |
| --- | --- |
| **User Account** | Admin login (username and password). |
| **Log out** | Ends your session. |

**NUT**, **Proxmox** and **Telegram** only appear once turned on from the gear
icon's **Integrations** page — see [Integrations](#integrations) below.
**Bluetti** has no such switch: it's how the bridge reads the unit at all.

### Integrations

Gear icon → **Integrations**. Three switches — **NUT server**, **Proxmox shutdown**,
**Telegram alerts** — each off by default on a fresh device. Turning one on
makes its tab appear in the nav bar; turning it off hides the tab again and
stops that integration completely, not just the tab: the NUT listener isn't
bound, the Proxmox engine doesn't evaluate anything, Telegram sends nothing.

A device **upgrading** from a version before these switches existed keeps
running whatever it already had — NUT stays on (it had no switch before and
was never optional), and Proxmox/Telegram keep whatever their own enabled
setting already was. Only a genuinely new, never-provisioned device starts
with all three off.

Proxmox and Telegram apply immediately, the same as saving their own tabs.
NUT does not — there's no way to stop and restart its listener at runtime,
so switching it either way reboots the bridge.

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
green once linked over Bluetooth. Device Settings (gear icon) has an on/off toggle, a
**data GPIO** field, and a **Test** button that flashes red / green / blue on
that pin, so you can find the right GPIO without a rebuild. Common values are
48, 38 and 21; `-1` turns it off. `STATUS_LED_GPIO` sets the boot default. A
board with a plain single-colour LED, or none, stays dark.

### Reset

- **BOOT button** — hold GPIO0 to GND through a reset, keep it held ~3 s. The
  stored config is wiped and the device reboots into setup mode. This is also
  the way back in if you forget the admin password. Pin and hold time are
  configurable in `menuconfig`.
- **Web** — Device Settings (gear icon).

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

### Telegram alerts raised while offline

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

Off by default. Tick "Allow control of the Bluetti power station" on the
**Bluetti** tab — it applies immediately, no reboot — and the controls the
unit reports appear.

Changing a control stages it rather than sending it right away, and an
**Apply** button appears to send everything staged in one go. Nothing
reaches the unit until you click it — that's the point to catch a wrong
toggle, so flipping AC or DC output doesn't ask you to confirm a second
time — so several changes can be queued up together (say, a new charging
mode alongside a screen timeout) instead of round-tripping one at a time —
each one is actually sent, in order, rather than a later change silently
replacing an earlier one still waiting to go out.

Once sent, a control shows a small spinner in place of it until the unit's
own next read of that register confirms the change — it keeps showing the
value you asked for the whole time, rather than bouncing back to the old
one until that confirmation arrives.

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

Turn it on from the gear icon's **Integrations** page first — the **Telegram** tab
appears once it's on.

| Event | Default |
| --- | --- |
| Mains lost / restored | on |
| Battery low (crosses the NUT low-battery threshold) | on |
| Bluetti unit unreachable / back | off |
| Proxmox host shutdowns (dry-run fires included) | on |
| Proxmox guest (VM/CT) shutdowns (dry-run fires included) | on |

1. Message [@BotFather](https://t.me/BotFather), `/newbot`, copy the token.
2. Message [@userinfobot](https://t.me/userinfobot) for your numeric chat ID
   (group IDs start with `-`).
3. **Send your new bot a message first** — a bot can't start a conversation, so
   without this Telegram rejects the send with "chat not found".
4. Paste both into the Telegram tab and hit **Send test message**.

Repeats of the same event within a minute are suppressed, so a flapping supply
won't fill the chat. Messages are queued: if Telegram is unreachable the bridge
keeps serving NUT and drops the message rather than stalling.

Mains lost/restored fire only on genuine on-line ↔ on-battery transitions —
coming back from a dropped link (a reboot, say) is silent unless the
unreachable/back alert is on.

**Save** on this tab applies immediately too — no reboot.

## Proxmox shutdown

The bridge can shut Proxmox VE hosts down itself, through the Proxmox API, when
the Bluetti has been on battery too long or its charge is too low. It is the
appliance approach of [pve-ups](https://github.com/ffind-dev/pve-ups) in
firmware: no `upsmon` on the host, no shutdown script, no SSH — a dedicated API
token with the fewest privileges that work, and the decision made here where
you can see it. It reads the Bluetti directly and does not depend on the NUT
server or its thresholds.

### On Proxmox, once

A user that can do exactly what this needs, and a token that inherits it. In
the node shell (web UI → node → `>_ Shell`), as root; in a cluster this runs on
any one node and is valid on all of them:

```bash
pveum user add ups@pve
pveum role add UpsShutdown -privs "Sys.PowerMgmt VM.Audit VM.PowerMgmt"
pveum acl modify /nodes -user ups@pve -role UpsShutdown
pveum acl modify /vms   -user ups@pve -role UpsShutdown
pveum user token add ups@pve shutdown --privsep 0
```

The last line prints the **token ID** (`ups@pve!shutdown`) and the **secret** —
shown only once, copy it now. `Sys.PowerMgmt` shuts the node down; `VM.Audit`
lets the bridge *list* your VMs and containers so you can pick which to stop
first, and `VM.PowerMgmt` lets it stop them. Leave the `/vms` line out if you
only ever want the node shut down. Revoke at any time with
`pveum user token remove ups@pve shutdown`.

### On the bridge

Gear icon → **Integrations**, tick **Proxmox shutdown** — the **Proxmox**
tab appears once it's on. There, pick **Dry run** (the default) or **Armed**,
set how long the mains has to be back before a fired host re-arms (default
5 minutes), and how often every connected host is automatically re-tested
(default 24 hours; 0 turns this off).

Then the first host — **+ Add host** for more, up to four. Each host is
self-contained:

| | |
| --- | --- |
| **Connection** | API URL (`https://<node-ip>:8006`, **one per node** even in a cluster — a node that has already shut down can't proxy for the ones still to come), node name, token ID and secret (Proxmox's own two-field token — no certificate to type in, see below). The secret is write-only: the page never shows it again. |
| **Shut down when** | *On battery for N minutes* (default 30), timed on the bridge's own clock from the moment the mains drops; *or charge at or below N %* (default 10). Either one fires this host — the node itself, and any guest below with no trigger of its own. |
| **Guests** | **Load guests** lists the node's VMs and containers as cards; switch one on to give it its own on-battery/charge trigger, independent of the host's — no limit on how many. Leave a card off and it still goes down, just carried along whenever the host's own trigger fires rather than on a clock of its own. |

**Shut down when** and **Guests** stay hidden until **Test connection**
succeeds — nothing to set a trigger against until the bridge has actually
reached the host and pinned its certificate. A host can be filled in, left
disabled, and saved for later without ever being tested; only an *enabled*
host has to have a working, tested connection before you can save.

Hosts — and guests with their own trigger — fire **independently**: each has
its own countdown off the same shared outage and its own fire-once latch, so
a disposable VM can go at ten minutes while the hypervisor itself waits until
thirty. A guest with neither trigger set is still covered: it's stopped, then
waited on, the moment the *host's* trigger fires — the "stop these guests
before the node" behaviour this replaces, just per guest instead of typed as
a list.

### The certificate

Proxmox ships a self-signed certificate, so there is no authority to verify it
against. pve-ups leaves verification off; this bridge trusts the certificate
instead, the way SSH trusts a server's host key the first time you connect —
**trust-on-first-use**. There's no fingerprint to copy from anywhere and
compare: nothing to check it *against* until the bridge has seen it once, so
the page never shows or asks for one.

1. Fill in the connection (URL, node, token ID, secret) and click **Test
   connection**. On a host it hasn't seen before, the bridge completes the TLS
   handshake, trusts the certificate it was shown, and — in that same click —
   uses it to confirm the token's privileges and load the guest list.
2. From then on, every connection to that host — including the next Test — is
   checked against exactly that certificate. If it ever changes (Proxmox
   reissued it, or something is intercepting the connection), the host is
   refused and skipped with a clear message rather than silently trusted
   again.
3. Reissued Proxmox's certificate yourself? Test will say the host answered
   differently than before and offer a **reset and try again** button right
   there — that link only appears when it's actually needed, not as a
   standing control.

Until a host's certificate has been trusted, nothing is sent to it — not even
the token.

### Dry run, then arm

New installs are in **dry run**. Everything happens — the countdown on the
Status page, the log lines, the Telegram message — except the API call. Watch
one outage (or pull the Bluetti's mains plug) fire in dry run, check every host
reads *ready* on the Status page, and only then switch to **Armed**. Saving
armed asks you to confirm.

**Save** on this tab applies immediately — no reboot, unlike the other tabs.
Nothing here needs a subsystem restarted, and a reboot mid-outage would
briefly drop the NUT clients this feature exists to protect, right when
you might be adjusting a trigger.

### The safety model

Borrowed wholesale from pve-ups, because it is right:

- **Losing the Bluetti is never a reason to shut down.** No link, no data yet,
  or a sweep that hasn't read the mains registers all count as *unknown*, which
  starts no countdown and satisfies no trigger. A countdown that was *already*
  running keeps running, though — a confirmed outage doesn't become less
  confirmed because the Bluetooth link died in it, and a switch losing power is
  itself a symptom.
- **Each host fires once, then latches** until the mains has been back for
  *Re-arm after* minutes. A supply that flaps can't fire twice into a host that
  is mid-shutdown.
- **A failed request is retried** — three attempts, five seconds apart. A 503
  from a busy `pveproxy` isn't proof the node is going down.
- **The charge trigger needs a live reading**; a figure from before the link
  dropped says nothing about now. Only the timer fires blind.
- **Every connected host is re-tested on its own schedule**, exactly like a
  manual **Test connection** click, so a rotated certificate or a revoked
  token turns up on its own instead of waiting for a real outage to find it.
  Skipped entirely while a countdown is running — every host costs a few
  seconds on the network, and that time belongs to the countdown, not to
  routine checks.

The **Status** page shows one dot per host, alongside the BLE and NUT ones —
green for *ready*, amber for untested/counting down/just fired, red for a
failure. Hover a dot for the detail: the countdown while on battery,
*shutdown sent*, what went wrong, or how long ago it was last checked
(manually or by the automatic self-test).

> **The token can power off servers.** Keep the bridge on the same trusted
> network as the Proxmox web interface. The admin page's login is what stands
> between the network and the *Armed* switch.

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
| `input.realpower` / `input.realpower.ac` / `input.realpower.dc` | total input W / mains input W / DC (solar) input W |
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

There is no mains-present register, so `OL` / `OB` is inferred — and a full
unit sitting on the mains draws roughly 0 W, which is why line voltage is part
of the test. Registers are polled one per tick, so for the first few seconds of
a link some are still unread; the bridge reports `OL WAIT` until every field of
the plan has been read once, rather than reading "AC input not seen yet" as a
mains failure. Expect a few seconds of *Waiting for Bluetti* after a reconnect.

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
