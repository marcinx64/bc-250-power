# Final firmware specification

Project folder: `firmware/bc250_controller/` (PlatformIO, plain ESP-IDF
6.1.1, board `seeed_xiao_esp32c3`; see `docs/ESP_IDF_BUILD.md`). The bench
test firmware in `firmware/bc250_test/` is separate.

## 1. Hardware rules (must hold)

Pins (see `docs/HARDWARE.md`):

| Name | GPIO | Dir | Meaning |
|---|---|---|---|
| PSON | 3 | out | HIGH = ATX supply on |
| BUZZER | 4 | out | HIGH = sound (active buzzer) |
| SENSE | 5 | in, pull-up | LOW = BC-250 power LED lit |
| PWR | 6 | out | HIGH = power button pressed |
| RST | 7 | out | HIGH = reset button pressed |
| SWITCH | 20 | in, pull-up | LOW = front button pressed |
| LED | 10 | out, PWM | front button LED, duty 0..255 |

- First thing at start (`app_main`): PSON, PWR, RST, BUZZER, LED as outputs, LOW.
- Never touch GPIO2, GPIO8, GPIO9 (strapping) or GPIO21.
- Never start UART0 (its RX is GPIO20). The console is USB-Serial-JTAG only.
  Logging to it is fine and must never block when no host is attached.
- Buzzer pin: `gpio_set_drive_capability(GPIO_NUM_4, GPIO_DRIVE_CAP_3)`.
  Without it beeps fail.
- A reset of the ESP32 that is not prepared drops PSON and cuts the BC-250's
  power (crash, watchdog, brown-out, loss of +5VSB). A deliberate restart
  or update keeps the BC-250 on by latching PSON, section 17; it is refused
  only in `starting` and `shutting_down`.
- The control task never blocks: no delay longer than a few ms, WiFi
  connect and WiFi scan are asynchronous, beeps and pulses are timed with a
  millisecond clock. Button and power logic must keep working when WiFi is down.

## 2. Power state machine

"Running" is read from SENSE, debounced:
- lit: SENSE low continuously for 200 ms
- dark: SENSE high continuously for `off_detect_ms` (default 3000)

States: `off`, `starting`, `on`, `shutting_down`.

At boot the state is `off` (outputs are low, so the supply is off).

### off
- PSON low. LED breathes slowly (section 4).
- Front button pressed (acts on the press, debounced 30 ms), web "power
  on", or a Bluetooth wake: go to `starting`. That press is then ignored
  until released, however long it is held.

### starting
- 1 beep. PSON high.
- If `start_pwr_pulse` is true and SENSE is not lit after
  `start_pwr_delay_ms` (default 2000): pulse PWR for 500 ms, once.
- SENSE lit: go to `on`.
- Not lit after `start_timeout_ms` (default 15000) from entry: PSON low,
  error signal (one 1 s beep, LED fast blink for 3 s), go to `off`, log it.

### on
- LED steady at `led_on`.
- SENSE dark (debounced): the operating system shut the board down.
  PSON low, 1 beep, go to `off`.
- Front button, measured from the debounced press:
  - released before `hold_reset_s` (default 3 s): 1 beep, pulse PWR 500 ms,
    go to `shutting_down`
  - at `hold_reset_s` still held: 2 beeps at once (tells the user to let go)
  - released between `hold_reset_s` and `hold_force_s` (default 5 s): pulse
    RST 500 ms, stay `on`
  - at `hold_force_s` still held: 3 beeps and PSON low at once, go to `off`.
    Do not wait for release; ignore the button until it is released.

### shutting_down
- LED breathes fast.
- SENSE dark (debounced): PSON low, go to `off`.
- Still lit after `shutdown_timeout_ms` (default 90000): go back to `on`,
  log it. Never force power off on a timeout.
- A short press does nothing here.

### In `starting` and `shutting_down`
- Holding the button `hold_force_s` (default 5 s) forces power off exactly as in `on`
  (3 beeps, PSON low). The reset hold applies only in `on`.

### Automatic OS updates
The controller can start the BC-250 at night, have it update itself over
SSH and shut it down again (section 26). Those power-on and shutdown
requests go through the same state machine as the web actions. They are
not user actions, and while the job is silent the buzzer and the LED
behave as described in sections 3 and 4.

### Web actions
`on` (only from `off`), `shutdown` (only from `on`: same as short press),
`reset` (only from `on`: 2 beeps + RST pulse), `force_off` (from any state
but `off`: 3 beeps, PSON low). A refused action returns an error.

## 3. Beeps

Active buzzer, fixed pitch: 250 ms on, 200 ms gap. 1 = power on or off,
2 = reset, 3 = forced off, one 1 s beep = error. `buzzer` setting silences
all of them. Quiet hours (section 19) and the silent mode of an automatic OS
update (section 26) keep the buzzer from sounding at all; the sequences run
as usual.

## 4. Front button LED

Gamma: output = (level squared), as in the test firmware.

| State | Effect |
|---|---|
| off | breathing, period `led_off_period_ms` (default 6000), peak `led_off_max` (default 120) |
| starting, shutting_down | breathing, period 1000 ms, peak `led_on` |
| on | steady `led_on` (default 255) |
| error | 5 Hz blink for 3 s, then the state's effect |

While an automatic OS update is silent (section 26) the LED breathes as in
`off` in every state, with no error blink. A user action (front button,
web power action, wake) ends that at once.

## 5. WiFi

- Credentials in NVS. Hostname setting, default `bc250`,
  used for DHCP and mDNS (`http://bc250.local`).
- No credentials: access point `BC250-Setup`, WPA2 password `bc250setup`,
  address 192.168.4.1, with a captive portal (DNS answers everything with
  192.168.4.1) that opens the setup wizard `ui/setup.html`.
- Credentials present: station mode. If not connected 30 s after boot or
  after a drop, also start the access point (AP+STA) and keep retrying.
  Stop the access point 60 s after the station link is up again.
- WiFi modem sleep must stay enabled (needed for Bluetooth coexistence).
- Connect speed: connect scans (the first connect, our reconnects) take
  30..70 ms per active channel and 150 ms per passive one
  (`esp_wifi_set_scan_parameters`, after `esp_wifi_start()`), and DHCP does
  not ARP-probe the address it is given. The station has its address about
  1.9 s after a reset; the web page answers about 2.5 s after it.
