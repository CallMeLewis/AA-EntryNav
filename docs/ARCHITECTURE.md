# How AA-EntryNav is built

AA-EntryNav is three programs on the head unit, which is one ARMv7 computer running Linux 3.12 with glibc 2.15. Two of
them sit in `/opt/aa`. The third is a library that iDrive's own `HmiMain3D` loads at start-up.

| Program | Installed in | Job |
|---|---|---|
| `libAAEntryNav.so` | `/opt/hmi/data/ro/Plugins/AAEntryNav` | Adds the *Android Auto* row to iDrive's Media menu and runs its menu. It keeps iDrive out of the way while Android Auto is on screen, and its crash guard switches it off if iDrive keeps crashing |
| `evo-trial` | `/opt/aa` | The supervisor. It sets up Bluetooth and Wi-Fi, starts the service, and gives Bluetooth back to iDrive when it ends. With `--compat` it walks through a start without changing anything, for the compatibility check |
| `aa-entrynav` | `/opt/aa` | The Android Auto service: it talks to the phone and drives the screen, the controls and the cluster |

The programs live on the system partition because Linux won't run anything from `/var/local`. That partition stays
read-only except while `setup.sh` installs. Settings and logs go to `/var/local/data/AA` on the data partition.

## What happens when a phone connects

1. The iDrive app starts `evo-trial`, either when you pick *Start Android Auto* or by itself with iDrive.
2. The service offers Android Auto on the car's Bluetooth. When the phone connects there, the service hands it the
   name and password of the car's Wi-Fi.
3. The phone joins that Wi-Fi and opens a TCP connection. Everything after that runs inside TLS.
4. The service describes the car to the phone. The phone answers with an H.264 stream, and the service shows it on
   its own LayerManager layer. Over Wi-Fi the stream is 1280x720. Over the cable it is 1920x1080, unless the
   `video-720` file asks for 1280x720.
5. Touch, the controller and buttons, GPS, day and night, the microphone and turn-by-turn guidance each get a channel
   of their own.
6. A phone that sends nothing for 2 seconds gets a ping every second. If 5 seconds pass without a word, the service
   ends the session and waits for the phone to come back.

A phone on a USB cable, with the `usb` file, skips steps 2 and 3. The service switches it to accessory mode and runs
the same session over USB.

## Source folders

| Folder | What it holds |
|---|---|
| `core/` | The car's side of the Android Auto protocol. Nothing in it knows about BMW |
| `platform/` | The service and everything specific to the EntryNav2: picture, inputs, Bluetooth, Wi-Fi, USB, sound, cluster |
| `hmiapp/` | The iDrive app, `libAAEntryNav.so`, with its screens, texts and images |
| `tools/` | The supervisor, `setup.sh`, developer tools for the car, and the build, release and check scripts |
| `tests/` | The PC tests, the ARM tests run under QEMU, and their fixtures |

### `core/`

- **The session.** `aa_session` owns a phone's session from start to stop: its threads, the silence limit and the
  pings, and the dispatch of each message to its channel. `aa_channels.cpp` has one handler per channel (video, sound,
  input, sensors, microphone, navigation). `aa_protocol.hpp` names the channels, message types and flags.
- **The wire.** `aa_frame` splits and joins frames and refuses lengths that would run past its bounds. `aa_tls` wraps
  the session in OpenSSL, with RAII owners for its objects. `pb.hpp` writes protobuf and reads what the phone sends
  without trusting its lengths. `aa_messages` builds the messages the head unit sends.
- **Connecting.** `tcp_server` accepts one phone at a time on the car's Wi-Fi. `wireless_setup` is the Bluetooth
  exchange that gives the phone the Wi-Fi details.
- **Describing the car.** `sdr_builder` builds the service description: the screen and the canvas the phone draws
  on, the model, year and ID from the `vehicle` file, and only the sensors the car feeds. `include/` has the
  description's template and the head unit certificate, neither written by this project.
- **Car data for the session.** `aa_input.hpp` and `car_inbox` queue touches and key presses for the session.
  `car_location.hpp` turns the car's position into Android Auto's location sensor.
- **Helpers.** `crash_handler` logs a trace on a crash, then runs the clean-up the platform registered.
  `mutex_lock.hpp` holds a pthread mutex for a scope.

### `platform/`

- **The service.** `main.cpp` reads the command line and wires the modules together. `service_app` holds the running
  service's state and reacts to the session, the picture, the inputs and the app's commands. `service_contract.hpp`
  has the paths, the control channel's name and its protocol version, which the app, the supervisor and the service
  share. `evo_control` and `control_address.hpp` are that control channel: a Unix datagram socket with an abstract
  name, so there is no file and no network port, and only root or the service's own user may send on it.
