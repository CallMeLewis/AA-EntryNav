# Porting AA-EntryNav to Rust

[CLAUDE.md](../CLAUDE.md) sets the rule: new code is Rust, the C and C++ are ported piece by piece, and each C++ piece
stays until its Rust replacement has matched it in the car. Then the same change deletes the C++, its build rules and
the tests nothing else needs, after its outputs are recorded as fixtures. This page gives the order, how each step is
checked against the C++, what stays C++, and what each step deletes.

| Step | State |
|---|---|
| 1. `evo-capture` | Done on the PC and under QEMU. Waiting for its car run |
| 2. glibc Rust, `evo-rsprobe` | Done under QEMU with the car's glibc. Waiting for its car run |
| 3. `aa-dbus`, `evo-ric` | Done on the PC and under QEMU on the car's `dbus-daemon`. Waiting for its car run |
| 4. `evo-check` | Done on the PC and under QEMU against a fake Alps service. Waiting for its car run |
| 5 onwards | Not started. Step 5 needs vMOST reverse engineered and a capture from the car |

## What the head unit allows

The head unit runs Linux 3.12 and glibc 2.15 on an ARMv7 Cortex-A15. The C++ programs use these of the car's
libraries:

| Library | Used by | How |
|---|---|---|
| `libxsvideo.so.4`, `libdrm_omap.so.1`, `libdrm.so.2` | the service (`evo_video`, `omap_dmabuf`) | linked |
| `libssl.so.1.0.0`, `libcrypto.so.1.0.0` | the service (`aa_tls`) | linked |
| `libdbus-1.so.3` | the service, `evo-trial`, `evo-check`, `evo-ric`, `evo-usb` | linked |
| `libvmostclient.so.1` | the service (`evo_knob`, `audio_route`), `evo-trial` (`evo_wlan`), `evo-mfl`, `evo-sound` | `dlopen` |
| `libasound.so.2` | the service (`evo_mic`, `evo_alsa_out`), `evo-sound` | `dlopen` |
| `libwayland-client.so.0`, `libivi-application.so.0` | the service (`evo_pill`) | `dlopen` |
| `libAppFramework.A.so`, `libCoreFramework.A.so` | `hmiapp/libAAEntryNav.so` | linked, C++ ABI |

`LayerManagerControl` is a program that `evo_ilm` runs, so any Rust program can run it too.

### Two kinds of Rust program

**Static musl** (`armv7-unknown-linux-musleabihf`, linked by rust-lld, as EntryNav-Apps builds its workers). The
program needs nothing from the car but the kernel. It can't load any of the libraries above: a static program has no
dynamic loader, and even a dynamic musl program can't load a library built against glibc (another ABI, symbol
versions and thread-local storage layout). This kind fits every program that talks only to the kernel: sockets,
`AF_PACKET`, usbfs, `/proc`, D-Bus spoken over its Unix socket, and running `LayerManagerControl`.

**glibc Rust**: Rust built for `armv7-unknown-linux-gnueabihf` as a `staticlib`, linked by the build image's GCC 4.7
against the copy of the car's system, with a C `main` of a few lines that calls into it. It is the only way for Rust
to call a car library.

On 28 November 2026, the owner set the rule in CLAUDE.md. A program that loads no car library is static musl. Any
program that must load a car library (the service and its helpers) and any code inside HmiMain3D use glibc Rust. A
glibc Rust program needs nothing newer than `GLIBC_2.15`, and tests run it under QEMU with the car's glibc.

An experiment on 28 November 2026 checked that the second kind works, with Rust 1.94.1, `panic = "abort"` and LTO,
and step 2 made it part of the build:

- `arm-linux-gnueabihf-gcc` links the staticlib, including std, against the car's libraries. When std starts a thread,
  it calls `getauxval` to find the size of the thread's signal stack. That function arrived in glibc 2.16, so glibc
  2.15 lacks it. Every glibc Rust program links `rust/glibc-compat`, which defines `getauxval` from `/proc/self/auxv`.
  The program then needs `GLIBC_2.15` at most, along with `GCC_4.3.0` from `libgcc_s`, so it passes
  `tools/verify_binary.py`.
- Under QEMU with the car's glibc, std's threads, clocks, files, UDP, TCP and Unix sockets, `Command`, thread-locals,
  `Mutex`, `Condvar` and `HashMap` all worked. `dlopen` loaded every library the service uses, and the car's linked
  libssl created a TLS 1.2 client context.
- QEMU passes system calls to the PC's kernel, so nothing yet shows how the car's 3.12 kernel handles calls it lacks.
  Rust's std falls back when `getrandom`, `statx`, `clone3` or `pidfd_open` returns `ENOSYS`, and musl falls back from
  the 64-bit time calls. The car runs in steps 1 and 2 settle both kinds.

### What this forces

