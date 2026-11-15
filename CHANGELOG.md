# Changelog

All notable changes to this project. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow [SemVer](https://semver.org/).

Each version says whether it is verified or unverified. Verified means this exact build has run in a car. Unverified
means it has only been tested on PC: the PC tests passed and the ARM programs ran under QEMU with the car's libraries.
Each version links to the commit its archive was built from.

## [Unreleased]

### Added

- **The phone can show the car's series.** A `series=F20` line in the `vehicle` file makes the phone show "1 Series
  (F20)", or "M140i (F20)" with `model=M140i`. It also sets the make for MINI, Rolls-Royce and the Toyota Supra. Every
  series code iDrive knows is covered. Unverified.

## [0.1.6-test.1] - 2026-11-15 · Unverified

<!-- Flare Drive:
Announcements over Wi-Fi now turn the radio down instead of stopping it. On the cable, Android Auto's first-time prompt
on the phone can now be accepted, and the phone is told when Android Auto stops. A frozen picture now reconnects by
itself, and the volume bar no longer pops up for announcements.
-->

Announcements that lower the radio instead of stopping it, first-time setup over the cable, and fixes for a frozen
picture, the car's GPS speed, the volume bar and the cluster's arrows. Only the service changes. This version is
unverified.

### Added

- **Right-hand drive and diesel cars can say so.** A `side=right` line in the `vehicle` file tells Android Auto the
  driver sits on the right, and `fuel=diesel` tells Maps the car takes diesel. Unverified.

### Changed

- **Navigation announcements over Wi-Fi lower the radio instead of stopping it.** With sound over Wi-Fi on and Android
  Auto's music not playing, an announcement plays the way CarPlay's Maps does, on its own channel, so the car turns the
  radio down for it and back up after. Before, the first announcement took the radio's place. Unverified.

### Fixed

- **A frozen picture now ends the session.** When the encrypted stream from the phone broke, Android Auto froze on its
  last picture until the cable was pulled or the Wi-Fi dropped. After 16 frames in a row that can't be read, the
  session ends and the phone connects again. Unverified.
- **Maps no longer sees a petrol car that also charges at Tesla Superchargers.** Android Auto was told what Google's
  test head unit says. Without a `fuel=` line it is now told petrol only. Unverified.
- **The car's GPS speed stays inside what Android Auto accepts.** A speed of 70 m/s or more, which a speed worked out
  from two positions could reach, made Android Auto ignore the car's speed for the rest of the drive. Such a speed is
  no longer sent. Unverified.
- **No straight arrow at 0 in the cluster when a route starts.** Maps sends a short "depart" step at the start of a
  route and after each reroute. The cluster no longer draws it, and after a reroute the last arrow stays until the
  next turn. Unverified.
- **The next turn in the cluster no longer flashes up with a few metres to go.** Right after a turn, the next step
  can come before its own distance. The cluster now waits up to 3 s for that distance, so a turn that is still far off
  stays hidden. Unverified.
- **Roundabout exit numbers in the cluster from phones that send only the older turn message.** Unverified.
- **A phone on the cable that doesn't answer yet is given time.** Android Auto's first-time prompt on the phone could
  never be accepted over the cable, because the phone was reset and asked again every few seconds, which closed the
  prompt. Now the car waits for it, and after a few tries leaves the phone alone for two minutes, or until it's
  plugged in again. Unverified.
- **A phone on the cable is told when Android Auto stops.** It used to be left waiting on a connection nobody read.
  Now it ends its Android Auto session cleanly. Unverified.
- **The volume bar over Android Auto shows only the music volume.** A navigation prompt, a call or a chime could make
  it pop up with that sound's own level. Unverified.

## [0.1.5] - 2026-11-14 · Verified

<!-- Flare Drive:
Android Auto's music can now come over Wi-Fi and play on the car's speakers, with the volume knob, like CarPlay's. The
wireless picture is smoother, the cable gets a sharper one, and Bluetooth pairs and reconnects normally with the cable
in. It also waits for iDrive before it shows and stays connected when your phone goes quiet.
-->

Music over Wi-Fi on the car's speakers, a smoother wireless picture, a sharper one over the cable, and fixes for
Bluetooth, My BMW and the wait for iDrive. The service, the supervisor, the iDrive app and the installer change. This
version is verified.

### Added

- **Music over Wi-Fi plays on the car's speakers.** With sound over Wi-Fi switched on in Flare Drive, Android Auto's
  music and announcements come over Wi-Fi instead of Bluetooth, and the car plays them as it plays CarPlay's: Android
  Auto asks the car for CarPlay's music route before it plays, and the volume knob sets the level. Calls stay on
  Bluetooth. Verified.
- **Turn arrows on the X3 G01's instrument cluster.** Its cluster refuses the full route Android Auto sends, so no
  arrows appeared. Create `/var/local/data/AA/kombi-single-maneuver` over SSH and restart Android Auto, and only the
  next turn is sent. Cars without the file are unchanged. Unverified.
- **More detail in the service's log**, when logs are on: which Bluetooth device connects or disconnects (the phone
  or another device, never its address), why a Bluetooth link closed, the phone's longest silence every 10 seconds,
  and how many picture frames came close together. Verified.

