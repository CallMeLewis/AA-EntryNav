# Fixtures

The tests read these files. A golden record changes only when the behaviour it records is meant to change, and the
commit says why. Never write a new one just so a test passes.

## Records the tests made

`session_phone.golden`, `wireless_phone.golden` and `kombi-golden.txt` hold every byte this project sent to a fake
phone and to the cluster. `test_session_phone`, `test_wireless_phone` and `test_kombi_golden` compare each run against
them.

## Records of the C++ for its Rust ports

Each folder holds what a C++ program gave, so that its Rust port can be checked against it after the C++ is deleted
([docs/rust-port.md](../../docs/rust-port.md)). While the C++ exists, `make test-host` checks that it still gives them.

- `evo_capture/` is from `tools/evo_capture.cpp`, recorded by `tests/test_capture_golden.cpp` (`make
  record-capture-golden`). `corpus.pcap` is a fixed set of IPv4 packets written by the tool's own pcap writer, each
  record's microseconds field naming the interface it came in on (0 `vbr0.73`, 1 `eth0`, 2 `wlan0`).
  `summary.golden` is the tool's summary of them and `selftest.golden` what `evo-capture --selftest` prints.
  `rust/evo-capture/tests/golden.rs` reads all three. The packets are made up, with no address or data from a car.

- `dbus/messages.bin` holds 239 D-Bus messages back to back, as a bus delivers them. `tests/test_ric_golden.cpp`
  uses libdbus to build and read them back. They include every type, both byte orders, the limits evo-ric cuts at, and
  200 reproducible random messages. `rust/aa-dbus/tests/wire.rs` checks that aa-dbus reads each message and writes
  each little-endian one back to libdbus's exact bytes. The two recorders below use this file too.
- `evo_ric/` holds fixtures from `tools/evo_ric.cpp` that `tests/test_ric_golden.cpp` writes (`make
  record-ric-golden`). `describe.golden` has the line printed for each message. `watch.golden` and `watch-small.golden`
  hold the watch log for all of them, without a limit and with a 3000 byte limit. `selftest.golden` captures
  `evo-ric --selftest` with `FILE` in place of the log path, and `usage.golden` holds the usage text.
  `rust/evo-ric/tests/golden.rs` reads these files.
- `evo_check/` holds fixtures from `tools/evo_check.cpp` and `platform/alps_spp.cpp` that
  `tests/test_check_golden.cpp` writes (`make record-check-golden`). `dump.golden` is `--dbus-watch`'s dump of each
  message in `dbus/messages.bin`. `alps-config.golden` has the Alps configuration summary for each
  `alps-config-*.xml` case and three unreadable ones, and `usage.golden` holds the usage text. `alps/` records what
  the Alps link of `--bluetooth` and `--monitor` did against a fake Alps service. It contains the requests the link
  sent in `requests.bin` (each with serial 0 and without the sender the bus adds), the signals it received in
  `signals.bin`, the service's unique name in `owner.txt`, and its log without times in `monitor.golden`.
  `fake-lmc.sh` stands in for LayerManagerControl when `tests/run_qemu.py` runs both programs' `--scene` side by side.
  `rust/evo-check/tests/golden.rs` reads these files. All configuration cases are made up, and none is BMW's file.

## Readings from a head unit

These come from a BMW EntryNav2 head unit, so the tests check the code against the car's own output:

- `compositor-entryevo.conf` is the unit's `/etc/wayland/compositor.conf`. `test_evo_ilm` reads its
  `genivi-flip-renderorder` line.
- `ilm-scene/` is what `LayerManagerControl` printed on the same unit for its screen, layers and surfaces.
  `test_evo_ilm` reads it too.
- `spp-bmw-uuids.txt` is the pair of Bluetooth serial port UUIDs as the unit stores them, in hex. `test_trial_recovery`
  reads it.

None of them hold personal data. There is no serial number, address or position in any of them.

## From another project

`gps_e7/` holds aasdk's `LocationData.proto`, which stays under the GPL-3.0. [Its README](gps_e7/README.md) says
where it comes from and how the test uses it.
