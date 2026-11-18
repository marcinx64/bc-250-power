# User guide

How the controller behaves. The exact rules are in
`docs/FINAL_FIRMWARE_SPEC.md`.

## Front button

| BC-250 state | What you do | What happens |
|---|---|---|
| Off | Press | 1 beep, the supply turns on and the BC-250 starts. The rest of that press is ignored, however long you hold it |
| On | Press and release within 3 s | 1 beep, a power-button pulse: the operating system shuts down cleanly |
| On | Hold | The LED flashes slowly after 1 s. 2 beeps at 3 s and the LED flashes fast. Release now and the BC-250 resets |
| On | Keep holding | 3 beeps at 5 s and the supply is cut at once, without waiting for release |
| Starting or shutting down | Hold 5 s | Slow flashing, then 3 beeps and the supply is cut |

The 3 s and 5 s are settings (Settings, Front button): reset 2 to 30 s,
forced off 3 to 60 s, forced off at least 1 s longer than reset.

If the operating system shuts the BC-250 down by itself, the controller sees the
power LED go dark, releases the supply and gives 1 beep.

If the BC-250 does not show its power LED within 15 s of a start, the supply is
released again, with one long beep and a fast blinking LED.

## Button LED

| State | LED |
|---|---|
| Off | Slow breathing, one cycle in 6 s |
| Starting, shutting down | Fast breathing |
| On | Steady |
| Error | Fast blink for 3 s |

Brightness and the breathing speed are in Settings.

## First start and WiFi

1. With no WiFi stored, the controller opens the network `BC250-Setup`,
   password `bc250setup`. Join it; the setup page opens by itself, or go to
   `http://192.168.4.1`.
2. Set the web UI password (8 to 64 characters).
3. Choose your WiFi network and enter its password.
4. Afterwards the UI is at `http://bc250.local` (or the IP address shown).
   The setup network switches off about a minute after the connection is up.

If the controller loses your WiFi for 30 s, it opens `BC250-Setup` again
while it keeps trying to reconnect.

If your network has several access points with the same name (a mesh or a
router with extenders), the controller moves to a stronger one when its
signal gets weak. The Network page shows the access point in use and its
channel, and the Log shows each move. Roaming is paused during a firmware
update, 2.4G learning, the Bluetooth device scan and the list of WiFi
networks. If the controller keeps trying an access point that went out of
reach, it starts over with the strongest one after 15 s; the Log shows
"wifi: connecting stuck, starting over".

## Web UI

| View | What is there |
|---|---|
| Dashboard | Power state with how long the BC-250 has been on or off, power buttons, then summary cards: BC-250 signals, wake (BLE or 2.4G/BT), network, clock, front button, controller (CPU and memory use as bars, chip temperature) |
| Wake | Wake method (one button switches between BLE and 2.4G/BT), wake switch, wake options, wake list, scan and add a controller (all but the switch hidden while wake is off) |
| OS updates | Switch for automatic OS updates; status, last run and the end of its output; the BC-250's address and user, the update window, days and what to do after a successful update; the controller's key; first-time setup (hidden once the connection works); the two scripts installed on the BC-250 |
| Settings | Time, front LED, buzzer and quiet hours, front button hold times, power timing |
| Network | Connection status, signal, access point and channel, IP settings, hostname, change or forget the WiFi network |
| Security | Change password, log out |
| Controller | Full-width cards: Hardware card (chip, flash, MAC address, chip temperature, CPU and memory use), Firmware card (version, build, ESP-IDF, partition, size, uptime, start reason), update, restart, clear wake lists, reset settings, factory reset. Old `#firmware` links open this view. |
| Debug | Spoof the LED sense, show all Bluetooth devices in the scan |
| Log | The last 40 events since the controller started |

Messages appear at the top of the page, under the title bar:

- Messages that stay while something lasts: a restart or update (waiting
  for the BC-250 to shut down, then in progress until "Back online"), a
  wake method switch, a factory reset or a new IP address; connection lost
  (not while one of those explains it); debug mode. Each is a bar across
  the page that moves the page down while it shows.
- Short messages after an action (saved, added, refused...). They float
  over the page as small cards, at most two at a time, and close after 4 s
  (errors after 8 s) or with their × button.

Questions before an action (shut down, restart, clear a list, reset) appear
next to the button that was pressed; a restart or update while the BC-250
is on opens a dialog.

The chip temperature is the controller chip's own sensor: it shows how warm
the chip runs, not the room (about 1 to 3 °C accurate). Amber from 75 °C. The
CPU and memory bars turn amber from 75 % and red from 90 %.

You stay logged in through a restart, a firmware update or a
wake method switch started from the page; a power loss, a crash or the reset
button logs everyone out. After 5 wrong passwords
the login is blocked for 30 s.

## Wake method

