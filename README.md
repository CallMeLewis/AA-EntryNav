<div align="center">
  <img src="hmiapp/res/aa_content_preview_549x484.png" width="150" alt="AA-EntryNav icon">
  <h1>AA-EntryNav</h1>
  <p>Android Auto for BMW iDrive, wireless or over a USB cable.</p>
  <p>
    <a href="https://github.com/CallMeLewis/AA-EntryNav/releases"><img src="https://img.shields.io/github/v/release/CallMeLewis/AA-EntryNav?include_prereleases&amp;display_name=tag&amp;sort=semver" alt="Latest release"></a>
    <a href="https://github.com/CallMeLewis/AA-EntryNav/actions/workflows/ci.yml"><img src="https://github.com/CallMeLewis/AA-EntryNav/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  </p>
</div>

AA-EntryNav adds Android Auto to BMWs with the **EntryNav2** head unit, the one with a 1280×480 screen and CarPlay
over Wi-Fi. NBT Evo, ID7 and MGU units are different systems and can't run it. Start it from iDrive's *Media* menu and
your phone connects over the car's own Bluetooth and Wi-Fi, or over a USB cable. It runs alongside iDrive and replaces
none of BMW's files.

> [!WARNING]
> AA-EntryNav is experimental and writes to the head unit. Install it with the car parked, entirely at your own risk.
> The authors accept no responsibility for any damage to your car or loss of its warranty.

## Installing

You need an ENET cable, a PC with SSH and the head unit's root password. This project doesn't provide the password.

1. Back up the car's system with `tools/backup_headunit.sh`. It only reads.
2. Copy a release archive (from `tools/package_release.py`) to the head unit's `/tmp` and unpack it.
3. Run `sh setup.sh check`. It checks that the car has what AA-EntryNav needs and writes nothing.
4. Run `sh setup.sh install`, then restart the head unit with `reboot`.
5. On iDrive, open *Media*, *Android Auto*, *Start Android Auto*.

| On the head unit | |
|---|---|
| `sh /opt/aa/setup.sh status` | What is installed, and whether its files are intact |
| `sh /opt/aa/setup.sh uninstall` | Removes it. Settings and logs in `/var/local/data/AA` stay, and `--purge` deletes them |

## What it does

- **Wireless Android Auto.** The phone pairs over Bluetooth, then joins the car's own Wi-Fi.
- **Started from iDrive.** Open *Media*, *Android Auto* to start or stop it, or set it to start with iDrive.
- **The car's controls.** The iDrive controller, its buttons, the touch screen, and the steering wheel buttons and
  voice button all work in Android Auto.
- **The car's microphone and GPS.** Calls and the assistant use the car's microphone, and maps use the car's GPS.
- **Turn arrows in the cluster and head-up display.** Android Auto's navigation shows its next turn there too.

### Experimental, off until you switch it on

Each one is a file in `/var/local/data/AA` on the head unit. Create the file over SSH to switch the feature on, or
delete it to switch it off, then restart Android Auto.

| Feature | File |
|---|---|
| Android Auto over a **USB cable**, with sound and calls on the car's Bluetooth | `usb` |
| **Wireless off**, so only the cable works and the car's Bluetooth and Wi-Fi are left alone | `wireless-off` |
| iDrive **switches to Bluetooth audio** as the phone connects | `select-bt-audio` |
| **Sound over Wi-Fi** instead of Bluetooth | `audio-wifi` |

## Building it yourself

You need Docker. The build links the ARM programs against a copy of the car's system, which
`tools/backup_headunit.sh` makes. That copy holds BMW files and never goes into git.

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
rewritten. AA-EntryNav comes with no warranty.

AA-EntryNav is an independent project. It is not made or endorsed by BMW or Google. BMW and iDrive are trademarks of
BMW AG. Android and Android Auto are trademarks of Google LLC.
