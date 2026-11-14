# Changelog

All notable changes to this project. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow [SemVer](https://semver.org/).

Each version says whether it is verified or unverified. Verified means this exact build has run in a car. Unverified
means it has only been tested on PC: the PC tests passed and the ARM programs ran under QEMU with the car's libraries.
Each version links to the commit its archive was built from.

## [Unreleased]

## [0.1.5-test.7] - 2026-11-14 · Unverified

<!-- Flare Drive:
Sound over Wi-Fi now plays on the car's speakers, with the volume knob, and Bluetooth only carries calls. With a phone
on the cable, Bluetooth pairs and reconnects normally again.
-->

Music over Wi-Fi, and Bluetooth fixes for the cable, to try before 0.1.5. Turn on sound over Wi-Fi in Flare Drive to
hear Android Auto through the car as CarPlay plays. Only the service changes. This version is unverified.

### Added

- **Music over Wi-Fi plays on the car's speakers.** With sound over Wi-Fi switched on in Flare Drive, Android Auto's
  music and announcements come over Wi-Fi instead of Bluetooth, and the car plays them as it plays CarPlay's: Android
  Auto asks the car for CarPlay's music route before it plays, and the volume knob sets the level. Before, the switch
  left you with no sound. Bluetooth no longer streams music while it's on, which may also help the wireless picture.
  Calls stay on Bluetooth. Unverified.

### Fixed

- **Bluetooth pairs and reconnects with a phone on the cable.** Android Auto told the phone the car's Bluetooth
  address, which made the phone hide its own pairing code and wait for one from the car that never came, and stopped
  it reconnecting after its Bluetooth dropped. The phone now pairs through iDrive's new device screen with its own code,
  and reconnects by itself, as it does without the cable. Unverified.

## [0.1.5-test.6] - 2026-11-14 · Unverified

<!-- Flare Drive:
Wireless Android Auto reconnects after a quick ignition cycle, instead of staying on Connecting. Android Auto also
starts again if it was stopped while your phone was reconnecting.
-->

Two fixes for reconnecting, to try before 0.1.5. Android Auto reconnects after a quick ignition cycle, and starts again
after stopping while your phone was reconnecting. The service and the supervisor change. This version is unverified.

### Fixed

- **Android Auto reconnects after a quick ignition cycle.** When your phone's Bluetooth came back during a session,
  the car sent it the Wi-Fi details again. The phone then dropped its session to start a new one, and the car turned
  the new one away, so iDrive stayed on Connecting. The car now ends the old session and takes the new one. Unverified.
- **Android Auto starts again after it stops while your phone is reconnecting.** On stopping, Android Auto hands its
  Bluetooth slot back to the car. A phone trying to reconnect got there first every time, so the handover never
  finished and Android Auto refused to start until the head unit restarted. It now waits for the slot to come free and
  tries again. Unverified.

## [0.1.5-test.5] - 2026-11-14 · Unverified

<!-- Flare Drive:
A smoother wireless picture, without a slower response to touch. Wireless Android Auto also no longer drops when the
car connects to the My BMW app.
-->

Two fixes for wireless Android Auto, to try before 0.1.5: a smoother picture, and no more drops when the car connects
to My BMW. Only the service changes. This version is unverified.

### Fixed

- **A smoother wireless picture.** Over Wi-Fi the phone's frames often arrive in pairs, and the screen showed only the
  second of each, so about a quarter of them were lost and the picture judders. A frame that arrives right behind
  another now waits up to 25 ms, so both are shown. Frames that arrive evenly aren't held, so touch and the iDrive
  controller respond as before. Unverified.
- **Wireless Android Auto no longer drops when My BMW connects.** When the car connects to the My BMW app, Android
  Auto's Bluetooth link to the phone drops with it. The phone opened it again and waited for the car, which said
  nothing, so it closed it every 12 seconds, and sometimes the whole session ended. The car now answers that link with
  the Wi-Fi the phone is already on. Unverified.

## [0.1.5-test.4] - 2026-11-14 · Unverified

<!-- Flare Drive:
Wireless Android Auto connects again. Since 0.1.5-test.1 a phone on Wi-Fi hung up a few seconds after connecting, and
iDrive kept saying Connecting. The sharper picture now stays on the cable.
-->

A fix for wireless Android Auto, to try before 0.1.5. A phone on Wi-Fi gets the 1280x720 picture again, and the
1920x1080 one stays on the cable. The service and the installer change. This version is unverified.

### Changed

- **Android Auto installs on any iDrive software version.** The installer no longer stops on a version it hasn't
  been tried on. It still checks that the car has what Android Auto needs, and stops if it doesn't. Installing on any
  car is at your own risk. Unverified.

### Fixed

- **Wireless Android Auto connects again.** Since 0.1.5-test.1, a phone on Wi-Fi connected, then hung up within a few
  seconds without showing a picture, and iDrive kept saying Connecting. The sharper 1920x1080 picture now stays on the
  cable, and a phone on Wi-Fi gets the 1280x720 picture it had before. Unverified.

## [0.1.5-test.3] - 2026-11-13 · Unverified

<!-- Flare Drive:
Fixes for the wait for iDrive in the last test. Android Auto can no longer stay hidden for a whole drive, and no empty
window is left over a call or the reversing camera.
-->

Fixes for the last test's wait for iDrive, to try before 0.1.5. The service, the supervisor and the iDrive app change.
This version is unverified.

### Fixed

- **Android Auto no longer stays hidden.** If the iDrive app never told it to come up, Android Auto could stay hidden
  for the rest of the drive while your phone stayed connected. It now comes up by itself after 15 seconds at the
  latest. Time on a call or in reverse doesn't count. Unverified.
- **No empty window over a call or the reversing camera.** If a call came in or you selected reverse while Android
  Auto was waiting for iDrive, an empty window could stay over iDrive's call screen or the camera and take the
  controller until the call or reverse ended. It now closes at once. Unverified.
- **iDrive's menu comes back if your phone leaves at the wrong moment.** If your phone disconnected just before
  Android Auto came up, iDrive could stay on a blank screen until you pressed a button. It now goes back to the
  Android Auto menu after 3 seconds. Unverified.
- **iDrive's messages show over Android Auto again.** After a session that ended before Android Auto came up, iDrive's
  own messages, such as Check Control warnings, could stay hidden behind Android Auto in the next session. Unverified.
- **The compatibility check looks at the right Bluetooth slot.** Before installing, it checked the slot My BMW uses,
  so it warned when My BMW was connected and missed a busy Android Auto slot. Unverified.

## [0.1.5-test.2] - 2026-11-13 · Unverified

<!-- Flare Drive:
A test of fixes from the original project. Android Auto no longer takes the My BMW app's Bluetooth slot, which may
make wireless faster, and it stays connected when your phone goes quiet. It also waits for iDrive before it shows, and
MENU leaves it straight after BACK.
-->

A test of fixes from the original project, to try before 0.1.5: Android Auto leaves My BMW's Bluetooth slot alone,
keeps a quiet phone connected and waits for iDrive before it shows. The service, the supervisor and the iDrive app
change. This version is unverified.

### Added

- **Turn arrows on the X3 G01's instrument cluster.** Its cluster refuses the full route Android Auto sends, so no
  arrows appeared. Create `/var/local/data/AA/kombi-single-maneuver` over SSH and restart Android Auto, and only the
  next turn is sent. Cars without the file are unchanged. Unverified.
- **More detail in the service's log**, when logs are on: which Bluetooth device connects or disconnects (the phone
  or another device, never its address), why a Bluetooth link closed, and the phone's longest silence every 10
  seconds of a session. Unverified.

