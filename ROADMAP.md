# Roadmap

A box gets ticked once a car has done it. Work that has passed only the PC tests stays unticked.

## On the reference car

The trial run on 10 November 2026, before 0.1.0, was in the maintenance profile.

- [x] `evo-trial --compat` checks compatibility and changes nothing
- [x] The manual installation steps, rehearsed with the car's BusyBox
- [ ] The whole installation guide, followed from start to finish
- [x] Trial run on the reference car
  - [ ] The check reports "Compatible", including its dry-run start
  - [x] When Android Auto comes up by itself (Start automatically, phone reconnected), it stays on screen and the
    iDrive controller only acts on Android Auto
  - [ ] After Media, Android Auto, Open Android Auto, iDrive stays on its blank page, also after a driving mode change
    followed by BACK
  - [ ] The volume badge shows the level, and Android Auto stays opaque
  - [ ] Android Auto closes about 5 seconds after the Wi-Fi drops
  - [ ] Stop Android Auto, then Start Android Auto
  - [ ] The iDrive controller (one step per notch), buttons, touch screen, driving modes, camera and Assistant
  - [ ] Waze on the car's GPS, and arrows on the head-up display
- [ ] Everything behaves the same with logging off (no `maintenance` file)

## On other cars, for 1.0

- [ ] A compatibility report from a second car, with nothing installed
- [ ] An install on another car, followed by a checked uninstall
- [ ] The table of tried software versions in [compatibility](docs/COMPATIBILITY.md) filled in
- [ ] The older iDrive controller (C10 and C20) tried in a real car

## Known problems

| Problem | Why | Next step |
|---|---|---|
| MAP opens BMW's map instead of Android Auto's navigation | MAP gives the screen back to iDrive, by choice for now | not decided |
| Sound over the car's Bluetooth is limited in quality and clips the start of voice prompts | sound over Wi-Fi exists but is experimental and off by default | try the `audio-wifi` file in a car |
| On head units with 2.4 GHz-only Wi-Fi, which shares the radio with Bluetooth, the wireless picture lags and stutters (measured on `202306092158`) | the car's radio | Android Auto over a USB cable (experimental, `usb` file), or a wireless adapter on the cable |
| Opening parking assistance without reverse gear leaves Android Auto in the background until "Open Android Auto" | AA-EntryNav can't tell which screen iDrive shows | fixed before 0.1.0 (comes back when iDrive returns focus), tested on PC only |
| During a call, the iDrive controller also moves through iDrive's call screen | the same | read which screen iDrive shows |
| The head-up display arrow stays away after a speed limit sign, and stays hidden beyond 1.6 km | how the cluster behaves | needs study |
| The cluster always showed the roundabout exit text in French | the text was fixed | fixed before 0.1.0 (follows the phone's language), tested on PC only |
| Every car had the same identity, so a phone used in two cars with AA-EntryNav mixed them up | `vehicleId` was fixed | fixed before 0.1.0 (`id=` line of the `vehicle` file), tested on PC only |

## Experimental features

| Feature | State |
|---|---|
| Sound over Wi-Fi instead of Bluetooth | not yet tried in a car |
| Android Auto over a USB cable | off by default, tried in a car. Pairing stays in iDrive |
| Wireless off, cable only | off by default, not yet tried in a car |
| iDrive switches to Bluetooth audio as the phone connects | off by default (`select-bt-audio`), not yet tried in a car |

## Later

| Idea | State |
|---|---|
| Read iDrive's current screen directly, for alerts, PDC and calls | needs study |
| Send the compass to the phone, so the map keeps its heading when the car stands still | idea |
| Open Android Auto from a dashboard favourite button | idea |
| Album artwork on iDrive | idea |
| Other screen sizes | waiting for a report from a car that has one |
| Bluetooth libraries with a different structure | refused for now: no report and no copy of such a library yet |
| A message on screen when Android Auto fails to start with logging off | idea |
| Updates that skip the full install procedure | idea |

## Not planned

- NBT Evo, ID7 and MGU head units. They run other systems, and these programs can't run on them.
- Cars from makers other than BMW.
