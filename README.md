<div align="center">
  <img src="hmiapp/res/aa_content_preview_549x484.png" width="150" alt="AA-EntryNav icon">
  <h1>AA-EntryNav</h1>
  <p>Android Auto for BMW iDrive. Wireless or over a cable, started from the Media menu.</p>
  <p>
    <a href="https://github.com/CallMeLewis/AA-EntryNav/releases"><img src="https://img.shields.io/github/v/release/CallMeLewis/AA-EntryNav?include_prereleases&amp;display_name=tag&amp;sort=semver" alt="Latest release"></a>
    <a href="https://github.com/CallMeLewis/AA-EntryNav/actions/workflows/ci.yml"><img src="https://github.com/CallMeLewis/AA-EntryNav/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  </p>
</div>

AA-EntryNav adds Android Auto to BMWs with the EntryNav2 head unit. Start it from iDrive's *Media* menu and your phone
connects over the car's own Bluetooth and Wi-Fi, or over a USB cable. It runs alongside iDrive and replaces none of
BMW's files.

> [!WARNING]
> Experimental software that writes to the head unit. Install it with the car parked, at your own risk.

## Installing

The easy way is **Flare Drive**, the companion Android app. It downloads a signed release, checks the car, then
installs, updates and removes AA-EntryNav, and it holds the switches for the experimental features below.

<details>
<summary><b>Installing by hand</b></summary>

You need an ENET cable, a PC with SSH and the head unit's root password, which isn't provided.

1. Back up the car's system with `tools/backup_headunit.sh`. It only reads.
2. Copy a release archive (from `tools/package_release.py`) to the head unit's `/tmp` and unpack it.
3. Run `sh setup.sh check`. It checks the car and writes nothing.
4. Run `sh setup.sh install`, then restart the head unit with `reboot`.
5. On iDrive, open *Media*, *Android Auto*, *Start Android Auto*.

| On the head unit | |
|---|---|
| `sh /opt/aa/setup.sh status` | What is installed, and whether its files are intact |
| `sh /opt/aa/setup.sh uninstall` | Removes it. Settings and logs in `/var/local/data/AA` stay, and `--purge` deletes them |

</details>

## What it does

- **Wireless Android Auto.** The phone pairs over Bluetooth, then joins the car's own Wi-Fi.
- **Started from iDrive.** *Media*, *Android Auto* starts and stops it, or starts it with iDrive every time.
- **The car's controls.** The iDrive controller, its buttons, the touch screen, and the steering wheel buttons and
  voice button all work in Android Auto.
- **The car's microphone and GPS.** Calls and the assistant use the car's microphone, and maps use the car's GPS.
- **Turn arrows in the cluster and head-up display**, from Android Auto's navigation.

### Experimental, off until you switch it on

Each one is a switch in Flare Drive, or a file in `/var/local/data/AA` on the head unit.

| Feature | File | Verified in a car |
|---|---|---|
| Android Auto over a **USB cable**, with sound and calls on the car's Bluetooth | `usb` | Yes |
| **Wireless off**, so only the cable works and the car's Bluetooth and Wi-Fi are left alone | `wireless-off` | Not yet |
| iDrive **switches to Bluetooth audio** as the phone connects | `select-bt-audio` | Not yet |
| **Sound over Wi-Fi** instead of Bluetooth | `audio-wifi` | Not yet |

## Compatibility

It needs an **EntryNav2** head unit, which has a 1280×480 screen and CarPlay over Wi-Fi. NBT Evo, ID7 and MGU units
are different systems and can't run it.

| Car | iDrive software | Notes |
|---|---|---|
| 2019 BMW G20 | `202111011321` | The reference car |
| 2019 BMW G20 M340i | `202306092158` | Its Wi-Fi is 2.4 GHz only and shares the radio with Bluetooth, so the wireless picture can lag. USB mode avoids it |

On iDrive software not listed here, `setup.sh` stops unless you give it `--other-software`.

## Building it yourself

You need Docker. The ARM programs are built against a copy of the car's system, made by `tools/backup_headunit.sh`.
That copy holds BMW files and never goes into git.

| Command | What it does |
|---|---|
| `git config core.hooksPath .githooks` | Once. Before each push to `main`, checks for secrets and runs `verify_arm.py` |
| `docker build -t entrynav2-tc:4.7 toolchain` | Once. Builds the cross compiler |
| `make check` | Style, content checks and the PC tests, as CI runs them |
| `python3 tools/verify_arm.py` | The ARM programs against the car's libraries, their tests under QEMU, and the release checks |
| `python3 tools/package_release.py` | The release archive, after `verify_arm.py` |

CI can't hold the copy of the car's system, so it runs only `make check`. The pre-push hook runs `verify_arm.py` on
each commit pushed to `main`, and `tools/verify_tests.py` makes sure every test runs in one or the other.

## Licence

Files marked `SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0` hold code from earlier projects and are under the
[PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0) until they are
rewritten. There is no warranty. Installing it modifies the head unit and may affect the car's warranty.

AA-EntryNav is an independent project. It is not made or endorsed by BMW or Google. BMW and iDrive are trademarks of
BMW AG. Android and Android Auto are trademarks of Google LLC.
