# The ESP-IDF build

The firmware has both wake radios (Bluetooth and 2.4G dongle mode); the
wake method is chosen on the wake page and applies after a restart
(`docs/FINAL_FIRMWARE_SPEC.md` section 23). It is plain ESP-IDF (no Arduino
core): `app_main` in `src/main.cpp` does the start-up and creates the main
task. Holding both radios in RAM needs a tightly trimmed configuration,
which is why the settings are the project's own.

## Build

PlatformIO, env `xiao_esp32c3`, `framework = espidf`:

- platform: pioarduino `platform-espressif32` 61.04.00-RC1
- framework: ESP-IDF 6.1.1 (pioarduino `esp-idf-v6.1.1.tar.xz`, as
  `platform_packages` in `platformio.ini`)
- libraries: ArduinoJson 7 (header-only, used with `std::string`; it does
  not need the Arduino core) from `lib_deps`; mDNS is the managed component
  `espressif/mdns` (`src/idf_component.yml`), fetched at the first build into
  `managed_components/`. Monocypher (Ed25519, X25519 and ChaCha20-Poly1305
  for the SSH client; the IDF's mbedTLS has no Ed25519) is the local
  component `components/monocypher/`, kept unmodified.

```sh
cd firmware/bc250_controller
uvx platformio run
```

No preparation step. Settings are in `sdkconfig.defaults` (committed); the
generated `sdkconfig.xiao_esp32c3` is not. After editing
`sdkconfig.defaults`, delete `sdkconfig.xiao_esp32c3` so that it is made
again. `tools/ram_guard.py` runs after linking and stops the build when
static RAM (`_bss_end`) passes 0x3fca0000 or RAM code (`_iram_end`) passes
0x403a0000, the 2.4G capture bank. `touch_buildinfo.py` writes
`src/buildinfo.cpp` (version marker, build time) and the gzipped UI pages.
Add `-DMEMCHECK` through `PLATFORMIO_BUILD_FLAGS` for heap and 2.4G
detector lines on the USB console.

Source conventions (strings, time, NVS, locking, console) are at the top of
`src/util.h`.

## How the two radios share the chip (`src/radio.h`)

- `radioBoot()`, early in `app_main`, reads the method from NVS
  (`radio`/`m`) and gives the unused radio's memory to the heap: in
  Bluetooth mode the 128 KiB 2.4G capture bank, in 2.4G mode the Bluetooth
  controller's data (`esp_bt_mem_release`).
- `wake.cpp` sends the `ble*` calls to Bluetooth (`ble.cpp`, whose
  functions `ble_bt.h` renames to `bt*`) or to the 2.4G stand-ins.

## Settings (`sdkconfig.defaults`)

Everything not listed is the IDF default. Rule: switch off what the
firmware does not use.

- Chip: ESP32-C3 4 MB, 160 MHz, FreeRTOS tick 1000 Hz, size optimisation,
  USB-Serial-JTAG as the only console (UART0's RX pin is the front switch),
  log level error, crash dump to the `coredump` partition.
- Bluetooth: controller code runs from flash (`BT_CTRL_RUN_IN_FLASH_ONLY`).
  NimBLE host with the observer role only: no connections, advertising,
  pairing or GATT. Two controller activities, four ACL buffers.
- WiFi: station and the setup hotspot only. Off: enterprise, WPA3
  OWE/SAE-PK, MBO, FTM, CSI, GCMP/GMAC, WPA3 on the hotspot. Smaller buffer pools. The WiFi
  code stays in flash (`ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT` off).
- WiFi roaming: `ESP_WIFI_11KV_SUPPORT` (RRM and WNM), `ESP_WIFI_11R_SUPPORT`
  and `ESP_WIFI_ENABLE_ROAMING_APP` (not marked experimental in 6.1.1). The
  STA flags `rm_enabled`, `btm_enabled`, `ft_enabled` are set in
  `wifi_mgr.cpp`. MBO is off: the app only includes its header, and MBO adds
  certification parts we do not need. App settings:
  weak-signal trigger on at -72 dBm (`LOW_RSSI_THRESHOLD`), -5 dB per further
  step (`LOW_RSSI_OFFSET`), candidate at least 10 dB better
  (`LOW_RSSI_ROAM_DIFF`); periodic scan monitor and periodic neighbour
  reports off (event-driven, no regular off-channel time for a device that
  hardly moves; while the signal stays weak `wifi_mgr` re-arms the
  weak-signal trigger every 30 s instead, spec section 5); BSS transition first (`NETWORK_ASSISTED_ROAM`, 2 tries),
  then directed reconnect (`LEGACY_ROAMING`); `BACKOFF_TIME` 30 s; scan all
  channels (`SCAN_CHAN_LIST` "None": the channels of the network are not
  known), 30..70 ms per channel; blacklist off; `ROAMING_IP_RENEW_SKIP` on:
  after a roam by the stack (reason ROAMING) the IP stack is not restarted,
  so the DHCP lease or fixed address stays (same network, same subnet). The
  app's directed reconnect has reason BSS_TRANSITION_DISASSOC and still renews
  DHCP. `src/CMakeLists.txt` adds the roaming app's private include
  directory (`esp_roaming.h`), which `wifi_mgr.cpp` uses.
- Network: IPv4 only, no PPP, no loopback, no TCP SACK, 10 sockets
  (`esp_http_server` takes `max_open_sockets` + 3), one SNTP server. DHCP
  does not check the offered address
  (`LWIP_DHCP_DOES_NOT_CHECK_OFFERED_IP`): the ARP probe cost about a second
  at every connect.
- Interrupt code of unused drivers (RMT, GPTIMER, I2C, I2S, GDMA, SPI
  slave) and the heap functions in flash. FreeRTOS functions are in flash
  too.
- Flash erase yields every 10 ms, TX power reduction after a brownout.

Off: Bluetooth connection roles (see above), heap poisoning, stack protector,
C++ exceptions, the trace facility.

## Boot compatibility with devices in the field

Devices running firmware 2.1.20 or later can update over WiFi to this build.
Their bootloader (IDF 5.5) and partition table stay as they are, so:

- `partitions.csv` keeps the layout those devices have (nvs 0x9000,
  otadata 0xe000, app0 0x10000, app1 0x1f0000 both 0x1e0000, spiffs
  0x3d0000, coredump 0x3f0000).
- `esptool image-info` shows the header fields the old bootloader expects:
  chip ESP32-C3, minimum chip revision v0.0 in the header (the setting is
  `ESP32C3_REV_MIN_3`), 64 KB MMU page, eFuse
  block revision 0.0 to 1.99, 4 MB, DIO, 80 MHz.
- The old bootloader has app rollback enabled. The app keeps
  `BOOTLOADER_APP_ROLLBACK_ENABLE` and calls
  `esp_ota_mark_app_valid_cancel_rollback()` first thing in `app_main`,
  otherwise the next reset would go back to the
  old slot. `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` keeps the 16 bytes at
  the top of the RTC memory free that this bootloader writes.
- A deliberate restart under a running PC (section 17) hands over through
  RTC memory (`rtcPcOn`, address 0x50000000, the same in every build since
  2.1.20) and the pad hold of PS_ON, so it works across an update from 2.1.20
  too.
- Flashing over USB writes the IDF 6.1 bootloader as well; it boots the
  same partition table.

## Console

`utilInit()` installs the USB-Serial-JTAG driver with a 512-byte transmit
ring. `logEvent()` and `conPrintf()` write with a zero wait: nothing blocks
when no host is connected or the host does not read (the line is dropped; it
stays in the event log). Plain `printf` and `ESP_LOG*` go through the same
driver; with no host they return at once, with a host that stopped reading
the first byte can wait up to 50 ms.

## Memory

Free heap is the tight resource in 2.4G mode, where the Bluetooth
controller's memory cannot all be returned. Typical figures (four firmware
uploads in each mode, no crash):

| Mode | Free idle | Lowest over 4 uploads |
|---|---|---|
| 2.4G/BT | about 52 KB | about 12 KB |
| BLE | about 120 KB | about 86 KB |

Build with `-DMEMCHECK` to print heap and detector lines on the USB console
and check the figures on your own board.

## Flashing over USB

A USB flash writes the first app slot only; after updates over WiFi the board
may boot the other slot. Erase otadata (`esptool erase-region 0xe000 0x2000`)
after a USB flash, or update over WiFi. See `docs/FLASHING.md`.