The firmware wakes the BC-250 either by Bluetooth Low Energy (BLE) or by a
device with a 2.4G USB dongle or a Bluetooth Classic controller (2.4G/BT).
The first card on the wake page names the method in use and has one button:
"Switch to 2.4G/BT" or "Switch to BLE". The controller restarts to switch;
the BC-250's power state is unaffected. Each method has its own wake list, and
both are kept when you switch. Settings stay the same.

The "Clear wake lists" card on the Controller page, above Factory reset,
shows how many devices each list holds and has a Clear button for each,
whichever method is in use. It asks before it clears; there is no undo.

Both methods share these rules:

- The wake list holds up to 8 devices. Rename changes a name in place;
  Remove asks for confirmation. Each device has an Enabled switch: only
  enabled devices can start the BC-250; a disabled one is still listed and
  shown as heard. Two devices cannot have the same name (upper and lower case
  count as the same); in BLE mode a device may have no name.
- Scan (BLE) and Learn (2.4G/BT) run until Stop or for the scan time (Wake
  options, 60 s by default, 30 s to 10 min). You add devices while it runs,
  and the results go when it ends.
- Each list row shows tags: switched on or not seen, armed or not, and when
  the device was last heard. BLE adds the signal strength as one bar (red
  weak, yellow fair, green good; the dBm value in the hover text). Devices
  already in the wake list are shown apart, under "Already in the wake
  list", without a name field.
- While a scan or Learn runs, wake is paused: no listed device starts the
  BC-250. The card that runs it says so, and when devices arm again: each
  device needs the re-arm time afterwards before it can start it.
- A device must have been unheard for the re-arm time (30 s by default)
  before it can start the BC-250: after a shutdown, after the controller
  starts, and after the device was last heard. So a controller that is left
  on does not restart the BC-250 after a shutdown, and adding a controller
  does not start it either.

## BLE wake (Bluetooth Low Energy)

- Scan on the Wake page and add your controller to the wake list. Only game
  controllers are listed; the Debug view has a switch to list every device.
- The BC-250 starts when a controller on the list is switched on while the
  BC-250 is off.
- Only Bluetooth Low Energy controllers can be seen. PlayStation 3, 4 and 5
  and Switch Pro controllers use Bluetooth Classic: they work in 2.4G/BT mode
  instead (below).

## 2.4G/BT wake

With the wake method set to 2.4G/BT, a device with a **2.4G USB dongle** or a
**Bluetooth Classic controller** wakes the BC-250 (tested: 8BitDo Ultimate 2
and Ultimate 2C Wireless game controllers in 2.4G mode, Rii mini i25 keyboard,
DualShock 4). BLE is off in this mode, and the web UI shows "2.4G/BT wake"
where it otherwise shows "BLE wake". The lists are "2.4G Dongle devices wake
list" and "Bluetooth Classic hosts wake list" (BLE: "Bluetooth Low Energy
devices wake list").

- Learn: press Learn and switch the device on in 2.4G mode, with the BC-250
  off or its dongle unplugged. It appears within a few seconds; give it a name
  and add it to the wake list.
- The BC-250 starts when a listed device is switched on while the BC-250 is
  off. It is noticed within a few seconds (it may take up to about 15 s).
- A searching 2.4G controller is heard only now and then, so a re-arm time
  under about 20 s can let it start the BC-250 again.
- It works only while the controller is connected to your WiFi. While it
  listens, it leaves the WiFi channel for about 1 ms a dozen times a second;
  the web UI stays usable.
- Switching to BLE keeps the 2.4G/BT lists; clear them on the "Clear wake
  lists" card if you no longer need them.

### Bluetooth Classic controllers

In 2.4G/BT mode a Bluetooth Classic controller (any; tested with a
DualShock 4) also wakes the BC-250, next to the 2.4G devices. Pair the
controller with the BC-250 as usual. When it is switched on it calls the
BC-250's Bluetooth address, so the "Bluetooth hosts" list (up to 2, apart from
the 2.4G devices) holds that address, and one entry covers every controller
paired with the BC-250.

- Add it by Learn on the "Learn a Bluetooth host" card: press Learn, then
  switch the controller on (its home button) with the BC-250 off, and add the
  host that appears. The name defaults to "BC-250" ("BC-250-2" for a second
  host); type another to replace it. Other Bluetooth devices nearby may show
  up too; take the one that appeared when you pressed the button.
- Or type it in under "Or enter its address" on the same card, with the
  address from `bluetoothctl show` on the BC-250.
- A controller paired with another device (PC, PS4) calls that device and does
  not wake the BC-250.
- Anything else paired with the BC-250 that reconnects by itself (a headset)
  calls the same address and would start the BC-250 too. Pairing it with
  another device, or giving the controllers their own Bluetooth adapter on the
  BC-250, avoids that.
- The controller calls for about 14 s after the button; the BC-250 starts
  within a second or two. Details: `docs/WAKE_BT_CLASSIC.md`.

## Automatic OS updates

The controller can install the BC-250's system updates by itself at night:
it starts the BC-250, logs in over SSH with its own key, runs the update
and shuts the BC-250 down again. It is off until you switch it on in the
OS updates view. The update script uses pacman (Arch, CachyOS),
rpm-ostree and Flatpak (Bazzite, Fedora Atomic) or dnf (Fedora).