The service must load `libxsvideo` to show a picture, so it can never be a static musl program. Instead, it uses glibc
Rust. From step 8, `aa-entrynav` links one Rust staticlib (`rust/aa-service`) and calls the ported modules through a C
header. `make RUST_MODULES=...` switches individual modules to Rust, while C++ stays the default until the car run.
Once every module is Rust, only the `main` stub stays in C. Everything runs in one process with no new IPC.

### Dependencies

- **vMOST.** `libvmostclient.so.1` talks to the vMOST daemon over the Unix stream socket `/var/run/vmost_sockstream`
  (`platform/vmost_client.cpp` checks for it). Once that wire protocol is recorded, Rust can speak it natively, and
  `evo-trial`, `evo-mfl` and the knob become static musl. Until then they `dlopen` the library from glibc Rust. Step 5
  records it.
- **D-Bus.** The car's D-Bus is old (the build image carries 1.6's headers to match it). A small blocking client of our own (`rust/aa-dbus`: EXTERNAL auth, the
  marshalling of the types `alps_spp`, `bluez_adapter`, `evo_bmw_guidance` and `evo-ric` use, match rules) works in
  both kinds of program. The `dbus` crate needs libdbus, so static musl can't use it. zbus is async and large, and its
  messages would not match the recorded ones byte for byte.
- **TLS.** `core/aa_tls.cpp` is a TLS 1.2 client on the car's OpenSSL (`libssl.so.1.0.0`): `TLSv1_2_client_method`,
  a fixed cipher list, no verification, the head unit certificate. The phones have always seen that ClientHello.
  On 28 November 2026, the owner decided that the Rust service will keep the car's libssl for the phone session.
  Calling it over FFI from glibc Rust preserves the exact ClientHello bytes, and step 2's `evo-rsprobe` already builds
  a TLS 1.2 client context with it. Switching to rustls would change the ClientHello, and its crypto providers either
  compile C for armv7 or are young, so step 15 stays optional and is not planned.
- **libxsvideo, libdrm_omap, ALSA, Wayland.** Called through `extern "C"` blocks written by hand from
  `platform/xsv_api.h` and the C headers in the build image, or loaded with `dlopen` as now. Wayland's wire protocol
  could be spoken natively, since the volume pill draws into shared memory, but it gains nothing while the process
  links glibc anyway.

## What stays C++

- **The iDrive app's glue** in `hmiapp/`: `entry_aa_app`, `bmw_framework`, `cluster_player`, `hmi_thread`,
  `cluster_title`, `cover_art`, `car_coding`, `car_signals`, `bt_audio`, `flare_page` and `service_link`. They
  subclass BMW's classes, patch their virtual tables and pass `std::string` and other libstdc++ 3.4.17 types across
  the GCC 4.7 C++ ABI. Rust can't do that.
- **The C `main` stub** of each glibc Rust program, a few lines, so that the link stays inside the GCC 4.7 image.
- **The fakes of car libraries the tests load**: `tests/fake_vmostclient.c`, `tests/fake_asound.c`,
  `tests/lm_cli_stub.c`. They stand in for BMW's C libraries, so they keep their C interface. They are tests, not
  ported code.
- **`tools/setup.sh`** stays a BusyBox shell script, because the car has no other way to run an installer.

The pure logic in `hmiapp/` (`hold_machine`, `qr_code`, `qr_png`, `pin_lock.hpp`, `sha256.hpp`, `flare_link.hpp`,
`crash_guard.hpp`, `car_identity.hpp`, `utf16.hpp`, `popup_peek.hpp`, `bt_audio_plan.hpp`) could become a glibc Rust
staticlib inside `libAAEntryNav.so`. A panic there aborts HmiMain3D, which is iDrive itself, so it comes last and is
optional (step 16).

## How each step is checked

1. **Record the C++ first.** A fixture holds the C++'s exact output for fixed input. Where the existing frozen records
   cover a module (`session_phone.golden`, `wireless_phone.golden`, `kombi-golden.txt`, the description in
   `test_sdr_g20.cpp`), those are the oracle. Where they don't, a test-only C++ recorder writes new ones, as
   `tests/test_capture_golden.cpp` does for step 1. `make test-host` checks the C++ against every fixture, so a
   fixture can't drift while the C++ lives.