- Roaming: between the access points of one network (same
  SSID, 802.11k/v/r). The station is set up with radio measurement (k), BSS
  transition (v) and fast transition (r) enabled, and no fixed BSSID or
  channel. The IDF roaming app runs event-driven, without periodic scans:
  - Trigger: the signal falls below -72 dBm. The app then scans the channels
    once (active, 30..70 ms per channel with returns to the home channel).
    Only an AP at least 10 dB stronger than the current one is taken. If
    none is, the threshold drops by 5 dB (-77, -82, ...) for the next try and
    returns to -72 when the signal recovers. At least 30 s lie between two
    roam attempts. While the signal stays below -70 dBm, `wifi_mgr` sets the
    threshold to -70 every 30 s (`esp_wifi_set_rssi_threshold`), so the
    driver posts the weak-signal event again and the app looks again: a
    controller carried next to another AP while the old one still reaches it
    moves within about 30 s. These repeated events are not logged.
  - Method: first a BSS transition query to the AP (802.11v; the router
    decides), twice at most; then, if the AP does not move the station, a
    directed reconnect to the better AP. The AP can also move the station on
    its own (BSS transition request, 802.11r fast transition).
  - IP: after a roam done by the stack (BSS transition, 802.11r; disconnect
    reason ROAMING) the IP stack is left alone, so the DHCP lease or fixed
    address and open connections stay. After the app's directed reconnect the
    station asks DHCP again (normally the same address; a fixed address is
    kept).
  - Link accounting in `wifi_mgr`: a disconnect with reason ROAMING or BSS
    transition is not a drop. No reconnect of ours runs for 10 s, "wifi
    dropped" is not logged and the setup AP does not start. If no
    connection follows within 10 s it counts as a drop. A reconnect of ours
    is also not sent while the station is associated but waits for its IP;
    after 20 s without IP the station is disconnected and reconnected.
  - Pinned AP: a roam sets its target AP (BSSID and channel) in the station
    config. Our reconnect clears that pin before it connects, so it searches
    every channel for the strongest AP. The driver refuses both while a
    connect is running, and the roaming app starts a new connect after every
    failed one: with the pinned AP out of reach the station can keep trying
    it. When our reconnect has been refused for 15 s, `wifi_mgr` stops the
    running connect (`esp_wifi_disconnect()`, which also stops the app's own
    reconnects until ours), clears the pin and connects again; logged once
    per outage.
  - Pause: `wifiUpdate()` (every 100 ms) switches roaming off while a
    firmware upload runs (`webOtaRunning()`), while 2.4G Learn or the
    Bluetooth device scan runs (`bleDiscoveryActive()`) and while the WiFi network scan runs
    (`wifiScanning()`), and on again afterwards (`wifiRoamAllow()`). Paused
    means: the app's weak-signal trigger, periodic scan, BSS transition
    query and directed reconnect are off (its config via
    `roam_set_config_params`) and its own reconnect after a disconnect is off
    (`roam_disable_reconnect`). Not pausable: a roaming scan already running
    (ends within about 2 s, its result is not acted on), and a BSS
    transition or 802.11r move that the AP starts itself (the station follows
    it). The connection recovery of section 5
    (our own reconnect) stays on.
  - 2.4G capture: the rf task does not visit channels while
    `wifiRadioBusy()` is true (our WiFi scan; 4 s after a weak-signal event,
    when the roaming scan runs; 1.5 s around an association or a roam, and a
    roam until it has ended or counts as a drop), and
    for 2 s after the driver refused a channel visit (it does so while the
    station is connecting).
  - Log: `wifi: signal -76 dBm, looking for a better AP` (weak-signal event),
    `wifi roamed to aa:bb:cc:dd:ee:ff ch 6 (-78 -> -52 dBm)` and
    `wifi: connecting stuck, starting over` (pinned AP, above).
- The full UI is served in both modes.
- Access is protected by a password, section 12.

## 6. Bluetooth wake

- NimBLE host, observer role only (scan, no connections).
- While the power state is `off` and no discovery runs, the scan window is
  160 ms in every 200 ms, so a controller is noticed sooner. Not while the
  web UI was used in the last 20 s or a firmware upload runs: then the 60 ms
  window applies, so WiFi keeps the radio.
- Background scan: passive, window 60 ms in every 200 ms, in 5 s chunks,
  results cleared between chunks. Runs always when `ble_wake` is on or a
  discovery is active.
- Wake list: up to 8 entries {address, address type, name}, in NVS.
- Each entry tracks last-seen time and RSSI. "Present" = seen in the last
  5 s.
- Arming: on entering `off` (and at boot) every entry is disarmed. An
  entry arms after it has not been seen for `ble_rearm_s` (default 30).
  A sighting of an armed, enabled entry in state `off` starts the BC-250, at
  any signal strength (a `ble_min_rssi` value in a settings request is
  ignored). This stops a
  controller that is still on from restarting the BC-250 after shutdown.
- Discovery (for the UI): active scan until `POST /api/ble/scan/stop` or
  for the scan time (`scan_s`), as 2.4G Learn, collecting up to 40
  devices {address, type, name, RSSI}, strongest first. The results go
  when it ends: `/api/ble/devices` lists them only while scanning. Wake
  matching keeps working during discovery.
- Scan callbacks run on the Bluetooth task: only copy small data there,
  guard shared data, do the logic in `loop()`.

## 7. Firmware update (OTA)

- Partition table `partitions.csv` (two 1.9 MB app slots).
- `POST /api/update`, multipart upload of `firmware.bin`, written with the
  ESP-IDF OTA API. Refused with 409 unless state is `off` (section 17 adds
  `on`) and while an automatic OS update runs (section 26). Reboots on success.
- Version string `FW_VERSION` in one header, shown in the UI.

## 8. Settings (NVS)

| Key | Default | Range |
|---|---|---|
| hostname | bc250 | 1..31 chars, a-z 0-9 - |
| buzzer | true | |
| led_on | 255 | 0..255 |
| led_off_max | 120 | 0..255 |
| led_off_period_ms | 6000 | 1000..20000 |
| off_detect_ms | 3000 | 500..30000 |
| start_pwr_pulse | true | |
| start_pwr_delay_ms | 2000 | 500..10000 |
| start_timeout_ms | 15000 | 3000..120000 |
| shutdown_timeout_ms | 90000 | 10000..600000 |
| ble_wake | true | |
| ble_rearm_s | 30 | 3..600 |
| scan_s | 60 | 30..600; length of a Bluetooth scan and of 2.4G Learn, s |

## 9. HTTP API

JSON everywhere. POST bodies are JSON (`Content-Type: application/json`)
except the firmware upload. Every POST answers `{"ok":true}` or
`{"ok":false,"error":"text"}` with a 4xx status.