Setting it up, once:

1. Switch on Automatic OS updates, enter the BC-250's address and your user
   name on it, and save.
2. On the BC-250, run the command shown under First-time setup
   (`curl -fsS http://<controller>/bc250-setup.sh | sudo sh`; it asks for
   the sudo password). It installs `/usr/local/sbin/bc250-update`, a sudo
   rule that lets your user run only that script without a password, and
   the controller's key in `~/.ssh/authorized_keys`, limited to that
   script. It also starts sshd. Both scripts can be read on the OS updates
   view before you run them.
3. With the BC-250 on and started up, press Test connection. It runs a
   check that changes nothing. Once it passes, the setup instructions are
   hidden and the next automatic run is shown.

The window has a start time, a length of 15 to 120 minutes, the days, and
what to do after a successful update: shut the BC-250 down, or restart it
(it then stays on). It follows the controller's clock and timezone; while
the clock is not set there are no automatic runs.

At the start of the window, if the BC-250 is off:

- It starts silently: no beeps at all, and the front LED breathes as when
  the BC-250 is off.
- The controller waits for SSH (up to 10 minutes), runs the update, then
  shuts the BC-250 down (or restarts it).
- If anything goes wrong (no connection, key refused, update failed, the
  window ends), the BC-250 is shut down the normal way, never by cutting
  the power.
- Pressing the front button, waking the BC-250 from a game controller, or a
  power action in the UI during the update hands the BC-250 back to you:
  beeps and LED come back and it is not shut down at the end.

If the BC-250 is on at the window start, that night is skipped. There is
at most one automatic run a day, and none while the connection has not
been tested.

Run now starts the same update at once; if the BC-250 was already on, it
stays on afterwards. Stop ends a running update, and if the update started
the BC-250 it is shut down. While an OS update runs, firmware updates are
refused. If the BC-250 is reinstalled or gets a new SSH host key, press
Forget host key, run the setup command again and test.

## Updating the firmware

On the Controller view choose a `firmware.bin`. The page reads the file,
shows its version, build time and checks, and uploads only when you press
Install. The BC-250's power state is unaffected: the controller holds the power supply on while it restarts, and the front button does not respond for a few
seconds. Settings, WiFi, the password and both wake lists survive an update. See
`docs/FLASHING.md` for building the file.

## Clock, timezone and quiet hours

- The controller sets its clock from internet time after a power loss and
  checks it once a day (Settings, Time; the interval can be changed or
  switched off, and there is a Sync now button). With no internet, opening
  the UI hands over the browser's time.
- The clock keeps running through a restart or update of the controller.
  It is lost only when the board loses power.
- The timezone is set from your browser whenever none is chosen: the
  first time you open the UI, and after Reset settings or a factory reset. It can be changed in Settings,
  Time. Daylight saving changes happen by themselves and need no internet.
- Quiet hours (Settings, Buzzer): between the two times there are no beeps
  at all. The LED and the button timings work as usual. If the clock is
  not set, quiet hours do not apply.
- Automatic OS updates use the clock too: while it is not set, there are no
  automatic runs.

## Fixed network address

Network, IP settings: Automatic (DHCP) or a fixed IP address, mask,
gateway and DNS. Saving reconnects the controller; the BC-250's power state is unaffected.
If the controller cannot be reached afterwards, `BC250-Setup` comes up
after about 30 s and the UI is at `http://192.168.4.1` there. "Forget
network" and the forgotten-password hold also return it to automatic.

## Debug mode

The Debug view can replace the real LED sense: Real, Always lit, Always
dark, or Simulated BC-250 (lit 3 s after the supply turns on, dark 5 s after a
power-button pulse). It returns to Real on every restart. Simulated BC-250 lets
you try the whole power sequence with nothing attached.

With a real BC-250 attached, Always dark or Simulated BC-250 while the BC-250 runs
makes the controller cut its power.

## Forgotten password

Hold the front button while the controller powers up and keep it held for
10 s. It gives 5 beeps with 5 LED flashes and erases the password and the
WiFi settings. Other settings and the wake list stay. Then start again with `BC250-Setup`.

## Reset settings

Controller view, Reset settings: asks first, then sets every
setting and wake option back to its default (re-arm 30 s, scan 60 s, front
button holds 3 s and 5 s, LED, buzzer, time, power timing). The password,
WiFi, IP settings, hostname, wake method, wake lists and the wake switch
stay. Nothing restarts. The timezone is then taken from the browser again.

## Factory reset

Controller view, Factory reset: erases the password, WiFi, all
settings, both wake lists and the wake method; the firmware stays. The
controller restarts in setup mode: join `BC250-Setup` (password
`bc250setup`) and open `http://192.168.4.1`. It is offered when the
BC-250 is off, or on with "keep the BC-250 on during restart" set (the
BC-250's power state is unaffected).