2. **Rust against the fixtures.** Each crate's `tests/` compiles the fixtures in with `include_bytes!` and must give
   the same bytes. `cargo test` runs them on the PC (CI's `rust` job), and `tools/build_rust.py` builds them for ARM so
   that `tests/run_qemu.py` runs them on the car's 32-bit CPU, where `usize` is 32 bits.
3. **Rust against the C++, side by side.** `RUST_CASES` in `tests/run_qemu.py` runs each Rust program and the C++
   program it replaces with the same arguments under QEMU, and their output and exit code must be the same.
   `tools/verify_tests.py` fails when a Rust program has no case, or a crate has no tests.
4. **The car run**, with the Rust behind its switch and the C++ still the default. Each step below says what to run
   and what output proves a match.
5. **The switch, then the deletion.** After the car run, the Rust becomes the default and the same change deletes the
   C++, its build rules, its C++ recorder and the tests nothing else needs. The fixtures stay, as the Rust's oracle.

## The order

Each step is small enough for one car run, and starts only when the one before has matched in the car. The early
steps are the developer tools, which are never installed, so a failure there costs nothing. The service comes after
the infrastructure it needs (glibc Rust, D-Bus, vMOST) has been proven by those tools.

| Step | Ports | Kind | Checked by | Deletes after the car run |
|---|---|---|---|---|
| 1 | `evo-capture` (537 lines) | musl | new fixtures (`tests/fixtures/evo_capture`), `--selftest` side by side | `tools/evo_capture.cpp`, `tests/test_capture_golden.cpp`, `host-evo-capture`, `host-capture-golden` |
| 2 | glibc Rust: `evo-rsprobe`, the experiment above as a developer tool, its GCC 4.7 link rule, and `rust/glibc-compat` | glibc | `verify_binary.py`'s GLIBC check, QEMU with the car's glibc | nothing; it proves the path |
| 3 | `rust/aa-dbus` and `rust/evo-text`, then `evo-ric` (804) | musl | new fixtures (`tests/fixtures/dbus`, `evo_ric`), both programs on the car's `dbus-daemon` under QEMU | `tools/evo_ric.cpp`, `tests/test_ric_golden.cpp` |
| 4 | `evo-check` (523), with the monitor mode of `alps_spp` and `evo_process` | musl | new fixtures (`tests/fixtures/evo_check`), each program on its own bus with a fake Alps service, and `--scene`, under QEMU | `tools/evo_check.cpp`, `tests/test_check_golden.cpp`, `tests/evo-check-host` once the tests that drive it use the Rust |
| 5 | vMOST: record the client's wire protocol, then `evo-mfl` (121) | musl, or glibc if the protocol can't be recovered | recorded exchanges, `test_evo_wlan`'s fake | `tools/evo_mfl.cpp` |
| 6 | `evo-usb` (295), with `usb_io`, `usb_aoa`, `usb_link`, `bluez_adapter` as a crate | musl | `test_usb`, `test_usb_link` (fake phone, rival program), recorded | `tools/evo_usb.cpp` |
| 7 | The supervisor `evo-trial` (729), with `evo_trial_state`, `evo_spp_snapshot`, `evo_elf`, `evo_ilm`, `evo_ap`, `evo_ap_config`, `evo_wlan` | musl with step 5's vMOST, else glibc | `test_trial_recovery`, `test_evo_ilm` with `ilm-scene/`, `test_real_lm_cli`, `test_evo_wlan`, `test_spp_snapshot`; `--compat` side by side | `tools/evo_trial.cpp` and the platform files only it uses |
| 8 | In the service: the cluster guidance, `kombi_*` (about 2,800) | glibc module | `kombi-golden.txt`, `test_kombi_*` | `platform/kombi_*` |
| 9 | Pure core logic: `pb.hpp`, `aa_frame`, `aa_messages`, `sdr_builder`, `canvas.hpp`, `evo_sdr_sensors`, `car_location.hpp`, `media_metadata.hpp`, `played_acks.hpp`, `keyframe_request.hpp` | glibc module | `test_sdr_g20`, `test_pb`, `test_aa_frame`, `test_aa_messages`, `test_parsers`, `session_phone.golden` | those files |
| 10 | Bluetooth handover: `wireless_setup`, `wireless_loop`, `alps_spp`, `evo_trial_wireless.hpp` | glibc module | `wireless_phone.golden`, `test_wireless_lifecycle` | those files |
| 11 | The session: `aa_session`, `aa_channels`, `aa_tls` (car libssl by FFI), `tcp_server`, `car_inbox` | glibc module | `session_phone.golden`, `test_session_silence`, `test_tcp_server` | `core/` but `crash_handler` |
| 12 | Inputs and sensors: `evo_touch`, `evo_touch_frame.hpp`, `evo_knob`, `evo_ccs.hpp`, `evo_gps.hpp`, `wheel_roller.hpp`, `media_key_jump.hpp`, `input_mode.hpp` | glibc module | `test_evo_input`, `test_knob_fallback`, `test_evo_gps`, `test_gps_wire_oracle`, `test_wheel_roller`, `test_media_key_jump` | those files |
| 13 | The service's core: `main`, `service_app`, `evo_control`, `control_address.hpp`, `peek_timing`, `aside_wait.hpp`, `app_heartbeat.hpp`, `crash_handler`, `evo_log`, `evo_process`, `tmp_file` | glibc, Rust `main` | `test_evo_control` (also on ARM), `test_peek_timing`, `test_aside_wait`, `test_wireless_lifecycle` | those files |
| 14 | The car library adapters: `evo_video`, `omap_dmabuf`, `evo_pill`, `volume_pill.hpp`, `volume_filter.hpp`, `evo_mic`, `evo_alsa_out`, `evo_audio`, `audio_route`, `audio_mixer.hpp`, `ap_uapsd.hpp`, `evo_bmw_guidance`, `usb_bridge`, `usb_loop.hpp` | glibc | `test_video_presentation`, `test_evo_audio`, `test_audio_mixer`, `test_volume_pill`, `test_volume_filter`, `test_bmw_guidance`, `test_prompt_path` | the rest of `platform/` but `service_contract.hpp` |
| 15 | Optional: rustls in place of the car's OpenSSL | glibc | `session_phone.golden` can't hold (new ClientHello); a fake phone on OpenSSL 3 and several real phones | the libssl FFI |
| 16 | Optional: the pure logic of `hmiapp/`, panic-free | glibc staticlib in `libAAEntryNav.so` | `test_hold_machine`, `test_qr_code`, `test_qr_png`, `test_pin_lock`, `test_flare_link`, `test_crash_guard`, `test_utf16`; `smoke_check.py` on a car report | those files |

`platform/service_contract.hpp` stays, as the contract the app, the supervisor and the service share. Once the service
is Rust, a test checks that the Rust constants equal the header's.

### Step 1: `evo-capture`

The passive SOME/IP recorder: one `AF_PACKET` socket, receive only, a pcap and a text summary. It is a developer tool,
never installed, and needs nothing but the kernel, so it was the one program that could be ported first with every
piece of the pipeline: the Cargo workspace, the musl build, the fixtures, the QEMU comparison and the switch.

- `rust/evo-capture`: `src/lib.rs` is the parser, the summary and the pcap writer, `src/main.rs` the recording, and
  `examples/summarize.rs` summarizes a pcap from the car.
- `tests/test_capture_golden.cpp` builds a fixed set of 377 packets (the self test's, every SD entry and option kind,
  every SOME/IP type, TCP segments, IPv4 header edge cases, and 320 reproducible random ones) and runs them through
  the C++ `tools/evo_capture.cpp` itself. It wrote `tests/fixtures/evo_capture/corpus.pcap`, `summary.golden` and
  `selftest.golden`, and `make test-host` checks that the C++ still gives them.
- `rust/evo-capture/tests/golden.rs` checks that the Rust gives the same summary, pcap bytes and self test output.
- `tests/run_qemu.py` runs both programs' `--selftest` on ARM and compares them with each other and with the fixture,
  then runs the Rust tests built for ARM.
- `make dev-tools RUST_TOOLS=evo-capture` puts the Rust program in place of the C++ one. Without it, `make dev-tools`
  builds the C++ as before.

**The car run.** `python3 tools/verify_arm.py` builds both: the C++ `evo-capture` in the repository root and the
Rust `build/rust/evo-capture`. Park the car. A session with guidance on the cluster gives the most SOME/IP traffic to
compare, but any running head unit sends some. From the PC on the ENET cable (through `ssh`, not `scp`, which needs an
SFTP server the head unit may not have):

```sh
ssh root@169.254.1.0 "cat > /tmp/evo-capture-cpp && chmod 700 /tmp/evo-capture-cpp" < evo-capture
ssh root@169.254.1.0 "cat > /tmp/evo-capture-rs && chmod 700 /tmp/evo-capture-rs" < build/rust/evo-capture
ssh root@169.254.1.0 "LD_LIBRARY_PATH=/usr/local/lib:/usr/lib:/lib /tmp/evo-capture-cpp --selftest" > cpp-selftest.txt
ssh root@169.254.1.0 "/tmp/evo-capture-rs --selftest" > rs-selftest.txt
cmp cpp-selftest.txt rs-selftest.txt && cmp rs-selftest.txt tests/fixtures/evo_capture/selftest.golden
ssh root@169.254.1.0 "LD_LIBRARY_PATH=/usr/local/lib:/usr/lib:/lib /tmp/evo-capture-cpp --someip 62" > cpp-live.txt &
sleep 2; ssh root@169.254.1.0 "/tmp/evo-capture-rs --someip 60" > rs-live.txt; wait
ssh root@169.254.1.0 "cd /tmp/entry-aa-releve && tar cf - someip-*.pcap" | tar xf -
ssh root@169.254.1.0 "rm -r /tmp/evo-capture-cpp /tmp/evo-capture-rs /tmp/entry-aa-releve"
```

The two recorders read the same traffic side by side, each with its own socket. They start two seconds apart, because
each names its pcap after the second it started. Three things prove the match:

1. Both self tests print the same as `selftest.golden` and end in `PASS evo-capture selftest`. That proves the parser
   on the car's CPU, and that a static musl program runs on the car's kernel.
2. The two live summaries list the same SOME/IP pairs and SD lines. Their counts differ slightly, since one ran two
   seconds less:

   ```sh
   keys() { grep -E '^svc |^  x' "$1" | sed -e 's/^  x[0-9]* *//' -e 's/: [0-9]* messages.*//' | sort -u; }
   diff <(keys cpp-live.txt) <(keys rs-live.txt)
   ```

   A pair seen only once near the start or end may show in one list only. Anything else is a failure.
3. Each pcap, summarized by both on the PC, gives identical text: real car traffic through both parsers.

   ```sh
   make host-capture-golden   # builds tests/test-capture-golden (in the CI image or WSL on Windows)
   for f in someip-*.pcap; do
       ./tests/test-capture-golden --summarize "$f" > "$f.cpp.txt"
       (cd rust && cargo run -q --example summarize -- "../$f") > "$f.rs.txt"
       cmp "$f.cpp.txt" "$f.rs.txt"
   done
   ```

After that run, the next change makes the Rust the default (`dev-tools` copies `build/rust/evo-capture`), deletes
`tools/evo_capture.cpp`, `tests/test_capture_golden.cpp`, the `host-evo-capture` and `host-capture-golden` targets,
`CAPTURE_OBJ` and the `capture-selftest` QEMU case, and keeps `tests/fixtures/evo_capture` for `golden.rs`.

### Step 2: glibc Rust and `evo-rsprobe`

This step brings the second kind of Rust program into the build. It replaces nothing and ships in no release.

- `rust/evo-rsprobe` builds as a staticlib for `armv7-unknown-linux-gnueabihf`, and as an rlib for its tests.
  `tools/build_rust.py` puts the archive at `build/rust/libevo_rsprobe.a` without needing a linker on the PC. Its C
  `main`, `rust/evo-rsprobe/main.c`, is four lines of code.
- `make evo-rsprobe` links the archive and `main.c` inside the build image with GCC 4.7 against the copy of the car's
  system. The link adds the car's libssl and libcrypto, plus `-ldl -lpthread -lrt -lm -lgcc_s` for std. Because
  `test-arm-all` builds it, `make test-arm-all` needs `make rust` first. `rust/glibc-compat` supplies the `getauxval`
  that glibc 2.15 lacks.
- The program prints one line per check. It starts with std (threads, thread-locals, `Mutex` and `Condvar`, both
  clocks, `HashMap` random keys, files and metadata, UDP, TCP and Unix sockets, `/proc` and starting `/bin/sh`), then
  checks `getauxval`, each car library the service uses with the functions it looks up, and a TLS 1.2 client context
  from the linked libssl. Each check line ends in `, ok`, and the final line prints `PASS evo-rsprobe: 24 checks`.
- `tools/verify_binary.py` checks that the program needs `GLIBC_2.15` at most, and `tests/run_qemu.py` runs it with
  the car's glibc as case `rsprobe`. On the PC, `cargo test` runs the std checks too.

**The car run.** `python3 tools/verify_arm.py` builds `evo-rsprobe` in the repository root. Park the car and connect
the PC over the ENET cable, then run:

```sh
ssh root@169.254.1.0 "cat > /tmp/evo-rsprobe && chmod 700 /tmp/evo-rsprobe" < evo-rsprobe
ssh root@169.254.1.0 "LD_LIBRARY_PATH=/opt/hmi/lib:/usr/local/lib:/usr/lib:/lib /tmp/evo-rsprobe; echo exit \$?" \
    | tee rsprobe-car.txt
ssh root@169.254.1.0 "rm /tmp/evo-rsprobe"
```

The program loads the car's libraries but calls nothing in them apart from libssl's context functions. It creates and
removes one folder under `/tmp` and talks only to itself on the loopback interface. For a match, every line except
the last two must end in `, ok`, followed by `PASS evo-rsprobe: 24 checks` and `exit 0`. Any `FAILED` line names what
the car's kernel or libraries do differently from QEMU. The `random`, `files` and `thread` lines catch a missing
`getrandom`, `statx` or `clone3` if std fails to fall back.

### Step 3: `aa-dbus` and `evo-ric`

`evo-ric` records cluster guidance calls on the session bus and sends nothing to BMW's programs. Because the port is a
static musl program, it needed a D-Bus client independent of libdbus.

- `rust/aa-dbus` speaks the bus protocol over its Unix socket. It supports `path=` and `abstract=` addresses, EXTERNAL
  authentication, Hello, calls and answers, signals, match rules, and every wire format type in both byte orders. Its
  messages match libdbus 1.6's layout with the same header field order and answer flags. `rust/evo-text` supplies
  glibc's `%g`, strtol, strtoul and error texts, which musl does not always match.
- `tests/test_ric_golden.cpp` uses libdbus to build 239 D-Bus messages (every type, the limits evo-ric cuts at, two
  big-endian messages, and 200 reproducible random ones) and runs them through `tools/evo_ric.cpp`. It recorded
  `tests/fixtures/dbus/messages.bin` and the outputs under `tests/fixtures/evo_ric`. Those files hold the line for
  each message, the watch log for all of them with and without its size limit, the self test output and the usage
  text. `make test-host` checks that the C++ still produces them.
- `rust/aa-dbus/tests/wire.rs` checks that aa-dbus reads every message and writes each little-endian one back to
  libdbus's exact bytes. `rust/evo-ric/tests/golden.rs` checks that the Rust port produces each recorded output.
- `tests/run_qemu.py` starts the car's own `dbus-daemon` (1.6) under QEMU on a private socket. Both programs pass
  their self tests on it and print the same `--names`. `rust/evo-ric/examples/ric_play.rs` then plays the corpus on
  that bus until the bus closes, and both record it. Once you strip each line's time along with the header's date and
  uptime, the two logs must match, and both must record every RIC call the player sent.
- `make dev-tools RUST_TOOLS=evo-ric` replaces the C++ program with the Rust one.

**The car run.** Never run `--selftest` on the car, because it claims `BMW.RueKoIC` on whichever bus it uses. Both
recorders listen side by side, each on its own connection, during a factory navigation route with guidance on the
cluster:

```sh
L="LD_LIBRARY_PATH=/usr/local/lib:/usr/lib:/lib"
ssh root@169.254.1.0 "cat > /tmp/evo-ric-cpp && chmod 700 /tmp/evo-ric-cpp" < evo-ric
ssh root@169.254.1.0 "cat > /tmp/evo-ric-rs && chmod 700 /tmp/evo-ric-rs" < build/rust/evo-ric
ssh root@169.254.1.0 "$L /tmp/evo-ric-cpp --names" > cpp-names.txt
ssh root@169.254.1.0 "/tmp/evo-ric-rs --names" > rs-names.txt
diff cpp-names.txt rs-names.txt
ssh root@169.254.1.0 "$L /tmp/evo-ric-cpp --watch 10 --out /tmp/ric-cpp" > cpp-watch.txt &
ssh root@169.254.1.0 "/tmp/evo-ric-rs --watch 10 --out /tmp/ric-rs" > rs-watch.txt &
# start the route now; both stop after 10 minutes
wait
ssh root@169.254.1.0 "cat /tmp/ric-cpp/ric-*.log" > cpp.log
ssh root@169.254.1.0 "cat /tmp/ric-rs/ric-*.log" > rs.log
ssh root@169.254.1.0 "rm -r /tmp/evo-ric-cpp /tmp/evo-ric-rs /tmp/ric-cpp /tmp/ric-rs"
```

Three checks prove the match:

1. The two `--names` lists match. A pid differs only if a program restarted between the two runs.
2. Both logs list the same messages with the same arguments. Strip each line's time, the header and the heartbeats
   before comparing them:

   ```sh
   norm() { grep -v -e '^# evo-ric watch:' -e '^# alive ' "$1" | sed -e 's/^# end after [0-9]* ms/# end after/' \
       -e '/^#/!s/^[0-9]* //'; }
   diff <(norm cpp.log) <(norm rs.log)
   ```

   Because the two started a second apart, a message at the very start or end may appear in one log only. Anything
   else is a failure.
3. The final lines of `cpp-watch.txt` and `rs-watch.txt` report roughly the same counts of calls, answers and signals.

After that run, the next change makes Rust the default, deletes `tools/evo_ric.cpp`, `tests/test_ric_golden.cpp`,
the `host-evo-ric` and `host-ric-golden` targets, `RIC_OBJ` and the `ric-load` QEMU case, points
`tools/ric_capture.sh` at the Rust build, and keeps the fixtures for `golden.rs` and `wire.rs`.

### Step 4: `evo-check`

`evo-check` reads Bluetooth, D-Bus, the IVI scene and the system without changing anything. Its port is a static musl
program built on aa-dbus.

- `rust/evo-check` holds the whole program. Its Bluetooth modes carry the monitor mode of `platform/alps_spp.cpp`,
  where the link logs every SPP and DM signal from the Alps service and sends only Introspect and configuration reads.
  Monitor mode refuses all writes, so the port omits them. `--scene` and `--context` run commands the way
  `platform/evo_process.cpp` does.
- The car's glibc 2.15 forces one adjustment. It starts a child before executing a program, so a missing program shows
  as `code=127 ... spawn_error=0`. Because musl reports the error directly instead, the port matches glibc 2.15's
  output.
- `tests/test_check_golden.cpp` runs the C++ and wrote the fixtures in `tests/fixtures/evo_check`. They hold
  `--dbus-watch`'s dump of the D-Bus corpus, the Alps configuration summary for four made-up cases and three
  unreadable ones, the usage text, and a trace of the Alps link against a fake Alps service. That trace records the
  request bytes it sent (Introspect and two configuration reads), the signals it received (every SPP and DM signal it
  reads, with valid and invalid signatures, followed by the service giving up its name), and its log of them.
  `make test-host` checks that the C++ still produces them.
- `rust/evo-check/tests/golden.rs` checks that the Rust port produces the same dump, summaries and usage, builds the
  same request bytes, and writes the same log for the same signals.
- `tests/run_qemu.py` runs each program alone on a fresh bus from the car's `dbus-daemon`, with
  `rust/evo-check/examples/alps_fake.rs` as the Alps service, so both see the same unique names. It tests
  `--bluetooth` with and without the service, `--monitor` while the service sends signals and gives up its name, plus
  `--dbus-names`, `--dbus-introspect` and `--dbus-watch`. After you strip the log times, outputs must match. Both
  programs also run `--scene` against a missing program and against `tests/fixtures/evo_check/fake-lmc.sh`, which
  tests each command failure (an exit status, a failure with status zero, output past 64 KiB, no answer in time, a
  signal and a NUL).
- `run_qemu.py` skips `--context` because it runs the car's ARM BusyBox, which QEMU cannot start from inside a
  program it runs. The car run compares it instead.
- `make dev-tools RUST_TOOLS=evo-check` replaces the C++ program with the Rust one.

**The car run.** Park the car with iDrive running and the phone paired, without starting Android Auto:

```sh
L="LD_LIBRARY_PATH=/usr/local/lib:/usr/lib:/lib"
ssh root@169.254.1.0 "cat > /tmp/evo-check-cpp && chmod 700 /tmp/evo-check-cpp" < evo-check
ssh root@169.254.1.0 "cat > /tmp/evo-check-rs && chmod 700 /tmp/evo-check-rs" < build/rust/evo-check
for mode in --bluetooth --dbus-names "--dbus-introspect org.alps.evolution /org/alps/evolution/hli" --scene \
    --context; do
    ssh root@169.254.1.0 "$L /tmp/evo-check-cpp $mode; echo exit \$?" > "cpp$(echo $mode | tr ' /' '__').txt"
    ssh root@169.254.1.0 "/tmp/evo-check-rs $mode; echo exit \$?" > "rs$(echo $mode | tr ' /' '__').txt"
done
ssh root@169.254.1.0 "$L /tmp/evo-check-cpp --monitor 60" > cpp-monitor.txt &
ssh root@169.254.1.0 "/tmp/evo-check-rs --monitor 60" > rs-monitor.txt &
# within the minute: turn the phone's Bluetooth off, wait 10 seconds, turn it on again
wait
ssh root@169.254.1.0 "rm /tmp/evo-check-cpp /tmp/evo-check-rs"
```

Three checks prove the match:

1. `--bluetooth`, `--dbus-names`, `--dbus-introspect` and `--scene` print the same text and return the same exit code
   once you strip the leading uptime from each log line (`sed 's/^\[ *[0-9.]*\] //'`). A pid in `--dbus-names`
   differs only if a program restarted between runs. Both `--bluetooth` runs send the Alps service the same bytes,
   which `golden.rs` proves against libdbus.
2. `--context` prints the same sections in the same order with the same `[OK]` or `[FAILED]` line under each
   (`grep -E '^(---|\[OK\]|\[FAILED\])'`). The uptime, network counters and free space may differ.
3. Without timestamps, the two `--monitor` logs list the same signals and connections in the same order. Because one
   started a second after the other, a signal at the very start may appear in one log only.

After that run, the next change makes Rust the default, points `tests/test_evo_ilm.cpp` and
`tests/test_alps_spp.cpp` at the Rust program, deletes `tools/evo_check.cpp`, `tests/test_check_golden.cpp`,
`tests/evo-check-host`, `CHECK_OBJ`, the `host-check-golden` target and the `check-load` QEMU case, and keeps the
fixtures. `platform/alps_spp.cpp` and `platform/evo_process.cpp` stay for the service and the supervisor.

### Steps 5 to 7: vMOST, the cable tool and the supervisor

- **Step 5** records vMOST. Ghidra on `libvmostclient.so.1` from the copy of the car's system gives the framing. Under QEMU, the real library connected to a recording socket gives what
  the client sends. In the car, a small `LD_PRELOAD` logger on the C++ `evo-mfl`, which only sends Gets, records the
  daemon's answers. With that, `rust/aa-vmost` speaks the protocol and `evo-mfl` ports to static musl. If the protocol
  can't be recovered, `evo-mfl` and step 7 use `dlopen` from glibc Rust instead. Car run: both `evo-mfl 90` while the
  same buttons are pressed in the same order, same lines.
- **Step 6** ports `evo-usb` and the cable link as a crate. Car run: `evo-usb list`, `evo-usb link` and
  `evo-usb bluetooth` with each program, same output, and the phone handed back each time.
- **Step 7** ports the supervisor, the first program a release ships. `tools/package_release.py` gets
  `--supervisor rust|cpp`, `cpp` the default, as EntryNav-Apps' `--workers` does. Car run: `evo-trial --compat` with
  each, same report (it changes nothing). Then a test release with the Rust supervisor, through Flare Drive, never
  stable: Start and Stop Android Auto, start along with iDrive, and Bluetooth given back to iDrive after the service
  ends (`bluetooth-retour.txt` gone, CarPlay pairs again). It deletes `tools/evo_trial.cpp`, and the platform files
  only the supervisor used. The service still links `alps_spp`, `evo_spp_snapshot`, `evo_ap` and `vmost_client` until
  steps 10 to 14.

### Steps 8 to 14: the service

Each step moves modules of `aa-entrynav` into `rust/aa-service`, behind `RUST_MODULES`. The C++ calls the Rust
through `rust/aa-service/include/aa_service.h`, plain C functions and structures only. `package_release.py` gets
`--service-modules`, empty by default, so a test release can carry the Rust modules while every stable release keeps
the C++ until the car run.

Each car run is the matching part of the trial in [ROADMAP.md](../ROADMAP.md): arrows on the cluster and head-up
display for step 8, a whole session with the phone's sensors and media data for 9, the Bluetooth then Wi-Fi handover
for 10, a ten minute session with a radio switch and back and the 5 second silence limit for 11, the controller one
step per notch, buttons, touch and GPS for 12, Stop and Start and the app's commands for 13, and the picture, the
volume pill, sound over Wi-Fi and the microphone for 14. Each run ends with `python tools/smoke_check.py` on a
diagnostic report, as for a change to `hmiapp/`.

After step 14 the service is a Rust program with a C `main` stub. `core/` and `platform/` are gone, but for
`service_contract.hpp`.

## Risks

- **The car's kernel.** QEMU used the PC's kernel. A system call 3.12 lacks fails only in the car. Steps 1 and 2 are
  cheap car runs that find this before anything that matters depends on it.
- **A panic is a crash.** Every Rust program builds with `panic = "abort"`. In the service, `crash_handler` must still
  give the screen back to iDrive on `SIGABRT`; step 13 tests that under QEMU and in the car. Code that runs in
  HmiMain3D (step 16) must not panic at all.
- **Size.** The static `evo-capture` is 366 KB against the C++'s 34 KB, since it carries its own C library. The
  installed programs sit on the system partition, so step 7 checks the free space `setup.sh check` asks for.
- **Two toolchains in one link.** LLVM objects linked by GCC 4.7's binutils worked in the experiment. A future Rust
  release could emit something the old linker refuses. The toolchain stays pinned in `rust/rust-toolchain.toml`, and
  each change of it runs `verify_arm.py`.
- **Fixtures that miss behaviour.** A fixture holds only what its inputs exercise. Each recorder covers the edge
  cases on purpose (step 1's corpus has every branch of the parser), and the side by side comparison under QEMU and
  in the car catches what the fixtures miss.
- **Matching protocols written by hand.** The D-Bus and vMOST clients must match what BMW's daemons accept. Steps 3
  to 5 prove them with read-only tools before the supervisor or the service depends on them.
  aa-dbus already works with the car's `dbus-daemon` 1.6 under QEMU and writes libdbus's exact bytes for every
  message in the corpus.
- **glibc and musl differ.** A musl program prints numbers and errors, and starts programs, as musl does, which is
  not always as the car's glibc 2.15 does. The tools found three: `%g`, error texts, and how posix_spawnp reports a
  program it cannot run. `rust/evo-text` and `evo-check` give glibc's behaviour, and the side by side runs under
  QEMU compare against the C++ on the car's own glibc, so a fourth would show there.
- **CI has no Rust in its image.** A separate `rust` job in `.github/workflows/ci.yml` runs `cargo test`, Clippy and
  rustfmt on the runner's own rustup. The ARM side runs in `verify_arm.py`, which now requires cargo on the PC.

## Decisions

The owner made these decisions on 28 November 2026:

1. Any program that must load a car library, meaning the service and its helpers, uses glibc Rust (a staticlib that
   GCC 4.7 links). CLAUDE.md records this rule. Programs that load no car library stay static musl.
2. The Rust service keeps the car's libssl for the phone session. rustls (step 15) stays optional and is not planned.

Whether the pure logic of `hmiapp/` moves at all (step 16) stays open.