| Method, path | Body | Answer |
|---|---|---|
| GET `/` | | `ui/index.html` (in AP-only setup mode: `ui/setup.html`) |
| GET `/setup` | | `ui/setup.html` always |
| GET `/api/state` | | see below |
| POST `/api/power` | `{"action":"on"}` (on, shutdown, reset, force_off) | ok |
| GET `/api/config` | | all settings of section 8 as one object |
| POST `/api/config` | any subset of the settings | ok (validated, saved, applied live; hostname applies after restart) |
| POST `/api/config/reset` | | ok: every setting back to its default, saved and applied live; `hostname` and `ble_wake` kept; the password, WiFi, IP settings, wake method and wake lists are not settings and stay |
| GET `/api/log` | | `{"now":123456,"events":[{"t":120034,"msg":"power on: button"}]}` newest first, last 40, `t` and `now` are ms since boot |
| GET `/api/ble/wake` | | `{"list":[{"addr":"e4:00:00:00:00:10","type":"public","name":"8BitDo","present":true,"rssi":-60,"armed":false,"enabled":true,"last_s":4}]}`; `last_s`: seconds since last heard, -1 if not since the start |
| POST `/api/ble/wake/add` | `{"addr":"..","type":"public","name":".."}` | ok |
| POST `/api/ble/wake/remove` | `{"addr":".."}` | ok |
| POST `/api/ble/wake/rename` | `{"addr":"..","name":".."}` | ok; name up to 31 characters |
| POST `/api/ble/wake/enable` | `{"addr":"..","enabled":false}` | ok; a disabled device is still heard and listed but never wakes the BC-250 |
| POST `/api/ble/scan` | | ok, starts a discovery (until stop or the scan time) |
| POST `/api/ble/scan/stop` | | ok, ends the discovery; its results go |
| GET `/api/ble/devices` | | `{"scanning":true,"left_s":97,"devices":[{"addr":"..","type":"public","name":"","rssi":-70,"in_list":false}]}` |
| POST `/api/wifi/scan` | | ok, starts an async scan, one channel at a time (a whole-band scan would keep every network seen in RAM) |
| GET `/api/wifi/networks` | | `{"scanning":false,"networks":[{"ssid":"..","rssi":-50,"secure":true}]}` strongest first, no duplicates |
| POST `/api/wifi` | `{"ssid":"..","pass":".."}` | ok; saves and starts connecting, no reboot |
| POST `/api/wifi/forget` | | ok; clears credentials, back to setup AP |
| POST `/api/update` | multipart, field `firmware` | ok then reboot; 409 unless `off`, and 409 while an automatic OS update runs (section 26) |
| GET, POST `/api/osupdate`, POST `/api/osupdate/run`, `/test`, `/stop`, `/forget_host` | | automatic OS updates, section 26 |
| GET `/bc250-setup.sh`, `/bc250-update.sh` | | the scripts for the BC-250, no login, 404 while automatic OS updates are off (section 26) |
| POST `/api/restart` | | ok then reboot; 409 unless `off` |
| POST `/api/factory_reset` | `{"confirm":true}` | same gate as `/api/restart`; ok, then the whole NVS partition is erased (settings, password, WiFi, both wake lists, wake method) right before the restart; the controller comes back in setup mode. 400 without confirm |

`type` is `"public"` or `"random"`. Addresses are lower-case with colons.

`GET /api/state`:

```json
{
  "fw": "2.2.0",
  "state": "off",
  "sense": false,
  "pson": false,
  "pwr": false,
  "rst": false,
  "button": false,
  "button_held_ms": 0,
  "button_live": false,
  "error": false,
  "on_s": null,
  "off_s": 5400,
  "off_since_start": false,
  "uptime_s": 1234,
  "heap": 150000,
  "heap_total": 280000,
  "chip_temp_c": 41.5,
  "cpu": 23,
  "wifi": {"mode": "sta", "connected": true, "ssid": "Home", "ip": "192.168.1.50", "rssi": -55, "ap": false, "hostname": "bc250"},
  "ble": {"wake": true, "scanning": false, "adverts": 1234}
}
```

`wifi.mode` is `"sta"`, `"ap"` or `"ap+sta"`; `wifi.ap` is true while the
access point is up; `wifi` also has `bssid` (the AP's
address, `""` when not connected), `channel` (0 when not connected), `roams`
(roams since start) and `roam` (false while roaming is paused); `ble.scanning` is true during a discovery;
`ble.adverts` counts every advert heard. `on_s`, `off_s` and
`off_since_start` are described in section 25.

## 10. Source layout and ownership

```
firmware/bc250_controller/
  platformio.ini
  src/            C++ sources (main.cpp plus modules)
  components/monocypher/  Monocypher, unmodified (Ed25519, X25519, ChaCha20, Poly1305 for the SSH client)
  ui/index.html   main UI, one self-contained file (CSS and JS inline)
  ui/setup.html   WiFi setup wizard, one self-contained file
```

The two HTML files are gzip-compressed at build time by
`touch_buildinfo.py` (into `ui/gz/`, not committed), embedded with
`board_build.embed_files` and served from flash with
`Content-Encoding: gzip` (index.html 101 KB -> 32 KB, so a page load holds
network buffers for a quarter of the time). They must work with no internet access: no CDN, no
external fonts, images or scripts.

## 11. Open items

- Whether the controller still advertises with its public address when it
  is bonded to the BC-250 and that BC-250 is off.
- Whether the BC-250 power LED is steady while running (a blinking LED
  needs a longer `off_detect_ms`).
- BC-250 AUTO_PWRON1 jumper position, which decides `start_pwr_pulse`.
- The password and session cookie travel over plain HTTP (no TLS).

## 12. Password

One password protects the UI and the API.

- Stored in NVS as a random 16-byte salt plus SHA-256(salt + password),
  never in clear. Length 8..64 characters.
- Factory state: no password set. Then everything is open, and both the
  setup wizard and the main UI ask the user to set one (the wizard as a
  required step before the WiFi step, the main UI as a blocking prompt).
- Password set: every `/api/*` request needs a valid session, except
  `GET /api/auth`, `POST /api/login`. Without one the answer is 401
  `{"ok":false,"error":"login required"}`. The HTML pages themselves and
  the captive-portal redirects are always served; the pages show a login
  form when needed.
- Session: `POST /api/login` with the right password answers ok and sets
  `Set-Cookie: sid=<32 random hex chars>; Path=/; HttpOnly; SameSite=Strict`.
  Up to 4 sessions, oldest dropped. They are kept in RTC memory
  (src/auth.cpp, `RtcAuth`: magic, FNV-1a check value and the first 8 bytes
  of the stored password hash) and taken over at start only after a software
  restart (`ESP_RST_SW`: web restart, OTA update, wake method switch, factory
  reset finds no password); a power loss, crash, watchdog or reset button
  clears them, as does a password change for every session but the
  caller's. Random
  values from `esp_random()`. Compare hashes in constant time.
- Wrong password: 401. After 5 wrong attempts in a row, logins are
  refused with 429 for 30 s (timed with `millis()`, no blocking delay).
- Forgotten password: hold the front button while the board powers up
  and keep it held for 10 s. The board gives 5 beeps with 5 LED flashes in
  step and erases the
  password and the WiFi credentials (other settings stay). A button that
  is already pressed at boot never powers the BC-250 on.

| Method, path | Body | Answer |
|---|---|---|
| GET `/api/auth` | | `{"password_set":true,"authed":false,"locked_s":0}` (always open) |
| POST `/api/login` | `{"password":".."}` | ok + cookie, 401, or 429 |
| POST `/api/logout` | | ok, session removed |
| POST `/api/auth/password` | `{"old":"..","new":".."}` | ok. `old` is not needed while no password is set; otherwise a session and the right `old` are required. Other sessions are dropped. |

## 13. Debug mode: spoofed LED sense

For bench work without a BC-250. Held in RAM only: always `real` after a
restart.

| Mode | Sense value the state machine sees |
|---|---|
| `real` | the SENSE pin (default) |
| `lit` | always lit |
| `dark` | always dark |
| `sim` | a simulated BC-250: lit 3 s after PSON goes high; a PWR pulse while lit makes it dark 5 s later; a RST pulse makes it dark for 1 s; PSON low makes it dark at once |

