# ESP32-AC

A [Seeed XIAO ESP32C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/) that holds the
credentials for two Wi-Fi air conditioners, polls them over the LAN, and re-exposes them as a small
REST API — and, from phase 4, as native HomeKit accessories in Apple Home.

Both units are already on the network, so this board is not a radio bridge. Its job is to be the one
always-on device in the house: it keeps the sessions alive, normalises two very different vendor
protocols into one vocabulary, and speaks HomeKit so no Home Assistant or Homebridge install is
needed. The two units remain independent throughout, and appear in Apple Home as two separate
accessories.

One board handles both. Unlike an IR blaster this needs no proximity or line-of-sight to either AC —
both are reached over the LAN through the router — so the board can sit anywhere with decent WiFi.

| Unit | Protocol | Reference implementation |
|---|---|---|
| Midea Porta Split | Midea LAN, TCP/6444, AES + SHA256 V3 handshake | [`christiandt/mpsac`](https://github.com/christiandt/mpsac) (Python, `msmart-ng`) |
| Electrolux | Broadlink OEM module, UDP, AES-128-CBC + ASCII JSON | [`christiandt/electrolux-ac-cli`](https://github.com/christiandt/electrolux-ac-cli) (Python, `broadlink`) |

Those two repos stay the source of truth for provisioning and for debugging against real hardware.
This firmware is a port of their wire protocols to C++; neither repo is modified.

## Status

| Phase | What | State |
|---|---|---|
| 1 | Skeleton, task model, REST API, provisioning, native test harness | **done** |
| 2 | Electrolux / Broadlink driver | **done** (status schema needs confirming — see below) |
| 3 | Midea LAN V3 driver | **done** |
| 4 | HomeKit via HomeSpan | **done** |

Either unit falls back to a `MockDriver` under its final id if it isn't configured, so the REST shape
is the same whether or not hardware is attached.

### Confirming the Electrolux status schema

`electrolux-ac-cli` never parses the status response — `status()` returns the raw blob and only
`envtemp` is named anywhere (in its README). The driver reads the field names that mirror the
setters (`temp`, `ac_pwr`, `ac_mode`, `ac_mark`, `ac_vdir`, `scrdisp`, `ac_slp`, `mldprf`), which is
the obvious reading but is inference until a real reply is seen. Any field that isn't there simply
stays `null`.

To check, ask the device what it actually said:

```bash
curl -H "Authorization: Bearer $TOKEN" http://esp32-ac.local/api/units/electrolux/raw
```

If a name differs, it's a one-line fix in `ElectroluxDriver::parseStatus`.

## Toolchain

The stock PlatformIO `espressif32` platform pins Arduino core 2.x, which has **no ESP32-C6 support at
all**. This project uses [pioarduino](https://github.com/pioarduino/platform-espressif32), the
community fork tracking Arduino core 3.x / ESP-IDF 5.x. It is pinned by URL in `platformio.ini`;
bump the tag there to update. (Note the release asset is `platform-espressif32.zip` — no version
suffix.)

```bash
pip install platformio

pio run  -e xiao_esp32c6                # build firmware
pio run  -e xiao_esp32c6 -t upload      # flash
pio device monitor                      # serial, 115200

pio test -e native                      # host tests for src/proto/
```

## Layout

```
src/ac_types.h          vendor-neutral vocabulary (Mode, Swing, FanSpeed, Feature, AcState, AcCommand)
src/ac_driver.h         the seam every AC implementation sits behind
src/ac_registry.*       per-unit FreeRTOS tasks, mutexed state, command queues
src/config_store.*      NVS-backed configuration
src/rest_api.*          esp_http_server routes
src/drivers/            mock.*, and later electrolux.* / midea.*
src/proto/              pure C++ protocol code — no Arduino includes, host-testable
test/test_proto/        native unit tests
tools/export_config.py  builds the provisioning blob from the Python CLIs' cached config
```

`src/proto/` is deliberately free of Arduino and ESP-IDF headers. Framing and checksums are the
likeliest place for a porting bug, and keeping them host-compilable means those bugs get caught by
`pio test -e native` instead of on the bench.

### How the ports are verified

Every expected value in `test/` comes from the *reference* implementation — msmart for Midea,
python-broadlink and `electrolux-ac-cli` for the Electrolux — not from this C++ code. A mistake in
the port therefore fails a test rather than being baked into the fixture. `tools/gen_vectors.py`
regenerates them all.

Encryption can't be checked that way, because the native environment has no mbedtls. Instead the
firmware runs a **boot self-test** over vectors generated the same way: the MD5-derived AES-ECB key,
PKCS7, AES-CBC (including that mbedtls doesn't mutate the caller's IV), the V3 handshake key
derivation with a tampered-digest negative case, and a full Midea V2 packet encode/decode compared
byte-for-byte against one built by msmart. The result is logged at startup and reported by
`GET /api/health`:

```json
{ "ok": true, "selftest": { "passed": true, "detail": "all checks passed" } }
```

## First boot

With no stored config the device generates a REST bearer token, prints it to serial, and comes up as
a Wi-Fi access point:

```
SSID:     esp32-ac-setup
Password: acsetup123
```

Join it and POST your configuration. `tools/export_config.py` builds the blob from the config the
Python CLIs already cached — `~/.config/mpsac/config.json` (written by `mpsac setup`, including the
V3 token/key it fetched from the Midea cloud) and `~/.electrolux_ac_config.json`:

```bash
python3 tools/export_config.py --ssid HomeWiFi --psk hunter2 \
    --post http://192.168.4.1/api/config
```

Add `--probe` to connect to the Midea unit first and read its real capability flags and setpoint
limits (needs `msmart-ng` installed and the unit reachable). Without it a conservative default set is
used.

The response carries the bearer token — it is only ever returned while in provisioning mode. The
device then reboots and joins your network. The Midea cloud is never contacted from the ESP32.

> The blob contains your Wi-Fi password and the Midea token/key. Treat it like a password file.

## REST API

All routes require `Authorization: Bearer <token>`, except during provisioning where the device is
serving its own private AP.

```
GET   /api/health
GET   /api/units                      -> [{id, vendor, name, online}]
GET   /api/units/{id}                 -> normalised state
GET   /api/units/{id}/raw             -> the last raw vendor payload, for schema debugging
PATCH /api/units/{id}                 -> partial state; only the keys present are applied
POST  /api/units/{id}/actions/{name}  -> led_toggle | self_clean | clear_timer
GET   /api/config                     -> current config, secrets redacted
POST  /api/config                     -> provisioning; reboots on success
```

The two ACs stay entirely separate — separate ids, separate driver tasks, separate state, and (from
phase 4) separate accessories in Apple Home. "Normalised" refers only to the *shape* of this JSON:
both vendors report through one shared vocabulary so the REST and HomeKit layers need no per-vendor
code paths.

Fields a unit doesn't report are `null` rather than omitted, and `features` only lists what the
driver says it supports:

```json
{
  "id": "midea", "vendor": "midea", "name": "Stue", "online": true,
  "power": true, "mode": "cool", "target_temp": 22.0,
  "indoor_temp": 24.5, "outdoor_temp": 31.0, "humidity": 48,
  "fan": "auto", "swing": "vertical",
  "features": { "eco": false, "boost": false, "sleep": false, "led": true },
  "min_target": 16.0, "max_target": 30.0,
  "age_ms": 4200, "error": null
}
```

Writes are queued to the unit's task and answered `202 Accepted`; poll the unit to see the result.

```bash
curl -H "Authorization: Bearer $TOKEN" http://esp32-ac.local/api/units/midea

curl -X PATCH -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
     -d '{"mode":"cool","target_temp":21.5,"fan":"high"}' \
     http://esp32-ac.local/api/units/midea

curl -X POST -H "Authorization: Bearer $TOKEN" \
     http://esp32-ac.local/api/units/midea/actions/led_toggle
```

`mode` is `auto|cool|dry|heat|fan` and `swing` is `off|vertical|horizontal|both` — both vendors cover
exactly these. `fan` accepts a preset name (`auto|silent|low|medium|high|turbo`) or an integer
percentage 1–100, and reports back whichever form the unit itself uses; units with custom fan-speed
support report a raw percentage rather than a named step.

Unknown enum values are rejected with `400`. The Python original did the opposite — an unrecognised
name made `dict.get()` return `None` and sent a literal `{"ac_mode":None}` to the device with no
error (`electrolux/cli.py:108-148`).

## Design notes

**One task per unit.** The C6 has a single high-performance core, so a blocking socket read on
`loop()` would stall HomeSpan's HAP polling. Every device call happens on that unit's own FreeRTOS
task; REST and HomeKit only read a mutex-guarded snapshot and push onto a command queue. That also
means two callers can never race on one device session — which matters, because both vendors'
modules tolerate only one session at a time and the phone apps can hold it.

**Failures back off.** A unit that doesn't answer is marked `online: false` with its last readings
still visible, its session is dropped, and the retry interval doubles from 5 s to a 120 s ceiling
rather than hammering a device whose session is held elsewhere.

**Capabilities are baked in, not probed.** `tools/export_config.py --probe` reads them once from the
Midea unit and stores them in NVS, so the firmware never needs an on-device capabilities parser.
`mpsac` already treats capabilities as best-effort (`mpsac/device.py:69`).

## Apple Home

The device pairs as a **bridge** carrying one accessory per AC, so the two units appear in the Home
app as two independent air conditioners with their own rooms and automations. Pair with HomeSpan's
default code `466-37-726` unless you change it.

The HAP category is `Bridges`, not `AirConditioners`. The category is device-wide and only sets the
icon shown *while pairing* — as HomeSpan's own bridge example puts it, it "does NOT change the icons
of the Accessory Tiles". The air-conditioner look and controls come from each accessory's
`HeaterCooler` service, which is marked as the primary service. Choosing `Bridges` additionally lets
each accessory be renamed independently and lists the device on the Home app's Hubs & Bridges page.
`AirConditioners` would be the right pick only for one ESP32 per unit.

| HomeKit | Midea | Electrolux |
|---|---|---|
| `Active` | `power_state` | `ac_pwr` |
| `CurrentTemperature` | `indoor_temperature` | `envtemp` |
| `TargetHeaterCoolerState` | `operational_mode` | `ac_mode` |
| `CurrentHeaterCoolerState` | derived from setpoint vs. room | derived |
| `Cooling`/`HeatingThresholdTemperature` | `target_temperature` | `temp` |
| `RotationSpeed` | `fan_speed` | `ac_mark` |
| `SwingMode` | `swing_mode` | `ac_vdir` |

Two places where HomeKit's model and the hardware's don't line up, and what this does about it:

- **One setpoint, two thresholds.** HomeKit's AUTO mode shows separate heating and cooling
  thresholds; these units have a single target. Writing either threshold sets the target, and both
  are kept equal on read. Their ranges are widened from HomeKit's defaults (cooling 10–35, heating
  0–25) to whatever the unit reported to `mpsac` — which is why `export_config.py` stores them.
- **No dry or fan-only mode, and no automatic fan.** HomeKit has no vocabulary for these, so each
  unit also gets plain switches: **Dry**, **Fan Only**, **Fan Auto**, plus one per supported feature
  (ECO, Boost, Sleep, Ion, Freeze Protection, Follow Me, Self Clean) and **Display**. The Midea's
  display is a flip rather than a settable flag, so its switch sends the toggle action and only when
  the state would actually change.

`CurrentHeaterCoolerState` is derived rather than reported: neither protocol says whether the
compressor is running, so it is inferred from the setpoint against the room temperature.

## Licence

MIT.