### Changed

- **A sharper picture over the cable.** A phone on the cable draws Android Auto at 1920x1080 instead of 1280x720, and
  the head unit shrinks it onto the screen, so the map arrow, roads and text have smoother edges. Over Wi-Fi the
  picture stays at 1280x720, because a phone asked for the larger one there hung up straight away. To keep the cable
  at 1280x720 too, create `/var/local/data/AA/video-720` over SSH and restart Android Auto. Verified.
- **Android Auto installs on any iDrive software version.** The installer no longer stops on a version it hasn't
  been tried on. It still checks that the car has what Android Auto needs, and stops if it doesn't. Installing on any
  car is at your own risk. Unverified.

### Fixed

- **A smoother wireless picture.** Over Wi-Fi the phone's frames often arrive in pairs, and the screen showed only the
  second of each, so about a quarter of them were lost and the picture juddered. A frame that arrives right behind
  another now waits up to 25 ms, so both are shown. Frames that arrive evenly aren't held, so touch and the iDrive
  controller respond as before. Verified.
- **Android Auto leaves My BMW's Bluetooth slot alone.** It used to take over the slot the My BMW app connects to, so
  My BMW couldn't find the car while Android Auto ran. Android Auto now takes a spare slot, and falls back to the old
  one only on a car that doesn't have it. Verified.
- **Bluetooth pairs and reconnects with a phone on the cable.** Android Auto told the phone the car's Bluetooth
  address, which made the phone hide its own pairing code and wait for one from the car that never came, and stopped
  it reconnecting after its Bluetooth dropped. The phone now pairs through iDrive's new device screen with its own
  code, cable in or not. Verified.
- **Android Auto stays connected when your phone goes quiet.** Some phones stop sending anything while iDrive has the
  screen and no music plays, and the car ended the session 5 seconds after you pressed MENU or MAP. The car now checks
  on a quiet phone itself and ends the session only if the phone doesn't answer. Verified.
- **Android Auto waits for iDrive before it shows.** When your phone connected while one of iDrive's own windows was
  on top, such as right after unlocking the car, Android Auto came up, couldn't hold the screen and left after about
  4 seconds. It now waits until iDrive is ready, then comes up, and by itself after 15 seconds at the latest. No empty
  window is left over a call or the reversing camera meanwhile, and iDrive's own messages, such as Check Control
  warnings, still show over Android Auto. Verified.
- **iDrive's menu comes back if your phone leaves at the wrong moment.** If your phone disconnected just before
  Android Auto came up, iDrive could stay on a blank screen until you pressed a button. It now goes back to the
  Android Auto menu after 3 seconds. Unverified.
- **Wireless Android Auto no longer drops when My BMW connects.** When the car connects to the My BMW app, Android
  Auto's Bluetooth link to the phone drops with it, and the phone kept closing and reopening it every 12 seconds,
  sometimes ending the session. The car now answers that link with the Wi-Fi the phone is already on, and if the phone
  then starts a new connection, the car takes it instead of leaving you on Connecting. Unverified.