The spoofed value goes through the same debouncing as the real pin.
Changing the mode is logged.

Danger, to be stated in the UI: with a real BC-250 attached, `dark` (or
`sim`) while the BC-250 runs makes the controller cut its power.

- `POST /api/debug` with `{"sense":"real"}` (real, lit, dark, sim): ok.
- `GET /api/state` gains `"sense_raw": false` (the real pin) and
  `"debug": {"sense": "real"}`. `"sense"` stays the value the state
  machine uses.

## 14. Device kind in discovery

So the UI can list only game controllers, discovery records two fields
from the advertising and scan-response data of each device:

- Appearance (AD type 0x19, 16-bit), 0 when absent.
- Whether the HID service UUID 0x1812 is in any 16-bit service UUID list
  (AD types 0x02, 0x03).

Both can arrive in different packets (advert and scan response), so they
are merged per device over the scan, never cleared by a later packet.

`kind` is derived from them:

| kind | Rule |
|---|---|
| `gamepad` | appearance 0x03C4 |
| `joystick` | appearance 0x03C3 |
| `keyboard` | appearance 0x03C1 |
| `mouse` | appearance 0x03C2 |
| `hid` | any other appearance in the HID category 0x03C0..0x03FF, or HID service present with no more specific appearance |
| `other` | everything else |

Each entry of `GET /api/ble/devices` gains `"kind":"gamepad"`,
`"appearance":964` (number, 0 when absent) and `"hid":true`.

"Controller-like" means kind `gamepad`, `joystick` or `hid`. When the
40-entry discovery table is full, a controller-like device replaces the
weakest entry that is not controller-like. The UI shows only
controller-like devices unless "Show all devices" is on.

Wake matching is unchanged: it is by address only.

## 15. Build and device information

`GET /api/info` (login required) returns facts for the Controller page (Hardware and Firmware cards):

```json
{
  "fw": "2.2.0",
  "build": "2026-11-15 02:53:59",
  "idf": "6.1.1",
  "chip": "ESP32-C3",
  "chip_rev": 4,
  "cpu_mhz": 160,
  "mac": "A0:00:00:00:00:94",
  "flash_size": 4194304,
  "app_size": 1447840,
  "app_slot": 1966080,
  "partition": "app0",
  "heap": 150000,
  "heap_min": 120000,
  "heap_total": 280000,
  "chip_temp_c": 41.5,
  "cpu": 23,
  "uptime_s": 1234,
  "reset_reason": "firmware update"
}
```

`build` is the compile time of this build in the build machine's local
time. `app_size` is the size of the running firmware and `app_slot` the
size of one firmware slot, both in bytes. `partition` is the slot the
firmware runs from (`app0` or `app1`; it alternates with each OTA
update). `reset_reason` says why the controller last started. One of: power
connected, firmware update, restart from the web UI, software restart,
crash, watchdog, brown-out, reset over USB, deep sleep, other.

Health figures in `/api/state` and `/api/info`, measured every
5 s by the main task (src/sysmon.cpp):
- `heap_total`: size of the heap that `heap` and `heap_min` count, bytes.
  The UI shows memory use as (total - free) / total, with a mark at the
  highest use since start (from `heap_min`).
- `chip_temp_c`: the ESP32-C3's internal temperature sensor, the die
  temperature, not the room (about +-1..3 C in -10..80 C, Espressif); one
  decimal; `null` until the first reading.
- `cpu`: CPU use in percent over the last 5 s, 100 minus the idle task's
  share (FreeRTOS run-time statistics, esp_timer clock); `null` until the
  first 5 s have passed. A 2.4G capture counts as busy.
The UI hides those rows when a device does not send them.

## 16. Firmware file marker

Every firmware image contains the text
`@@BC250FW|<version>|<build time>|@@`, for example
`@@BC250FW|2.2.0|2026-11-15 03:20:00|@@`. The UI searches a chosen
`firmware.bin` for it in the browser, before anything is uploaded, to show
what the file is.

Other checks the UI can make on the file without the device: byte 0 is
0xE9 (ESP image), the 16-bit little-endian chip id at offset 12 is 5
(ESP32-C3), and the size fits `app_slot`.

`GET /api/info` also returns the running firmware's marker as `"marker"`.

## 17. Keeping the BC-250 on through a controller restart

Without this, every restart of the ESP32 releases PS_ON and cuts the BC-250's
power, and `setup()` drives PSON low. This section adds a way to restart or
update the controller under a running BC-250.

Tested on a board with the BC-250 attached: a restart from the web UI and an
OTA update both kept the BC-250 running.

Setting: `pson_hold` (bool, default true), in NVS like the
others. It has no control in the UI any more; it can still be set through
`POST /api/config`.

Before a deliberate restart (web restart or the restart after an update)
with `pson_hold` true and power state `on`:

1. PSON is high. Latch the pad with `gpio_hold_en(GPIO_NUM_3)`.
2. Store a marker in RTC memory (survives a software reset): "BC-250 was on".
3. Restart.

At boot, as the very first thing, before any pin is configured:

- If the reset reason is a software restart and the marker is valid: this
  is a resume. Clear the marker. Configure PSON as an output with level
  HIGH while the pad is still latched, then release the latch with
  `gpio_hold_dis`, so the pin never leaves HIGH. The other outputs start
  LOW as always. The power state machine starts in `on` with the sense
  treated as lit; the normal rule then applies (sense dark for
  `off_detect_ms` → PSON low, state `off`). Log "resumed: PS_ON held
  through the restart".

  The outcome of the resume is `pending` until one of:
  - the sense is lit for 200 ms: `kept`, log "resumed: BC-250 kept on";
  - the state leaves `on` first: `lost`. When the sense was dark for
    `off_detect_ms` (the BC-250 lost power during the restart, or nothing
    lit the sense, as with the debug sense back on `real` after the boot),
    log "resume failed: power LED dark, BC-250 not kept on" instead of
    "off: BC-250 shut down".
- Otherwise (power-up, crash, watchdog, brown-out, no marker): release any
  latch left on the pad with `gpio_hold_dis` and start as before, PSON
  low, state `off`. The marker is only trusted once.

The latch is enabled only for the moment of a deliberate restart. It is
not enabled during normal running, so a crash still drops PSON (a crashed
controller must not hold a BC-250 on with nobody watching the button).

While the controller restarts (about a second, longer after an update) the
front button and the sense are not served.

API changes:

- `POST /api/restart` and `POST /api/update` are accepted in state `off`
  (as before) and, when `pson_hold` is true, also in state `on`. Still
  refused with 409 in `starting` and `shutting_down`, and in `on` when
  `pson_hold` is false. While an update is in progress with the BC-250 on, the
  power state machine keeps running (button shutdown, reset and forced off
  still work); if the state leaves `on` before the restart, the restart
  proceeds without the latch.
- `pson_hold` is part of `GET/POST /api/config`.
- `GET /api/state` gains `"hold": false` (the setting),
  `"resumed": false` (true if this boot was a resume, i.e. PS_ON was held
  through the restart) and `"resume": "none"` (the outcome: `none` when
  this boot was not a resume, `pending`, `kept` or `lost`).
