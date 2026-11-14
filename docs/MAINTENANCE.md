# Settings and logs

AA-EntryNav has no configuration file, and the service ignores environment variables. Each setting is a small file in
`/var/local/data/AA` on the data partition, so you never have to make the system writable to change one. Most files
only count when Android Auto starts. After a change, pick *Stop Android Auto* and then *Start Android Auto* in its menu.

Type the commands on this page in an SSH session on the head unit
([installation](INSTALLATION.md#step-1-copy-the-release-to-the-head-unit)). Most settings are on while their file exists, so
`touch` turns one on and `rm` turns it off:

```sh
touch /var/local/data/AA/kombi-off   # no arrows on the cluster or the head-up display
rm /var/local/data/AA/kombi-off      # arrows again
```

## Setting files

### Starting and logging

| File | While it exists |
|---|---|
| `autostart` | Android Auto starts when iDrive does. The *Start automatically* box in the menu creates and deletes it |
| `entry-direct` | The *Android Auto* row in *Media* goes straight to Android Auto and skips its menu |
| `maintenance` | The iDrive app and the service write [logs](#logs) |

### What the phone gets

| File | Effect |
|---|---|
| `vehicle` | Holds up to three lines, `model=…`, `year=…` and `id=…` (a UUID). The phone shows the model and year. It files its settings for the car under the ID, so give each car its own if one phone goes into two of them |
| `gps-telephone` | While it exists, the phone uses its own GPS and the car's position isn't sent |

### Picture, sound and connection

| File | While it exists |
|---|---|
| `video-720` | A phone on the cable draws a 1280x720 picture and the head unit shows it pixel for pixel. Without the file a phone on the cable draws 1920x1080 and the head unit scales it down to the screen, which smooths the edges. A head unit that can't decode 1920x1080 drops back to 1280x720 by itself. A phone on Wi-Fi always draws 1280x720 |
| `audio-wifi` | Music and announcements come over Wi-Fi and play on the car's speakers, an experiment ([details](#sound-over-wi-fi-experimental)). Without the file, sound uses Bluetooth |
| `select-bt-audio` | iDrive switches to Bluetooth audio when the phone connects, an experiment ([details](#bluetooth-audio-when-the-phone-connects-experimental)). Read at each connection. It does nothing while `audio-wifi` exists |
| `usb` | A phone plugged into the car's USB port can run Android Auto too, an experiment ([details](#usb-cable-experimental)). Without the file the service leaves the USB bus alone |
| `wireless-off` | The cable is the only way in. Android Auto leaves the car's Bluetooth and Wi-Fi hotspot as they are. It needs `usb`. With `wireless-off` alone, Android Auto doesn't start |

### Cluster and head-up display

| File | While it exists |
|---|---|
| `kombi-off` | Android Auto sends nothing to the cluster or the head-up display |
| `kombi-single-maneuver` | The cluster gets only the next turn, not the whole route. This is for the X3 G01, whose cluster (software 146) rejects the full route and then shows no arrow at all. Read when the service starts |

### Files AA-EntryNav writes

Leave these alone unless a section below says otherwise.

| File | What it holds |
|---|---|
| `flare-pin` | The PIN of the *Flare Drive* page as a salted hash, plus the count of wrong PINs in a row ([QR code](#flare-drive-qr-code)) |
| `install-record` | Written by `setup.sh`: the installed version, and whether it created the `Plugins` folder. Uninstalling only removes `Plugins` when this file says it made it |
| `app-guard` | The [crash guard](#crash-guard)'s count of short iDrive runs |

## USB cable (experimental)

> [!WARNING]
> The cable works in the reference car. Nobody has tried `wireless-off` in a car yet.

Some head units only have 2.4 GHz Wi-Fi, and Bluetooth shares that band. On those cars the wireless picture lags and
stutters. With the `usb` file, the service also offers Android Auto to a phone on one of the car's USB ports whenever no
phone is connected. Only the transport changes. The session, the picture, touch and the iDrive controller behave as
they do wirelessly. Sound and calls still go over Bluetooth.

```sh
touch /var/local/data/AA/usb    # then Stop Android Auto and Start Android Auto
rm /var/local/data/AA/usb
```

Over a cable, the phone runs Android Auto in Android's accessory mode. This is how the service gets it there.

- **BMW's media program asks first.** It puts every Android phone into accessory mode for its own app link, within
  milliseconds of the phone appearing. While the `usb` file exists, the service adds a udev rule in
  `/run/udev/rules.d` that makes a new phone's device file in `/dev/bus/usb` readable by root alone. BMW's program
  doesn't run as root, so it can't reach the phone first. The rule ignores USB sticks and iPhones. It lives in memory,
  so a restart clears it, and the service removes it when it stops. While it's in place, BMW's own USB music from an
  Android phone doesn't work.
- **A phone that's already in accessory mode** gets a port reset, which returns it to its normal mode, and the service
  asks again as soon as it comes back. It tries this three times at most.
- **A phone can refuse.** On a PC this happened with the phone's USB setting on charging only. Each request knocks the
  phone off the bus for about a second, so the service waits 30 seconds before asking again. Right after a plug-in,
  or after a session on the cable, it retries up to three times 2 seconds apart instead, and unplugging the phone and
  plugging it back in clears the wait.
- **Bluetooth stays with the phone and iDrive.** The service doesn't tell the phone the car's Bluetooth address. Told
  it, Android Auto took the phone's Bluetooth over: the phone hid its own pairing code and waited for one from the
  car, and it didn't reconnect after its Bluetooth dropped. Pair through iDrive's new device screen as usual, cable in
  or not. Without Bluetooth, the cable still carries Android Auto, but there are no calls and no sound unless
  `audio-wifi` is on.
- While the cable is connecting a phone or carrying its session, the service doesn't send that phone to the Wi-Fi.

`wireless-off` makes the cable the only path. Android Auto then puts nothing on BMW's Bluetooth serial slot, so there's
nothing to give back at the end, and it doesn't switch the Wi-Fi hotspot on.

```sh
touch /var/local/data/AA/wireless-off    # needs the usb file; then Stop Android Auto and Start Android Auto
rm /var/local/data/AA/wireless-off
```

To walk through the cable steps by hand, build `evo-usb` with `make dev-tools` and copy it to `/tmp`. It isn't part of
an installation. `evo-usb list` prints what's on the bus. `evo-usb link` runs every step the service takes and then
hands the phone back. `evo-usb bluetooth AA:BB:CC:DD:EE:FF` prints the car's Bluetooth address, whether the car accepts
pairing right now, and whether that phone is paired.

## Sound over Wi-Fi (experimental)

> [!WARNING]
> Tried once in the reference car, by hand, on 14 November 2026. Try it parked, with the volume low.

The `audio-wifi` file has the phone send its music and announcements over Wi-Fi rather than Bluetooth. The service mixes
the two, turning the music down while an announcement plays, and plays the result through the car's sound library.
Calls stay on Bluetooth. Flare Drive's "sound over Wi-Fi" switch writes an empty file, which uses the defaults below.

The car only plays a sound output once its audio manager has connected it to the speakers. Before the first sound of a
session, the service asks for that connection on the car's network, as BMW's CarPlay does for its music: route `0x315`,
the `entertainment` output. It gives the route back when the session ends or Android Auto stops. While the route is
ours, iDrive shows no Bluetooth audio selected, and the volume knob sets the level. Each line of the file is optional:

| Line | Meaning | Default |
|---|---|---|
| `pcm=…` | The sound output, named as in the head unit's `/etc/asound.conf` | `entertainment` |
| `route=…` | The route asked of the car's audio manager, in hex, or `0` for none | `315` with `entertainment`, else none |
| `gain=…` | Overall level, 0 to 100 % | 100 |
| `duck=…` | How much music is left during an announcement, 0 to 100 % | 30 |

The service log shows the request: `asking the car's audio manager for route 0x315`, then `route 0x315 connected to the
speakers by the car's audio manager`, or why not. The output may refuse to open until the car has switched the route.

`evo-sound` lists the outputs and plays a test tone, without asking for a route. Build it with `make dev-tools` and copy
it to `/tmp`. It isn't installed.

```sh
LD_LIBRARY_PATH=/usr/local/lib:/usr/lib:/lib /tmp/evo-sound list
LD_LIBRARY_PATH=/usr/local/lib:/usr/lib:/lib /tmp/evo-sound tone entertainment 6 5
```

`tools/car_sound_probe.sh <situation>` changes nothing. It records what the head unit declares and which outputs each
source opens. Run it once each for the radio, Bluetooth, CarPlay and a navigation announcement, then compare.

## Bluetooth audio when the phone connects (experimental)

> [!WARNING]
> Nobody has tried this in a car yet.

When sound goes over Bluetooth, the car only plays Android Auto's sound while iDrive's source is Bluetooth audio. With
the `select-bt-audio` file, the iDrive app picks that source for you when the phone connects. It sends iDrive's own
"mode player select" event for Bluetooth audio (focus ID 2270, player type 13) to iDrive's event queue, just as choosing
*Bluetooth audio* in the Media source list does. The app keeps hold of that queue from the moment iDrive creates it.

- It happens once per connection. During a call or in reverse the app waits, and after 2 minutes it gives up and leaves
  the source as it is.
- If iDrive's media app is in front, iDrive also opens its Bluetooth audio screen. For 2 seconds the app holds back
  iDrive leaving the Android Auto screen, the same way it does when NAV, MEDIA or COM opens an Android Auto app.
- If the phone's Bluetooth audio isn't connected yet, iDrive drops the current source and starts Bluetooth audio once
  it connects. It's silent until then.
- The app sends nothing if it didn't find iDrive's event queue when it started, or if `sendEvent` isn't at the address
  it had on `202306092158`. With logs on, `app.log` says which.

```sh
touch /var/local/data/AA/select-bt-audio    # from the next phone connection
rm /var/local/data/AA/select-bt-audio
```

## Flare Drive QR code

> [!NOTE]
> This works in the reference car since 0.1.4. The PIN screen and the picture use parts of iDrive that no other app on
> this head unit uses. With logs on, the app's log records each step.

The Android Auto menu has a *Flare Drive* row above *Help*. It asks for a PIN, then shows a QR code. Flare Drive scans
the code to join the car's Wi-Fi and find the head unit. The code holds the Wi-Fi name and password, read from wland's
`/tmp/wland/ap.conf`, and the head unit's address on `wlan0`. It never holds the root password. On first use you choose
a PIN of 4 to 8 digits and type it twice.

- The code disappears while the car moves (`driving.speedActual` at 3 km/h or more), and returns when it stops if the
  page is still open.
- Leaving the page forgets the code, so the next visit asks for the PIN again.
- Five wrong PINs in a row lock the page for a minute. Each wrong PIN after that doubles the wait, up to an hour. The
  count is kept in `flare-pin`, so a restart doesn't reset it, though the wait starts over from the restart.
- If the car's Wi-Fi is off, the page says so. wland switches the hotspot off some time after its last client leaves,
  and starting Android Auto switches it back on.

Forgot the PIN? Delete the file and open the row again to choose a new one:

```sh
rm /var/local/data/AA/flare-pin
```

## Logs

Without the `maintenance` file, AA-EntryNav logs nothing, either to flash or to memory. To chase a fault, turn logs on,
make the fault happen again, copy the logs off, then turn them off.

```sh
touch /var/local/data/AA/maintenance
```

| Log | Where | Size |
|---|---|---|
| iDrive app | `/var/local/data/AA/app.log`, moved to `app.old.log` when it passes 512 KB | 512 KB per file |
| Android Auto service | `/tmp/aa-entrynav-service.log`, moved to `aa-entrynav-service.old.log` when it passes 1 MB | About 2 MB in all, in memory, gone after a restart |

To copy them to the PC:

```sh
scp root@169.254.1.0:/var/local/data/AA/app.log root@169.254.1.0:/tmp/aa-entrynav-service.log .
```

To turn logs off and delete them:

```sh
rm -f /var/local/data/AA/maintenance /var/local/data/AA/app.log /var/local/data/AA/app.old.log /tmp/aa-entrynav-service.log \
    /tmp/aa-entrynav-service.old.log
```

## Crash guard

The app counts every iDrive start in `/var/local/data/AA/app-guard` the moment iDrive loads the library, before the app
even exists, so a crash between the two still counts. A start that loads the library twice counts once. If iDrive
restarts three times in a row, each within 2 minutes of loading the app, the app stays off and iDrive runs without it.
Two minutes without trouble clear the count.

The guard fails closed. If the app can't record a start because the data partition is full or read-only, it stays off,
since the guard could no longer stop it. Installing a new version resets the count.

To keep the app off without touching the system, create `app.disable` by hand. Once you've found the cause, turn the app
back on with:

```sh
rm -f /var/local/data/AA/app.disable /var/local/data/AA/app-guard
reboot
```

## Checking the installation

`setup.sh status` prints the version, the folders, whether iDrive has the app loaded and the crash guard's state. It
writes nothing.

```sh
sh /opt/aa/setup.sh status
```

The same checks by hand:

```sh
ls -la /opt/aa /opt/hmi/data/ro/Plugins/AAEntryNav /var/local/data/AA
grep -c libAAEntryNav /proc/$(pidof HmiMain3D)/maps   # above 0: iDrive has loaded the app
pidof evo-trial aa-entrynav                           # Android Auto is running
```
