# Working on AA-EntryNav

Everything here is in English: the docs, code, comments, logs, commit messages and the CHANGELOG.

## Ways to help

- **Report how the check goes on your car.** `sh setup.sh check` from the [installation guide](docs/INSTALLATION.md)
  writes nothing to the head unit, so you can run it without installing. Open an issue with its output and the first
  line of `/etc/version`.
- **Report a bug.** Give the AA-EntryNav version and the car's software version. Logs help most: create the
  `maintenance` file, reproduce the problem, then attach `app.log` and the service log.
- **Send a pull request.** For anything bigger than a small fix, open an issue first so we can agree on the approach.

## Before your first push

Turn on the repository's hooks once after cloning:

```sh
git config core.hooksPath .githooks
```

The pre-push hook runs `tools/verify_repo.py`, which reads every file of every commit you push straight from git. It
looks for the car's root password if `.secrets/hu-password.txt` holds it, and for Flare Drive's release signing key if
`~/.flare-drive/release-signing-key` exists. When one of them is missing, it says so and runs the other checks. On a
push to `main` or a `release/*` branch, the hook also runs `tools/verify_arm.py` on each pushed commit.

`.gitignore` works as an allow list, so a new file stays out of git until it is listed there. These never go in:

- the car's root password, which lives in `.secrets/` on your PC only
- compiled programs
- BMW files: the backup of the car's system, its libraries, logs and captures. Tests that need them read
  `~/entrynav2/rootfs`, outside the repository. The short car recordings in `tests/fixtures/` are the one exception,
  and [tests/fixtures/README.md](tests/fixtures/README.md) says where each came from.
- anything that identifies a car or a person: the VIN, Bluetooth and Wi-Fi addresses, GPS positions

## Rules for code that runs in the car

A change must keep all of these true:

- Only `tools/setup.sh` writes to the car's system. It changes the two AA-EntryNav folders and nothing else, and only
  by renaming a complete folder it has already checked into place. A new kind of write goes into `setup.sh`, with a
  case in `tests/test_setup.py` that cuts the power at each step.
- AA-EntryNav adds two folders and replaces no BMW file. It never restarts a BMW service by itself, and uninstalling
  removes both folders.
- The compatibility check writes only to `/tmp`, which is in memory.
- When Android Auto stops, iDrive gets its Bluetooth back.
- Android Auto over a USB cable stays off until its `usb` file exists. In that mode, AA-EntryNav reads the car's
  Bluetooth stack without changing it.
- If the iDrive app's crash guard can't record a start, it keeps the app switched off (`hmiapp/crash_guard.hpp`).
- Any new traffic AA-EntryNav sends on the car's network goes in the CHANGELOG and the installation guide.
- Settings come from constants, command-line arguments and the switch files in `/var/local/data/AA`. Environment
  variables set nothing.
- Leave `vehicleId` in `core/sdr_builder.cpp` alone. The phone files its settings for the car under that value. A car
  that needs its own ID gets it from the `id=` line of the `vehicle` file. `tests/test_sdr_g20.cpp` pins the bytes the
  phone receives.

## Testing

| What | Command | Needs |
|---|---|---|
| The PC suite, as CI runs it | `make check` | `g++`, `libssl-dev`, `libdbus-1-dev`, `protobuf-compiler`, `python3-protobuf`, clang-format 18.1.8 and ruff 0.6.9 (`pip install -r requirements-dev.txt`) |
| ARM build | `bash toolchain/tc.sh make -j4 all hmiapp test-arm-all` | Docker and the backup of the car's system |
| ARM tests under QEMU | `python3 -I tests/run_qemu.py` | `qemu-user` and the backup |
| Checks on the programs that ship | `make verify_binary verify_wireless` | the same |
| The installation guide's commands | the commands in [docs/INSTALLATION.md](docs/INSTALLATION.md), run with the car's BusyBox under QEMU | the same |
| `setup.sh` and the backup script | `sudo python3 tests/test_setup.py --busybox "$(sh tests/build_busybox.sh)"`, which `make test-host BUSYBOX=...` also runs as root, as CI does | root, `chroot`, `curl` and a C compiler to build BusyBox. No backup needed |

The golden files in `tests/fixtures/` hold every byte that `test_session_phone`, `test_wireless_phone` and
`test_kombi_golden` send to the phone and the instrument cluster. Never regenerate one to get a test passing. A golden
file changes only when the behaviour is meant to change, and the commit and the CHANGELOG explain why.

The car runs BusyBox 1.21, so try any shell command meant for it under QEMU with that BusyBox first. It has no
`ip neigh`, no `netstat -p` and no character classes in `tr`. Processes started from an SSH session die when the
session closes.

A PC test is not a car test. Pull requests, the CHANGELOG and the ROADMAP all say which of the two a change has had,
and nothing counts as working until it has run in a car.

## Code style

| Language | Rules | Tools |
|---|---|---|
| C and C++ | GCC 4.7 with `-std=gnu++0x` (no C++14), `.clang-format` | `python tools/format_sources.py` (clang-format 18.1.8) |
| Python | 3.9 or later, a `main()` function, `pathlib`, no `assert` for checks | `python tools/format_sources.py`, `ruff check .` (0.6.9) |
| Shell | POSIX `sh` for BusyBox 1.21, ASCII only, 4-space indents | `shellcheck`, `shfmt -i 4` |

Each module tags its log lines with its own name, such as `[Video]` or `[KombiNav]`. The app's text on iDrive is in
English, and `hmiapp/make_db.py` holds its translation for each iDrive language.

### File headers

A file written for AA-EntryNav opens with the copyright line and one line on what the file does:

```cpp
// Copyright (c) 2026 CallMeLewis
// One line that says what the file does.
```

A file that holds entry-aad's code keeps its original header, `Copyright (c) 2026 Jiraiya43` with its
`SPDX-License-Identifier` line ([NOTICE.md](NOTICE.md)). `tools/verify_core.py` fails when a file has neither.

## Commits and branches

- Branch from `main`, one topic per branch: `fix/...`, `feat/...` or `docs/...`.
- Write messages as [docs/commit-messages.md](docs/commit-messages.md) describes. The subject is imperative, 72
  characters at most, with no `type:` prefix. The body explains why.
- Each commit holds one change, builds, and passes `make check`.
- Versions follow [SemVer](https://semver.org/), and each release gets a `vX.Y.Z` tag.

## Releasing

1. Write the release's notes under `## [Unreleased]` in `CHANGELOG.md`, Flare Drive note included, as
   [docs/release-notes.md](docs/release-notes.md) describes.
2. Run `tools/publish_release.py prepare X.Y.Z "<summary>"`. It turns the Unreleased part into the version's section,
   dated today and linked to the `vX.Y.Z` tag, and commits it with `VERSION` as `Release X.Y.Z: <summary>`. `preview`
   shows the Flare Drive note and the GitHub Release's text.
3. Push the commit. The pre-push hook builds it for ARM and runs the QEMU tests. Wait for CI to pass as well.
4. The same day, run `tools/publish_release.py publish`. It builds and tests the commit again, then sends it with its
   note to Flare Drive's plugin catalog through `flare-publish`, which also updates the release list older Flare Drive
   versions read. It also tags it `vX.Y.Z` and creates a GitHub Release with the archive, its `.sha256` and the text
   from the CHANGELOG section. It changes nothing in the repository, so there is nothing to commit afterwards.
   `flare-publish` is in Flare Drive's repo, which must be cloned beside this one (or set `FLARE_DRIVE_REPO`), and
   needs the publishing service token on this PC. Flare Drive's `docs/publishing.md` describes the process.