- **Android Auto starts again after it stops while your phone is reconnecting.** On stopping, Android Auto hands its
  Bluetooth slot back to the car. A phone trying to reconnect could get there first every time, so the handover never
  finished and Android Auto refused to start until the head unit restarted. It now waits for the slot to come free
  and tries again. Unverified.
- **MENU and MAP leave Android Auto straight after BACK.** Pressed within 2 seconds of BACK or a left tilt, they could
  leave you in Android Auto. Unverified.
- **The iDrive controller works on cars with the older controller, such as the i3.** It did nothing in Android Auto
  there, because one stray message from the car stopped Android Auto from trying the older controller's protocol.
  Verified on a G20 with the older controller.
- **The compatibility check looks at the right Bluetooth slot.** Before installing, it checked the slot My BMW uses,
  so it warned when My BMW was connected and missed a busy Android Auto slot. Unverified.

## [0.1.4] - 2026-11-13 · Verified

<!-- Flare Drive:
Connect Flare Drive by scanning a QR code in iDrive's Android Auto menu. Connecting over the USB cable is more
reliable, tilting the controller left no longer closes Android Auto, and several picture, microphone and Bluetooth
problems are fixed.
-->

Connect your phone with a QR code in iDrive, more reliable connecting over the USB cable, and fixes for the picture,
the microphone, Bluetooth, the instrument cluster and the controller. The service and the iDrive app change. This
version is verified.

### Added

- **Connect your phone by scanning a QR code in iDrive.** A new row in iDrive's Android Auto menu shows a QR code
  that joins your phone to the car's Wi-Fi and tells the phone app where to find the head unit. It never contains the
  root password. A PIN of 4 to 8 digits, chosen the first time and typed with the iDrive controller, protects it. The
  code is hidden while the car is moving. After five wrong PINs the page locks for a minute, and each wrong PIN after
  that doubles the wait, up to an hour; restarting doesn't reset the count. Forgot the PIN? Delete
  `/var/local/data/AA/flare-pin` over SSH and choose a new one. Verified.

### Fixed

- **Tilting the iDrive controller left no longer closes Android Auto.** iDrive also takes a left tilt, and the BACK
  button, as its own back, which could close Android Auto while you moved between its apps. Both now stay in Android
  Auto. Verified.
- **Over a USB cable, Android Auto no longer hangs on a blank screen while connecting.** If the phone stops answering
  partway through, Android Auto tries again by itself after 10 seconds. It used to wait until you unplugged the cable.
  Verified.
- **Over a USB cable, replugging the phone works straight away.** If the phone turned down the first request to start
  Android Auto, the car ignored it for 30 seconds, even after you unplugged it and plugged it in again. It now asks
  again after 2 seconds, and at once when you replug it. Verified.
- **Over a USB cable, a connection that fails to start no longer retries every second.** It now waits 10 seconds
  before asking the phone again. Verified.
- **The picture recovers by itself if the car's video decoder stops.** It used to stay frozen until you stopped
  Android Auto. It now comes back within a few seconds. Verified.
- **The picture no longer stays smeared after a lost video frame.** Android Auto now asks the phone for a fresh
  picture. Verified.
- **The Assistant's microphone no longer gets stuck.** If the car's microphone stopped sending sound, stopping
  Android Auto could hang, and the microphone could restart over and over while the Assistant listened. It now gives
  up after a few tries and waits for the phone to ask again. Verified.
- **A slow answer from the car's Bluetooth no longer stalls Android Auto.** It could hold up everything the phone
  sent for up to 25 seconds, and drop the connection. Verified.
- **No more garbled road names or distances on the instrument cluster.** Odd text or numbers from the phone are
  cleaned up before they reach it. Verified.

## [0.1.3] - 2026-11-12 · Verified

<!-- Flare Drive:
Android Auto stays on screen when you change driving mode. Touches and the COM button no longer get stuck, and the picture no longer freezes after a video glitch.
-->

Android Auto stays on screen when you change driving mode, and several fixes for touches, the COM button, the picture
and BMW's USB music. The service, the supervisor and the iDrive app change. This version is verified.

### Changed

- **Android Auto stays on screen when you change driving mode.** Switching between Comfort, Sport and the other modes
  used to hide it for a few seconds. It now only steps aside if iDrive is set to show driving mode changes. Verified.

### Fixed

