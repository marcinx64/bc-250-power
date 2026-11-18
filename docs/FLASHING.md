# Building and flashing

Built with PlatformIO Core run through `uvx`, platform pioarduino
espressif32 61.04.00-RC1, plain ESP-IDF 6.1.1 (see `docs/ESP_IDF_BUILD.md`).
Install [uv](https://docs.astral.sh/uv/); every command below uses
`uvx platformio`. The first build downloads the toolchain and takes several
minutes.

## Build

```sh
cd firmware/bc250_controller
uvx platformio run
```

No preparation step. The first build downloads the framework, the
toolchain and the mDNS component. The build ends with a RAM check
(`ram_guard`) that must print "static RAM ends at ...".

The result is `.pio/build/xiao_esp32c3/firmware.bin`: one firmware with both
wake methods (BLE and 2.4G/BT, chosen on the wake page). Copy it to
`firmware/releases/firmware-<version>.bin` to keep it; built files are not
committed.

Before building a version to install, raise `FW_VERSION` in
`src/version.h` (the version shown and the file marker). The build time is
stamped automatically (`touch_buildinfo.py` rewrites `src/buildinfo.cpp` on
every build).

## Update over WiFi (normal way)

Devices running firmware 2.1.20 or later can update over WiFi.

1. Open the web UI, Controller view.
2. Choose the firmware file: your own build, or a `firmware-<version>.bin`
   from the [Releases page](https://github.com/marcinx64/bc-250-power/releases).
   The page shows the file's version, build time and checks.
3. Press Install. The BC-250's power state is unaffected; the controller keeps
   the supply on through its restart.
4. The controller restarts. Log in again; a message confirms the running
   version.

Settings, WiFi, the password and the wake lists are kept. The previous
firmware stays in the other slot until a new one is completely written.

## Flash over USB (first install or recovery)

Only the USB-C cable connected. No ATX supply, no BC-250. Never have USB
and the ATX +5VSB wire connected together.

```sh
cd firmware/bc250_controller
uvx platformio run -t upload --upload-port /dev/ttyACM0
uvx esptool --port /dev/ttyACM0 --after watchdog-reset read-mac
```

The second command restarts the chip into the firmware. Without it the chip
can stay in download mode after an upload. Tapping the R button does the
same.

The board shows up as `303a:1001 Espressif USB JTAG/serial debug unit` on
`/dev/ttyACM0`. The upload normally enters download mode by itself. If it
cannot connect: hold **B** on the XIAO, tap **R**, release **B**, and
upload again.

Flashing over USB keeps the stored settings. Only a full chip erase removes
them.

A USB flash writes the first app slot only. After updates over WiFi the
board may boot the other slot, so a board that was updated over WiFi can keep
running the old firmware after a USB flash. Erase otadata
(`uvx esptool erase-region 0xe000 0x2000`) after a USB flash, or update over
WiFi instead.

## Troubleshooting

- Opening the serial port from a script can reset the chip into download
  mode (the DTR/RTS lines are interpreted by the USB interface). Use the
  watchdog reset above to get back out.
- WiFi plus Bluetooth does not fit the default 1.3 MB app slot. The firmware
  uses `partitions.csv` (two 1.9 MB slots, needed for OTA). It must stay
  identical: devices in the field keep their partition table and bootloader
  when they update over WiFi.

## Test firmware

`firmware/bc250_test/` is a bench test firmware (own WiFi network
`BC250-Test`, password `bc250test`, page at `http://192.168.4.1`) for
fault-finding the hardware. Flash it the same way from its own folder. It
uses the `huge_app.csv` partition table, so after it the main firmware has to
go back on over USB, not WiFi.