### Fixed

- **Android Auto leaves My BMW's Bluetooth slot alone.** It used to take over the slot the My BMW app connects to.
  While it ran, My BMW couldn't find the car, and BMW's own service treated Android Auto's Bluetooth link as its own,
  which may be what made wireless Android Auto slow for some drivers. Android Auto now takes a spare slot, and falls
  back to the old one only on a car that doesn't have it. Unverified.
- **Android Auto stays connected when your phone goes quiet.** Some phones stop sending anything while iDrive has the
  screen and no music plays, and the car ended the session 5 seconds after you pressed MENU or MAP. The car now checks
  on a quiet phone itself and ends the session only if the phone doesn't answer. Unverified.
- **Android Auto waits for iDrive before it shows.** When your phone connected while one of iDrive's own windows was
  on top, such as right after unlocking the car, Android Auto came up, couldn't hold the screen and left after about
  4 seconds. It now waits until iDrive is ready, then comes up. Unverified.
- **MENU and MAP leave Android Auto straight after BACK.** Pressed within 2 seconds of BACK or a left tilt, they could
  leave you in Android Auto. Unverified.
- **The iDrive controller works on cars with the older controller, such as the i3.** It did nothing in Android Auto
  there, because one stray message from the car stopped Android Auto from trying the older controller's protocol.
  Unverified.