- **Touches no longer get stuck.** If you lifted your finger just as something else took the screen, such as the
  volume bar, a popup, reverse gear or MENU, the phone could think you were still touching it. Unverified.
- **The COM button no longer gets stuck** when a call comes in while you're holding it. Unverified.
- **Android Auto no longer steps aside for no reason.** It should only do that when you press a button such as MENU.
  Unverified.
- **BMW's USB music and apps work with Android phones again after Android Auto crashes.** They could stay blocked
  until the head unit restarted. Unverified.
- **iDrive no longer says "Connecting..." while Android Auto is stopping.** Unverified.
- **The picture no longer freezes after a glitch in the video.** A few damaged video frames could freeze it for the
  rest of the drive. Unverified.

## [0.1.2] - 2026-11-12 · Verified

<!-- Flare Drive:
Over a USB cable, Android Auto reconnects by itself if it drops right after starting. When wireless is off, its entry in iDrive's Media menu tells you to plug in your phone.
-->

Two improvements for Android Auto over a USB cable. The service and the iDrive app change. This version is verified.

### Changed

- **iDrive tells you to plug in your phone.** With wireless off and no phone on the cable, Android Auto's entry in
  iDrive's Media menu says to plug it in, instead of "Connecting...". Verified.

### Fixed

- **Android Auto over a USB cable reconnects by itself.** If it dropped just after the Android Auto logo appeared on
  the phone, it could stay stuck until you stopped it. It now tries again within a few seconds. Verified.

## [0.1.1] - 2026-11-12 · Verified

<!-- Flare Drive:
Android Auto's entry in the iDrive menu now shows the colour Android Auto logo.
-->

The colour Android Auto logo in iDrive. Only the iDrive app's images (`AAEntryNav_ALL_Images.zip`) change. This version
is verified.

### Changed

- **The colour Android Auto logo in iDrive.** Android Auto's entry in the iDrive menu, and the Start Android Auto
  button on its page, show the colour Android Auto logo instead of a white outline. Verified.

## [0.1.0] - 2026-11-12 · Verified

<!-- Flare Drive:
Action needed: restart the head unit after installing, or Android Auto will not start. Android Auto starts every time, and it no longer appears over the reversing camera.
-->

Action needed: restart the head unit after installing. Android Auto won't start until it has.

The first release of AA-EntryNav: Android Auto starts every time and keeps clear of the reversing camera.

### Fixed

- **Android Auto starts every time.** Another program on the head unit could take the connection Android Auto uses at
  start-up, and Android Auto then wouldn't start for the whole trip. This was seen on another car with the same
  software. It now uses a private connection no other program can take.
- **Android Auto no longer appears over the reversing camera.** It could if the camera came on just before Android
  Auto stepped aside, or if you selected reverse within 3 seconds of opening Android Auto.
- **Changing the car's clock no longer upsets Android Auto's timing.** When GPS corrected the clock while driving, a
  wait could end early or drag on. Android Auto now times things from when the head unit started.

[Unreleased]: https://github.com/CallMeLewis/AA-EntryNav/compare/v0.1.6-test.1...HEAD
[0.1.6-test.1]: https://github.com/CallMeLewis/AA-EntryNav/tree/e2bde66ac3b3ba7c417f7a85784790d7c6bf2716
[0.1.5]: https://github.com/CallMeLewis/AA-EntryNav/tree/2b475f0c2594c6b7b4849ddca0093c84648b9385
[0.1.4]: https://github.com/CallMeLewis/AA-EntryNav/tree/6d271b999ed2f2d4d23ee61f7d7cbfa5cda2a43b
[0.1.3]: https://github.com/CallMeLewis/AA-EntryNav/tree/e3c28334465d6d86722a260685ef76a23a1a5f67
[0.1.2]: https://github.com/CallMeLewis/AA-EntryNav/tree/52fa9b73ff0f92cd267796a14ddc6d2232a0140a
[0.1.1]: https://github.com/CallMeLewis/AA-EntryNav/tree/f6cb9794f800dab7a6c406ac31579f0b373aa277
[0.1.0]: https://github.com/CallMeLewis/AA-EntryNav/tree/19387f1d350ca802f3bd4d6e1b0804f362ea2591
