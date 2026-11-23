<div align="center">
  <img src="hmiapp/res/aa_content_preview_549x484.png" width="150" alt="AA-EntryNav icon">
  <h1>Android Auto for EntryNav2</h1>
  <p>Android Auto for BMW iDrive, wireless or over a USB cable.</p>
  <p>
    <a href="https://github.com/CallMeLewis/AA-EntryNav/releases"><img src="https://img.shields.io/github/v/release/CallMeLewis/AA-EntryNav?include_prereleases&amp;display_name=tag&amp;sort=semver" alt="Latest release"></a>
    <a href="https://github.com/CallMeLewis/AA-EntryNav/actions/workflows/ci.yml"><img src="https://github.com/CallMeLewis/AA-EntryNav/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
    <a href="LICENSE.md"><img src="https://img.shields.io/badge/licence-PolyForm%20Noncommercial%201.0.0-blue" alt="Licence: PolyForm Noncommercial 1.0.0"></a>
  </p>
</div>

AA-EntryNav adds Android Auto to BMWs with the **EntryNav2** head unit, the one with a 1280×480 screen and CarPlay
over Wi-Fi. NBT Evo, ID7 and MGU units are different systems and can't run it. Start it from iDrive's *Media* menu and
your phone connects over the car's own Bluetooth and Wi-Fi, or over a USB cable. It adds two folders to the head unit
and leaves BMW's files as they are.

> [!WARNING]
> AA-EntryNav is experimental, and installing it changes the head unit's system. Install it with the car parked,
> entirely at your own risk. The authors accept no responsibility for any damage to your car or loss of its warranty.

## Installing

The easy way is **[Flare Drive](https://github.com/CallMeLewis/flare-drive-app)**, the companion Android app. It downloads a signed release and checks that your car
can run it before installing. Updates and removal happen there too, along with the switches for the experimental
features below. The [CHANGELOG](CHANGELOG.md) lists what changed in each release.

<details>
<summary><b>Installing by hand</b></summary>

You need an ENET cable, a PC with an SSH client and the head unit's root password. AA-EntryNav doesn't come with the
password.

1. Back up the car's system with `tools/backup_headunit.sh`. It only reads.
2. Copy a release archive (from `tools/package_release.py`) to the head unit's `/tmp` and unpack it.
3. Run `sh setup.sh check`. It checks that the car has what AA-EntryNav needs and writes nothing.
4. Run `sh setup.sh install`, then restart the head unit with `reboot`.
5. On iDrive, open *Media*, *Android Auto*, *Start Android Auto*.

| On the head unit | |
|---|---|
| `sh /opt/aa/setup.sh status` | What is installed, and whether its files are intact |
| `sh /opt/aa/setup.sh uninstall` | Removes it. Settings and logs in `/var/local/data/AA` stay, and `--purge` deletes them |

The [installation guide](docs/INSTALLATION.md) has every step in full, what each check looks at, and how to recover.

</details>

## What it does

Your phone finds the car over Bluetooth and gets the details of its Wi-Fi hotspot, the one CarPlay uses. Android Auto
then runs over that Wi-Fi and fills iDrive's screen.

- You start and stop it under *Media*, *Android Auto*, and can have it start along with iDrive.
- It answers to the car's own controls: the iDrive controller and its buttons, the touch screen, the steering wheel
  buttons and the voice button. With sound over Wi-Fi, MEDIA brings Android Auto back on its media app from any
  iDrive screen, and rolling the steering-wheel roller then pushing it changes track.
- Calls and the Assistant hear you through the car's microphone, and maps take their position from the car's GPS.
- Android Auto's next turn also shows in the instrument cluster and the head-up display, on cars whose cluster takes
  it over the car's network, such as the G20 and the X3 G01. F-series clusters don't show it yet.
- The phone is told the car's model, series and year, and which side the driver sits, from the `vehicle` file in the
  [settings guide](docs/MAINTENANCE.md).

### Experimental, off until you switch it on

Each one is a switch in Flare Drive, or a file in `/var/local/data/AA` on the head unit. The
[settings guide](docs/MAINTENANCE.md) lists every file, and how to turn on the logs.

| Feature | File |
|---|---|
| Android Auto over a **USB cable**, with sound and calls on the car's Bluetooth | `usb` |
| **Wireless off**, so only the cable works and the car's Bluetooth and Wi-Fi are left alone | `wireless-off` |
| iDrive **switches to Bluetooth audio** as the phone connects | `select-bt-audio` |
| **Sound over Wi-Fi** instead of Bluetooth: music on the car's speakers with the volume knob, announcements over the radio turn it down rather than stop it, and the cluster and Media/Radio tile show the current song and album art | `audio-wifi` |

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
the commit pushed to `main`, and `tools/verify_tests.py` makes sure every test runs in one or the other.

[ARCHITECTURE.md](docs/ARCHITECTURE.md) shows how the programs fit together, and [CONTRIBUTING.md](CONTRIBUTING.md)
says what a change needs.

## Credits and licence

AA-EntryNav started from entry-aad by Jiraiya43, now
[EntryNav2-Android-Auto](https://github.com/Jiraiya43/EntryNav2-Android-Auto), a port of
[Supra-iDrive6-Android-Auto](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto) by Sq00ky. [NOTICE.md](NOTICE.md)
says which files hold their code.

AA-EntryNav is under the [PolyForm Noncommercial License 1.0.0](LICENSE.md): you can use, change and share it for any
noncommercial purpose, but not sell it. AA-EntryNav comes with no warranty.

AA-EntryNav is an independent project. It is not made or endorsed by BMW or Google. BMW and iDrive are trademarks of
BMW AG. Android and Android Auto are trademarks of Google LLC.