- **Stepping aside.** `peek_timing` decides when the picture clears or dims so iDrive's volume bar, popups and driving
  mode changes show. `aside_wait` times how long a phone waits for the app's "show" before coming up anyway.
  `media_key_jump` decides when MEDIA, pressed over iDrive while Android Auto's sound plays, brings Android Auto back
  on its media app.
- **The picture.** `evo_video` feeds the H.264 stream to the car's decoder, `libxsvideo`, whose C interface
  `xsv_api.h` declares. `omap_dmabuf` holds the buffers that carry the stream, and `evo_ilm` places and stacks the
  surfaces through LayerManager's command-line tool. `evo_pill` shows the volume pill on a Wayland surface of its own,
  and `volume_pill.hpp` draws it.
- **Inputs.** `evo_knob` reads the iDrive controller, the buttons and the volume over MOST through `vmost_client`, the
  car's vMOST library loaded at run time. `evo_ccs.hpp` decodes both controller generations' messages. `evo_touch` and
  `evo_touch_frame.hpp` read the touchscreen's UDP packets. `input_mode.hpp` says what the inputs do during a run.
- **Bluetooth.** `alps_spp` drives BMW's Alps Bluetooth stack over D-Bus, and `alps_spp_codec.cpp` builds its
  messages. `evo_spp_snapshot` reads the stack's slot tables from memory without touching them, and `evo_elf.hpp`
  finds those tables among a library's exported symbols. `evo_trial_state` decides which original slot identifiers
  the supervisor saves and restores, and `evo_trial_wireless` registers Android Auto's identifier again while the slot
  is idle. `wireless_loop` hands each phone that connects on the slot to the Wi-Fi exchange. `bluez_adapter` asks
  BMW's BlueZ 4 for the car's address and paired phones, for cable sessions.
- **Wi-Fi.** `evo_wlan` turns the hotspot on the way BMW's CarPlay service does. `evo_ap` and `evo_ap_config.cpp` read
  the access point's name, password, BSSID and address.
- **The USB cable.** `usb_io` handles one USB device through usbfs. `usb_aoa` asks the phone for accessory mode.
  `usb_link` runs the steps from "a phone is plugged in" to "Android Auto answers", including taking the phone back
  from BMW's media program. `usb_bridge` moves the session's bytes between a local socket and the phone's endpoints.
  `usb_loop.hpp` polls the link for the service, keeping per-port retry pauses idle so wireless can connect and an
  observed unplug/replug can cancel the wait. The fake-phone test exercises the same polling turn.
- **Sound and microphone.** `evo_mic` feeds the car's microphone to the Assistant. For sound over Wi-Fi (the
  `audio-wifi` file), `evo_audio` receives the phone's two streams, `audio_mixer.hpp` mixes them, and `evo_alsa_out`
  plays the result through the car's own `libasound`.
- **GPS and the car description.** `evo_gps` reads the car's GPS without disturbing BMW's navigation or CarPlay.
  `evo_sdr_sensors` removes the sensors the car can't feed from the description.
- **Cluster and head-up display.** `kombi_nav` runs Android Auto's guidance on the cluster, with the `kombi-off` and
  `kombi-single-maneuver` files and an emergency cancel. `kombi_phone` reads the phone's navigation messages and
  `kombi_guide` turns them into what the cluster should show over time. `kombi_display` maps a maneuver to the
  cluster's direction, distance, lane and roundabout codes. `kombi_graph` and `kombi_graph_template.hpp` build the
  route frames from a recorded factory route start. `kombi_someip` frames them for SOME/IP service `0xB006`, and
  `kombi_client` sends them. `evo_bmw_guidance` checks whether BMW's own navigation is guiding, which takes priority.
- **Helpers.** `evo_log` has the monotonic clocks and the log writer. `evo_process` runs a command without a shell,
  with limits on time and output. `tmp_file` writes files in `/tmp` without ever following a link someone else put
  there.

### `hmiapp/`

- **The app.** `entry_aa_app.cpp` is the app object that `HmiMain3D` creates: the Media row, the menu, the focus of
  its states, and a once-a-second poll of the service. `bmw_framework` declares the parts of BMW's app framework it
  uses and the virtual table hooks it needs. `AAEntryNav.xml`, `AAEntryNav.manifest`, `res/` and `make_db.py` make its
  screens, texts and images, and `rhmi_ids.hpp` lists the XML's IDs the code uses.
- **Staying in front.** `hold_machine` keeps iDrive on the app's blank page while Android Auto is on screen and
  decides when to step aside. `popup_peek.hpp` handles iDrive popups. `car_signals` follows the gear, both phone
  lines, day and night, the driving mode and whether the car moves, read through `cds_parse.hpp`.
- **Talking to the service.** `service_link` starts the supervisor, sends commands to the service and stops it.
  `spawn_attr.hpp` sets up the process it starts from inside `HmiMain3D`. `service_state.hpp` reads the state line the
  service writes.