- `GET /api/info` `reset_reason` keeps its value; `"resumed"` and
  `"resume"` are there too.
- The UI says the BC-250 was kept on only for `kept` ("Started after" on
  the Controller view: "(BC-250 kept on)", "(BC-250 not kept on)" as a
  warning, "(checking whether the BC-250 stayed on)" while `pending`). The
  toast after a restart or update waits while `pending`. Firmware without
  `resume`: `resumed` alone.

The pad latch survives a software restart on the ESP32-C3 and the restart
after an OTA update, with the BC-250 staying up. PS_ON# has not been looked at
on a scope.

## 18. Clock and timezone

The controller keeps wall-clock time for the quiet hours (section 19) and
for the automatic OS updates (section 26).

- Source: SNTP over WiFi, started whenever the station link comes up.
  Setting `ntp_server` (default `pool.ntp.org`, 1..63 chars, host name or
  IPv4). Non-blocking; the time is "synced" once it is plausible (after
  2024-01-01).
- Fallback: `POST /api/time` with `{"epoch": 1794700000}` (UTC seconds)
  sets the clock when it is not synced by SNTP, so the UI can hand over the
  browser's time when the controller has no internet (setup AP). Ignored
  with ok when SNTP has already synced.
- The clock is kept through a restart of the controller (restart, update)
  by the chip's internal RTC timer, as long as the board stays powered. It
  is lost only when the board loses power (+5VSB); there is no battery.
  After a restart the time counts as set at once and is corrected by the
  next SNTP sync.
- `time.source` in `/api/state` says where the time comes from: `ntp`,
  `browser`, `kept` (carried over a restart, not yet confirmed by SNTP
  since) or `none`.
- Timezone: setting `tz` holds a POSIX TZ string, which carries the
  daylight-saving rules, so the change of clocks is automatic (for example
  `CET-1CEST,M3.5.0,M10.5.0/3`). Setting `tz_name` holds the IANA name it
  was chosen from (for example `Europe/Warsaw`), for display only.
  Defaults: `tz` = `UTC0`, `tz_name` = `` (empty = never chosen).
  `tz` is 1..47 chars from the set A-Z a-z 0-9 `+ - , . / : < >`;
  `tz_name` is 0..39 chars.
- The UI carries a table of IANA zones with their POSIX strings and sets
  both keys. When `tz_name` is empty it sets them once from the browser's
  timezone without asking.
- `GET /api/state` gains
  `"time": {"synced": true, "epoch": 1794700000, "local": "2026-11-15 18:02:11", "tz_name": "Europe/Warsaw", "quiet": false}`.
  `local` is empty when not synced. `quiet` is true while the quiet hours
  are in effect right now.

### When SNTP is used (not all the time)

The chip keeps the time itself; SNTP is only asked when there is a reason:

- The clock is not set (after a power loss), or was only set from a
  browser: ask as soon as the station link is up, and keep trying until
  one answer arrives.
- The clock is set (`ntp` or `kept`): do not ask at link-up. Ask once
  every `ntp_check_h` hours (setting, default 24, range 0..720; 0 = never
  again after the first answer) to correct drift, counted from the last
  answer or, for a kept clock, from boot.
- `POST /api/time/sync`: ask now (ok if the station is connected, 409
  otherwise).
- After one answer the SNTP client is stopped until the next reason.
- `state.time` gains `"last_sync_s": 3600` (seconds since the last SNTP
  answer in this boot, -1 if none).

Daylight saving does not depend on SNTP: the clock runs in UTC and the
local time is worked out from the `tz` rule on the controller.

## 19. Quiet hours for the buzzer

Settings: `quiet_on` (bool, default false), `quiet_from` and `quiet_to`
(minutes after local midnight, 0..1439, defaults 1320 = 22:00 and
420 = 07:00).

- While `quiet_on` is true, the clock is synced and the local time is
  inside the window, the buzzer makes no sound at all: no beeps for power
  on, shutdown, reset, forced off, errors or the factory-reset gesture.
  Everything else (LED, actions, timing) is unchanged.
- The window may cross midnight (from 22:00 to 07:00). `quiet_from` equal
  to `quiet_to` means no quiet time.
- While the clock is not synced the quiet hours do not apply: the buzzer
  sounds.
- The control task must not call time functions. The loop task works out
  "quiet now" about once a second and publishes it as a flag the buzzer
  reads.

## 20. Network settings: fixed address

Stored with the WiFi credentials. Default is DHCP.

`GET /api/net`:

```json
{"dhcp": true, "ip": "", "mask": "", "gw": "", "dns1": "", "dns2": "",
 "cur": {"ip": "192.168.1.50", "mask": "255.255.255.0", "gw": "192.168.1.1", "dns1": "192.168.1.1", "dns2": ""}}
```

`cur` is what the station interface uses right now (empty strings when not
connected). The other fields are the stored configuration.

`POST /api/net` with `{"dhcp": false, "ip": "..", "mask": "..", "gw": "..", "dns1": "..", "dns2": ".."}`
or `{"dhcp": true}`:

- With `dhcp` false: `ip`, `mask` and `gw` are required dotted IPv4
  addresses; `mask` must be a contiguous netmask; `ip` must not be the
  network or broadcast address; `gw` must be in the same subnet as `ip`;
  `dns1` and `dns2` are optional (empty `dns1` means: use the gateway).
- A valid request is saved, answered with ok, and then applied by
  reconnecting the station (no restart of the controller, the BC-250 is not
  affected). The page may lose contact because the address changes.
- Recovery from a wrong fixed address: if the station is not connected the
  setup AP opens after 30 s as usual, and the UI is reachable at
  192.168.4.1. In addition, the forgotten-password gesture (button held 10 s
  at power-up) and "Forget network" reset the network settings to DHCP.
- `GET /api/state` `wifi` gains `"dhcp": true`.
- The setup wizard is unchanged (always DHCP).

## 21. Hold times and LED feedback while the button is held

The two hold thresholds of section 2 become settings:

| Key | Default | Range | Meaning |
|---|---|---|---|
| hold_reset_s | 3 | 2..30 | held this long in `on`: reset cue (2 beeps), release = reset |
| hold_force_s | 5 | 3..60 | held this long: forced power off |

`hold_force_s` must be at least 1 s more than `hold_reset_s`; a
`POST /api/config` that would break this (taking the stored value for a key
that is not in the request) is rejected. Section 2 uses these settings. The forgotten-password gesture at power-up
stays at a fixed 10 s.

LED while the front button is held and the press can still act
(`button_live` true), overriding the state's normal effect:

| Phase | LED |
|---|---|
| first second of the press | unchanged (a short press should not flicker) |
| after 1 s, until the reset threshold (state `on`) or the force threshold (`starting`, `shutting_down`) | slow flash, 2 Hz, at `led_on` brightness |
| state `on`, from the reset threshold until the force threshold | fast flash, 6 Hz |

On release, or once the forced power off has happened, the LED returns to
the effect of the current state. The error blink has priority over this.
Quiet hours do not affect the LED.

`GET /api/state` is unchanged; the UI reads the thresholds from
`GET /api/config`.

## 22. Dashboard summary fields

