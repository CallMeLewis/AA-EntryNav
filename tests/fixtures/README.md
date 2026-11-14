# Fixtures

The tests read these files. A golden record changes only when the behaviour it records is meant to change, and the
commit says why. Never write a new one just so a test passes.

## Records the tests made

`session_phone.golden`, `wireless_phone.golden` and `kombi-golden.txt` hold every byte this project sent to a fake
phone and to the cluster. `test_session_phone`, `test_wireless_phone` and `test_kombi_golden` compare each run against
them.

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