- **Bluetooth audio.** With the `select-bt-audio` file, `bt_audio_plan.hpp` picks the moment and `bt_audio` sends
  iDrive's own source switch to Bluetooth audio.
- **The car's own details.** `car_coding` reads the series and the driver's side from iDrive's coding, and
  `car_signals` takes the VIN's serial number from the car's data. `car_identity.hpp` turns them into vehicle file lines, which the app
  writes to `/tmp/aa-entrynav-car` for Flare Drive to offer. The app never writes the `vehicle` file itself.
- **The Flare Drive page.** `flare_page` asks for the PIN with iDrive's speller, then shows the QR code. `pin_lock.hpp`
  and `sha256.hpp` keep a salted hash of the PIN and the lockout after wrong tries. `flare_link.hpp` builds the link
  from `ap.conf`, `qr_code` encodes it, and `qr_png` writes the PNG iDrive displays. None of these use a library.
- **Safety and logs.** `app_guards` and `crash_guard.hpp` hold the on and off switches, the automatic start and the
  crash guard, which counts a start as soon as the library loads and stays off if it can't count. `app_log` and
  `log_profile.hpp` keep the log in the maintenance profile only. `mono_clock.hpp` times the app's delays from the
  head unit's start, so a change of the car's date can't stretch or cut them.

### `tools/`

- **On the car.** `evo_trial.cpp` is the supervisor. `setup.sh` checks, installs, reports, tidies and uninstalls, and
  it is the only thing that writes to the system ([what it guarantees](INSTALLATION.md#how-setupsh-protects-the-head-unit)).
- **Developer tools for the car**, built by `make dev-tools` and run by hand from `/tmp`, never installed:
  `evo_check.cpp` (Bluetooth and network), `evo_capture.cpp` (records SOME/IP), `evo_sound.cpp` (sound outputs),
  `evo_usb.cpp` (the cable link one step at a time) and `evo_mfl.cpp` (what each steering wheel button sends).
  `car_sound_probe.sh` records what each sound source opens.
- **On the PC.** `backup_headunit.sh` copies the car's system and only reads on the car. `pc_headunit.cpp` runs the
  real session against a phone's head unit server over `adb`, with no car at all.
- **Releases.** `package_release.py` builds the archive ([installation](INSTALLATION.md)). `publish_release.py`
  publishes it to Flare Drive's plugin catalog (through Flare Drive's `flare-publish`) and to GitHub, and `release_version.py` defines which version numbers it accepts and their
  order.
- **Checks.** `verify_arm.py` builds the ARM programs and runs everything that needs the copy of the car's system.
  `verify_binary.py` and `verify_wireless.py` check the ARM programs against the car's libraries and the approved
  fingerprints. `verify_repo.py` checks that nothing private or compiled reaches git, `verify_core.py` checks file
  headers and the licence notice, and `verify_tests.py` checks that every test runs somewhere. `format_sources.py`
  formats the C, C++ and Python.

## How it is tested

`make test-host` runs the logic on the PC, and `tests/run_qemu.py` runs the ARM programs under QEMU with the car's own
libraries. `make verify_binary verify_wireless` then checks the programs that ship.

Four tests compare against frozen records, byte for byte, and a change to those bytes has to be deliberate:

- `test_sdr_g20.cpp`, the car description sent to the phone.
- `test_session_phone.cpp`, a whole session against a fake phone.
- `test_wireless_phone.cpp`, the Bluetooth exchange that hands over the Wi-Fi.
- `test_kombi_golden.cpp`, every frame sent to the cluster.

Others cover the parts that are easy to get wrong in a car:

- When to step aside or wait: `test_peek_timing.cpp`, `test_hold_machine.cpp`, `test_popup_peek.cpp`,
  `test_aside_wait.cpp`, `test_session_silence.cpp`, `test_media_key_jump.cpp`.
- Life cycles: `test_wireless_lifecycle.cpp` for Bluetooth then Wi-Fi, `test_evo_control.cpp` for the control channel
  through the app's own calls.
- Hostile or odd input: `test_aa_frame.cpp`, `test_pb.cpp`, `test_parsers.cpp`.
- The cable: `test_usb.cpp` and `test_usb_link.cpp`, against a fake phone and a rival program.
- The Flare Drive page: `test_pin_lock.cpp`, `test_flare_link.cpp`, `test_qr_code.cpp` against a reference encoder,
  and `test_qr_png.cpp`, which reads the PNG back.
- Installing: `test_setup.py` runs `setup.sh` and the backup in a `chroot` with the car's BusyBox, from
  `tests/build_busybox.sh`, and cuts the power at every step.

The fixtures the tests read are listed in [tests/fixtures/README.md](../tests/fixtures/README.md).