`GET /api/state` `ble` gains `"count"` (wake list entries), `"present"`
(of those, seen in the last 5 s) and `"armed"` (of those, able to start the
BC-250 now), so the dashboard does not have to poll the wake list.

## 23. Wake method: Bluetooth or 2.4G

The firmware has both radios. The wake method is stored in NVS
(`radio`/`m`) and applies from the next start: Bluetooth, or 2.4G/BT: a
device with a 2.4G USB dongle that speaks a protocol the firmware has a
detector for (Nordic Enhanced ShockBurst, 5-byte address), or a classic
Bluetooth controller paging its host. Design and measurements of 2.4G:
`docs/WAKE_2G4.md` and `docs/WAKE_BT_CLASSIC.md`; how detectors are added:
`docs/ADDING_PROTOCOLS.md`; the build: `docs/ESP_IDF_BUILD.md`.

- `POST /api/radio {"radio":"ble"|"2g4"}` stores the method and restarts;
  the BC-250's power state is unaffected. Each method keeps its own wake list(s)
  (NVS namespace `ble`; `rf` and `bth` for 2.4G/BT); all are kept.
- `GET /api/wake/lists` returns `{"ble":n,"dongle":n,"bt":n,"radio":"ble"}`:
  the saved entries of the three lists, whichever method runs.
- `POST /api/wake/clear {"list":"ble"|"dongle"|"bt"}` empties one list. The
  running method's list is cleared in memory and in NVS (wake stops at
  once); the other is erased from NVS. No restart. Logged as "... wake list
  cleared". With no method stored yet, the controller starts in
  2.4G mode if there is a 2.4G wake list, else Bluetooth.
- At start the unused radio's memory goes to the heap: in Bluetooth mode
  the 128 KiB 2.4G capture bank, in 2.4G mode the Bluetooth controller's.
- Both methods use the same settings: `ble_wake` and `ble_rearm_s`.
- `GET /api/state` `ble` and `GET /api/info` have `"radio"`: `"ble"` or
  `"2g4"`; `ble.max` is 8. In 2.4G mode `ble.scanning` means learning and
  `adverts` is 0.
- Arming (the same rule for Bluetooth and 2.4G): an entry arms after
  `ble_rearm_s` unheard, counted from entering off, from the controller's
  start, or from its last sighting; a newly added entry counts as just
  heard. `present` means heard in the last 30 s. An armed 2.4G/BT entry wakes
  on one **exact** hit (a CRC-valid frame, an error-free access code), or on
  two **approximate** hits within 5 s (a listed key with a few wrong bits:
  at most 2 of 48 for an ESB address, at most 3 of 64 for a Bluetooth sync
  word). The error budgets are per protocol (`rfp::Proto::budget`).
- In 2.4G mode the Bluetooth routes stay, but return empty lists, and
  adding answers with an error; the 2.4G routes exist only in 2.4G mode
  (login required).

| Route | Body | Answer |
|---|---|---|
| GET `/api/rf/wake` | | `{"list":[{"addr":"A1B2C3D4E5","kind":"dongle","proto":"esb1m","name":"Ultimate 2","mhz":2459.0,"present":true,"armed":false,"enabled":true,"last_s":12},{"addr":"9A8B7C","kind":"bt","proto":"btpage","name":"BC-250",...}]}`; `last_s` -1 = not heard since start; `kind` `dongle` or `bt` (absent = dongle); a `bt` entry has no `mhz`; `proto` is the detector's id (see below) |
| POST `/api/rf/wake/add` | `{"addr":"A1B2C3D4E5","mhz":2459.0,"name":"..","proto":"esb1m"}` | ok; `addr` 10 hex digits (a 2.4G device; `mhz` 2400..2483.5, for information) or a Bluetooth host: 6 hex digits (LAP) or a full Bluetooth address (12 hex digits, `:` allowed; its lower 3 bytes are kept), no `mhz`; `name` 1..31 characters; up to 8 2.4G devices and 2 Bluetooth hosts ("list full"). `proto` is optional: without it the entry gets the protocol Learn found that address with, else the first protocol of its list (`esb1m`, `btpage`); an unknown id gives "unknown protocol", an id of the other list "address does not fit that protocol" |
| POST `/api/rf/wake/remove` | `{"addr":".."}` | ok |
| POST `/api/rf/wake/rename` | `{"addr":"..","name":".."}` | ok; name 1..31 characters |
| POST `/api/rf/wake/enable` | `{"addr":"..","enabled":false}` | ok; as for Bluetooth |
| POST `/api/rf/learn` | `{"on":true,"kind":"dongle"}` | ok; starts (clearing old results) or stops; stops by itself after the scan time; `kind` `dongle` (default) learns 2.4G devices only (every protocol of that list), `bt` Bluetooth hosts only |
| GET `/api/rf/learn` | | `{"active":true,"kind":"dongle","left_s":97,"found":[{"addr":"..","kind":"dongle","proto":"esb1m","packets":12,"sessions":1,"in_list":false,"freqs":[{"mhz":2447.0,"packets":5,"sessions":1}],"fixed_mhz":2447.0}]}`; `fixed_mhz` is the most-used frequency once 3 packets were decoded, else null; `kind` is that of the running Learn; with `bt`: `{"addr":"9A8B7C","kind":"bt","proto":"btpage","packets":7,"ready":true,"freqs":[],"fixed_mhz":null}` for a paged Bluetooth host, ready from 3 packets |
| GET `/api/rf/snapshot?ch=6` | | Raw bytes, `application/octet-stream`, 65520 bytes = 16380 little-endian 32-bit words: I = signed bits 0..9, Q = minus the signed bits 10..19, 16 MS/s, 1 ms. Headers `X-Centre-Hz` (2407000000 + 5000000 x ch), `X-Sample-Rate`, `X-Format: c3-raw-10bit`, `X-Channel`. Login required. 400: ch outside the allowed channels; 409 with `error`: WiFi not a connected station, upload running, WiFi scanning or roaming, another snapshot in progress, or no snapshot within 2 s. The rf task takes it and keeps its buffer until the response is sent (at most 3 s after the last chunk); the route exists only with the 2.4G method |

Adding or renaming refuses a name another entry already has, ignoring case
("that name is already used"), for both methods; empty Bluetooth names may
repeat.

**Protocol ids.** Each wake list entry belongs to a detector (protocol
module, `src/rf_proto.cpp`). Built in: `esb1m` (Nordic ESB at 1 Mbit/s,
5-byte address, list `dongle`) and `btpage` (classic Bluetooth paging, LAP,
list `bt`); `esb2m` (ESB at 2 Mbit/s) only in an experimental build
(`-DRF_PROTO_ESB2M`). The `"proto"` fields are optional for clients and may be
ignored; all other fields and the `kind` values do not change when a
protocol is added.

**Storage.** Each list has its own NVS namespace, with the keys `n` (count),
`list` (records), `off` (bit mask: bit k = the k-th stored entry is disabled;
a disabled entry still shows as heard, never counts as armed and never
wakes) and `proto`:

| Namespace | List | Records | Limit |
|---|---|---|---|
| `rf` | 2.4G Dongle devices | address (5 bytes), frequency in kHz (u32), name (32) | 8 |
| `bth` | Bluetooth Classic hosts | LAP (3 bytes), name (32) | 2 (`/api/state` `ble.host_max`) |

`proto` is a string with the protocol ids of the entries, comma separated, in
the order of the records (`"esb1m,esb2m"`). Absent or empty: every entry has
the first protocol of its list, as stored by firmware that does not know the
key, which also ignores it. An id the running firmware does not know (an entry
saved by an experimental build) is kept, saved again unchanged and never
matched.

## 24. Translations of the web UI

The UI is in English; other languages are catalogs `firmware/bc250_controller/ui/i18n/<code>.json`
(guide: `docs/TRANSLATING.md`). The build turns every catalog into a gzip'd byte array
(`src/i18n_data.cpp`, generated by `touch_buildinfo.py`; entries with an empty translation and the
translators' names are left out) and the web server serves them. No NVS key is involved: the
language is chosen in the browser (`localStorage` key `bc250_lang`, else the browser's languages).

| Request | Answer |
|---|---|
| GET `/i18n/index.json` | the languages built in: `[{"code":"pl","name":"Polski","english_name":"Polish"}]`; English is not listed |
| GET `/i18n/<code>.json` | the catalog of that language: an object, key = the English text, value = the translation, or for plurals an object of forms (`one`, `few`, `many`, `other`, ...); 404 for an unknown code |

Both are served without login (the login page needs them), as `application/json` with
`Content-Encoding: gzip`, `Cache-Control: no-cache` and an `ETag`; a request with a matching
`If-None-Match` gets `304`. A plural text has the key `"<one>|<other>"` (the English singular and plural
sentence, `{n}` for the number). Texts of the API's own `error` field are looked up in the catalog too
(a firmware `%d` or `%s` is `{a}`, `{b}`, ... in the key); the event log stays English.

## 25. How long the BC-250 has been on or off

The controller keeps two times for the BC-250, in 64-bit milliseconds since
its own start (they do not wrap):

- started running: set when the state goes from `starting` to `on`
- stopped: set when the state goes from `on` or `shutting_down` to `off`
  (a shutdown, the sense going dark, a forced off)

A failed start (`starting` back to `off`, or a forced off in `starting`), a
reset pulse and a shutdown that times out (`shutting_down` back to `on`)
change neither time.

The current one, with "running or not" and "not run since the controller
was powered up", is kept in RTC memory, which keeps its contents through every reset
except a power-up: two slots written in turn, each with a sequence number and
a check value, so a reset during a write leaves the other one whole. The
control task rewrites it on every edge, once a minute, and right before a
deliberate restart (plain memory writes, never blocking; RTC memory is SRAM,
so writing it causes no wear).

At the controller's start the newest valid slot is taken over, except after
a power-up, an external reset or a wake from deep sleep (how long the power
was gone is unknown):

- resume (section 17), slot says running: still running, the elapsed time
  carries on.
- no resume, slot says running (crash, watchdog, brown-out with the BC-250
  on): the reset dropped PSON, so the BC-250 stopped at this start.
- slot says stopped: still stopped, the elapsed time and "not run since the
  controller was powered up" carry on.

Without a slot taken over the BC-250 counts as stopped at this start with
"not run since the controller was powered up" set (after a power-up it had no power
either), or as started running at this start on a resume. The restart itself
(about a second) is not counted; after a reset that is not a deliberate
restart, neither is up to the last minute of an off period.

`GET /api/state`:

- `"on_s"`: seconds since the BC-250 started running, while the state is
  `on` or `shutting_down`; `null` otherwise.
- `"off_s"`: seconds since it stopped, while the state is `off` or
  `starting`; `null` otherwise.
- `"off_since_start"`: true while it is not running and has not run since
  the controller was powered up; `off_s` then counts from that power-up, so
  the BC-250 may have been off longer.

The dashboard shows it under the state name: in `on` "Powered on for 2h 13m,
since 14:32", in `off` "Powered off for 3d 4h, since 2026-11-15 18:02" (the
date is left out when it is today's, in the controller's local time); without
a set clock the "since" part is left out. With `off_since_start` it shows
"Powered off for at least 3h 5m, since the controller was powered up". In `starting`
and `shutting_down`, and when the fields are missing (older firmware), it
shows the state's sentence ("The BC-250 is running.").

## 26. Automatic OS updates

The controller can update the BC-250's operating system by itself: at night
it starts the PC, logs in over SSH with its own key, runs the update, and
shuts the PC down again. Code: `src/osupd.cpp` (schedule and job),
`src/ssh.cpp` (session task, key, settings), `src/ssh_client.cpp` (the SSH
protocol). The PC side is two scripts, `pc_scripts/bc250-setup.sh` and
`pc_scripts/bc250-update.sh`, embedded in the firmware and served by it.

The feature is off until the switch `enabled` is set. While it is off
nothing runs, no key is made, the two script routes answer 404, `run` and
`test` answer 409.

### Settings

NVS namespace `osupd`:

| Key | Type | Default | Content |
|---|---|---|---|
| `on` | u8 | 0 | the master switch |
| `from` | u32 | 180 | start of the window, minutes after local midnight, 0..1439 |
| `len` | u32 | 120 | length of the window, minutes, 15..120 |
| `days` | u32 | 127 | weekdays, bit 0 = Sunday .. bit 6 = Saturday, 1..127 |
| `after` | str | `off` | what to do after a successful update: `off` (shut the PC down) or `reboot` (restart it, it stays on) |
| `lk` | u32 | 0 | the window day of the last night job |

NVS namespace `ssh`:

| Key | Type | Content |
|---|---|---|
| `seed` | blob, 32 bytes | the controller's Ed25519 private key; made from the hardware random generator the first time it is needed while the feature is on, kept through restarts, updates and USB flashing (lost only with the NVS partition: factory reset, full chip erase) |
| `hostpk` | blob, 32 bytes | the PC's pinned host key: stored at the first key exchange, after that a different key ends the session |
| `host`, `user` | str | the PC (IPv4 address, DNS name or `name.local`) and the user to log in as |
| `port` | u32 | default 22 |
| `linked` | u8 | a session logged in with the controller's key |

Only the public key ever leaves the controller. `linked` is set by any
session that logged in; it is cleared when the PC refused the key, when
its host key did not match the pinned one, by `forget_host`, and when
`host`, `port` or `user` change (which also forget the pinned host key).

The automatic runs need the clock (section 18): without a synchronised
clock there are none. Times are local time.

### API

All routes need the login (section 12) except the two script routes.

| Method, path | Body | Answer |
|---|---|---|
| GET `/api/osupdate` | | the object below |
| POST `/api/osupdate` | any of `enabled`, `host` (1..63 characters), `port` (1..65535), `user` (1..32), `from`, `len`, `days`, `after` | ok; 400 for a bad value; 409 while a job runs, except `{"enabled":false}` alone, which is always accepted (it stops a running job like `stop`, then switches the feature off) |
| POST `/api/osupdate/run` | | ok, starts a manual job; 409 when off, not configured (host or user empty), a job or a firmware upload is running, without WiFi, or while the PC is starting or shutting down |
| POST `/api/osupdate/test` | | ok, one `check` session; 409 as `run`, and when the PC is not on |
| POST `/api/osupdate/stop` | | ok; a running job ends: the session is stopped and, if the job started the PC, the PC is shut down as after a failure |
| POST `/api/osupdate/forget_host` | | ok, the next session pins the PC's key again; 409 while a job runs |
| GET `/bc250-setup.sh` | | text/plain, no login, 404 while off: the setup script with `@CTL@` replaced by the controller's address (station IP) and `@KEY@` by the controller's public key line |
| GET `/bc250-update.sh` | | text/plain, no login, 404 while off: the update script |

`GET /api/osupdate`:

```json
{
  "enabled": true,
  "host": "192.168.1.20", "port": 22, "user": "gamer",
  "from": 180, "len": 120, "days": 127, "after": "off",
  "pubkey": "ssh-ed25519 AAAA... bc250-controller",
  "fingerprint": "SHA256:...",
  "host_key": "SHA256:...",
  "linked": true,
  "setup_cmd": "curl -fsS http://192.168.1.50/bc250-setup.sh | sudo sh",
  "clock": true,
  "next": "2026-11-19 03:00",
  "job": {"state": "idle", "kind": "", "since_s": 0},
  "last": {"kind": "night", "at": "2026-11-18 03:00", "result": "ok",
           "error": "", "exit": 0, "duration_s": 173, "reboot_needed": true,
           "end": "off", "out_bytes": 32688, "tail": "..."},
  "mem": {"free_before": 86832, "min_free": 56960, "peak_use": 29872}
}
```

- `pubkey` and `fingerprint` are `""` while the feature is off. `host_key`
  is `""` while none is pinned. `setup_cmd` is `""` while the station has
  no address.
- `next`: local time of the next automatic run, `""` when there is none
  (off, clock not synchronised, no host, not `linked`).
- `job.state`: `idle`, `starting` (PC starting), `waiting` (for the PC to
  accept the SSH connection), `updating`, `rebooting`, `shutting_down`.
  `job.kind`: `""`, `night`, `manual` or `test`.
- `last` is absent until the first job (it is kept in RAM). `result`:
  `ok`, `failed`, `skipped` or `stopped`. `end`: the PC's state when the
  job ended, `off` or `on`. `error` is the reason for a failure or skip;
  after a takeover it also contains `taken over`. `exit`, `out_bytes`,
  `tail` and `reboot_needed` are those of the update (or check) session;
  `tail` is its last 1 KB of output (line breaks and tabs kept, a lone
  carriage return as a line break, other control characters and broken
  UTF-8 as `?`); `reboot_needed` is true when the tail has
  `bc250-update: reboot needed`.
- `mem`: free heap before the last session (`free_before`,
  `largest_before`), the lowest free heap and largest free block during it
  (`min_free`, `min_largest`), their difference (`peak_use`), the session
  task's stack (`stack_size`, `stack_free`), the free heap after it
  (`free_after`) and at each stage (`stage_free`: version, key exchange,
  login, command, closed). The lowest values count everything the
  controller did meanwhile.

### Jobs

One job at a time. The state machine runs in the main task about once a
second; the session runs in its own task. No firmware upload starts while a
job runs (`/api/update` answers 409), and no job starts while one runs.

Night job (automatic): at the window start on a selected weekday, once per
day (up to 5 minutes late if the controller was busy; it records the day in
`lk`).
- Only if the controller is `linked` and the PC is `off` then. Not linked
  (never tested, or the key was refused, or the host key changed): the job
  is skipped without powering the PC on, error `not set up: test the
  connection first`, until a test or a manual run logs in again. PC on
  (someone uses it), or WiFi down for those 5 minutes: skipped too, error
  `the BC-250 was on` (or the reason). Each skip is a log line.
- Silent from start to end (see below).
- Power on (a job action, not a user action), wait for `on`, then the SSH
  session `update`. It keeps retrying the TCP connection every 10 s while
  the PC boots, for up to 10 minutes and never past the window. The
  session ends 5 minutes before the window end at the latest; the rest is
  for the shutdown.
- On success: session `poweroff` (or `reboot` when `after` is `reboot`),
  then wait for `off`. After `reboot` the job waits 30 s and ends with the
  PC on.
- Any failure (the PC did not start, no SSH connection, key refused, host
  key mismatch, update exit status not 0, window end reached): shut the PC
  down gracefully. With a login that worked in this job that is a
  `poweroff` session, otherwise a power-button press (the `shutdown`
  action). If the PC is still on after `shutdown_timeout_ms` the power
  button is pressed once more; then the job gives up, logs it and leaves
  the PC on. Forced power off is never used (section 2).

Manual run (`run`): the same steps. If the PC is off it is started
silently, updated and then handled by `after`. If the PC is on the update
runs and the PC stays on; the job is not silent.

Test (`test`): the PC must be on; one SSH session `check`, which changes
nothing. Success sets `linked`.

The SSH command is the mode: `check`, `update`, `poweroff` or `reboot`. A
`poweroff` or `reboot` session ends with the connection dropping; a
session that logged in and then lost the connection counts as success for
those two modes.

### Silence and takeover

While a job that started the PC runs, the silent mode is on
(`powerSetSilent`): the buzzer makes no sound at all, as in quiet hours,
and the LED breathes as in `off` in every state, without the error blink.
It ends a moment after the job, so the last beep stays silent.

A user action during the job (a front-button press, a web power action, a
wake from Bluetooth or 2.4G, also when the PC is already on) counts as a
takeover (`powerUserActions` counts them; the job's own power requests are
not counted). The silence and the LED override end at once, the running
session is not stopped, and the job never shuts the PC down at the end. The
result notes `taken over`.

### Failure handling

The PC must not be left on after a failed or incomplete update: every
failure path ends in the graceful shutdown above, and every path that
ends with the PC still on says so in `error`. If starting or stopping the
session itself fails (no memory, no key), that is a failure like any other.
A session that ended without an exit status counts as failed.

### The session

Protocol (`src/ssh_client.cpp`, one fixed algorithm set, the OpenSSH
defaults): key exchange `curve25519-sha256` with strict kex, host key and
user key `ssh-ed25519`, cipher `chacha20-poly1305@openssh.com`, no
compression, no PTY, no re-key. Keepalives (`keepalive@openssh.com`) after
60 s without a packet; the session ends after 3 minutes of silence or at its
time limit. The server may send channel data in packets of at most 2 KB
within a 4 KB window, so the receive buffer stays at 4 KB.

The event log has a line when a job starts and ends, for each session and
failure, on a takeover, when a key is made and when a host key is pinned
(all prefixed `os update:` or `ssh:`).

### PC side

`bc250-setup.sh` is run once on the PC as
`curl -fsS http://<controller>/bc250-setup.sh | sudo sh`. It installs
`/usr/local/sbin/bc250-update` (fetched from the controller), a sudoers rule
for it, and puts the controller's key in the user's `authorized_keys` with
`restrict` and a forced command, so the key can only run that script with
one of the four modes. The UI hides these instructions while `linked` is
true.