## [0.1.5-test.1] - 2026-11-13 · Verified

<!-- Flare Drive:
A test of a sharper Android Auto picture. Your phone now draws it at 1920x1080 and the head unit shrinks it onto the
screen, so the map arrow, roads and text have smoother edges. If the head unit can't play the larger picture, it goes
back to the old one by itself.
-->

A sharper Android Auto picture, to try before 0.1.5. Only the service changes. This version is verified.

### Changed

- **A sharper Android Auto picture.** Your phone now draws Android Auto at 1920x1080 instead of 1280x720, and the
  head unit shrinks it onto the screen. The map arrow, roads, icons and text have smoother edges, and thin coloured
  lines lose the fringe around them. Buttons and text keep their size. If the head unit can't decode the larger
  picture, Android Auto goes back to the old one by itself. To go back by hand, create `/var/local/data/AA/video-720`
  over SSH and restart Android Auto. Verified.

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

## [0.1.1] - 2026-11-12 · Unverified

<!-- Flare Drive:
Android Auto's entry in the iDrive menu now shows the colour Android Auto logo.
-->

The colour Android Auto logo in iDrive. Only the iDrive app's images (`AAEntryNav_ALL_Images.zip`) change. This version
is unverified.

### Changed

- **The colour Android Auto logo in iDrive.** Android Auto's entry in the iDrive menu, and the Start Android Auto
  button on its page, show the colour Android Auto logo instead of a white outline. Unverified.

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

[Unreleased]: https://github.com/CallMeLewis/AA-EntryNav/compare/v0.1.5-test.7...HEAD
[0.1.5-test.6]: https://github.com/CallMeLewis/AA-EntryNav/tree/28ea2fa85eec44b439ace26c9bff01fdd0d26ab5
[0.1.5-test.5]: https://github.com/CallMeLewis/AA-EntryNav/tree/44dd960247e64cc8729d9613e594f2f29c092f1a
[0.1.5-test.4]: https://github.com/CallMeLewis/AA-EntryNav/tree/9ffd01a68f190c4d024332e91474af6f195378dc
[0.1.5-test.3]: https://github.com/CallMeLewis/AA-EntryNav/tree/1e5dd83343c90c716cc8d24d1b0396bac86ced87
[0.1.5-test.2]: https://github.com/CallMeLewis/AA-EntryNav/tree/aea35095080027157d7f2f218fd21e341f48cf71
[0.1.5-test.1]: https://github.com/CallMeLewis/AA-EntryNav/tree/f6405a09a8a38f02b0d4c968b6df5acbbf03c376
[0.1.4]: https://github.com/CallMeLewis/AA-EntryNav/tree/6d271b999ed2f2d4d23ee61f7d7cbfa5cda2a43b
[0.1.3]: https://github.com/CallMeLewis/AA-EntryNav/tree/e3c28334465d6d86722a260685ef76a23a1a5f67
[0.1.2]: https://github.com/CallMeLewis/AA-EntryNav/tree/52fa9b73ff0f92cd267796a14ddc6d2232a0140a
[0.1.1]: https://github.com/CallMeLewis/AA-EntryNav/tree/f6cb9794f800dab7a6c406ac31579f0b373aa277
[0.1.0]: https://github.com/CallMeLewis/AA-EntryNav/tree/19387f1d350ca802f3bd4d6e1b0804f362ea2591
