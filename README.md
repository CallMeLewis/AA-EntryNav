# AA-EntryNav

**Android Auto for BMW iDrive, on cars with the EntryNav2 head unit.** Wireless or over a USB cable, started from
iDrive's *Media* menu. It runs alongside iDrive and replaces none of BMW's files.

> [!WARNING]
> Experimental software that writes to the head unit. Install it with the car parked, at your own risk. Not affiliated
> with BMW or Google.

## Features

| | |
|---|---|
| **Wireless Android Auto** | The phone pairs over Bluetooth, then joins the car's own Wi-Fi |
| **Started from iDrive** | *Media*, *Android Auto*: start, stop, or start automatically with iDrive |
| **The car's controls** | iDrive controller, buttons, touch screen, steering wheel buttons and voice button |
| **The car's microphone and GPS** | Calls and the assistant use the car's microphone; maps use the car's GPS |
| **Cluster and head-up display** | Turn-by-turn arrows from Android Auto's navigation |

### Experimental, off until switched on

Each one is a switch in Flare Drive, or a file in `/var/local/data/AA` on the head unit.

| Feature | File | Verified in a car |
|---|---|---|
| Android Auto over a **USB cable**. Sound and calls stay on the car's Bluetooth | `usb` | Yes |
| **Wireless off**: only the cable, the car's Bluetooth and Wi-Fi left alone | `wireless-off` | Not yet |
| iDrive **switches to Bluetooth audio** as the phone connects | `select-bt-audio` | Not yet |
| **Sound over Wi-Fi** instead of Bluetooth | `audio-wifi` | Not yet |

## Compatibility

| Car | iDrive software | Notes |
|---|---|---|
| 2019 BMW G20 | `202111011321` | The reference car |
| 2019 BMW G20 M340i | `202306092158` | Its Wi-Fi is 2.4 GHz only, shared with Bluetooth, so the wireless picture can lag. USB mode avoids it |

It needs an **EntryNav2** head unit: a 1280×480 screen and CarPlay Wi-Fi. NBT Evo, ID7 and MGU units are different
systems and can't run it. On iDrive software not listed here, `setup.sh` stops unless given `--other-software`.

## Installing

**With Flare Drive**, the companion Android app: it downloads a signed release, checks the car, installs, updates and
removes it, and holds the switches above.

**By hand**, with an ENET cable, a PC with SSH and the head unit's root password (not provided):

1. Back up the car's system: `tools/backup_headunit.sh` only reads.
2. Copy a release archive (from `tools/package_release.py`) to the head unit's `/tmp` and unpack it.
3. `sh setup.sh check` checks the car and writes nothing.
4. `sh setup.sh install`, then restart the head unit: `reboot`.
5. On iDrive: *Media*, *Android Auto*, *Start Android Auto*.

| On the head unit | |
|---|---|
| `sh /opt/aa/setup.sh status` | What is installed, and whether its files are intact |
| `sh /opt/aa/setup.sh uninstall` | Removes it. Settings and logs in `/var/local/data/AA` stay; `--purge` deletes them |

## Building

Needs Docker. The ARM programs are built against a copy of the car's system, made by `tools/backup_headunit.sh`. That
copy holds BMW files and never goes into git.

| Command | What it does |
|---|---|
| `git config core.hooksPath .githooks` | Once: before each push to `main`, check for secrets and run `verify_arm.py` |
| `docker build -t entrynav2-tc:4.7 toolchain` | Once: the cross compiler |
| `make check` | Style, content checks and the PC tests, as CI runs them |
| `python3 tools/verify_arm.py` | The ARM programs against the car's libraries, their tests under QEMU, the release checks |
| `python3 tools/package_release.py` | The release archive, after `verify_arm.py` |

CI can't hold the copy of the car's system, so it runs only `make check`. The pre-push hook runs `verify_arm.py` on
each commit pushed to `main`, and `tools/verify_tests.py` makes sure no test is left out of either.

## Licence

Files marked `SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0` hold code from earlier projects and are under the
[PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0) until they are
rewritten. No warranty: installing it modifies the head unit and may affect its warranty.

BMW and iDrive are trademarks of BMW AG. Android and Android Auto are trademarks of Google LLC.
