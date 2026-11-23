# Changelog

All notable changes to this project. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow [SemVer](https://semver.org/).

Each version links to the commit or tag its archive was built from.

## [Unreleased]

<!-- Flare Drive:
With Sound through Android Auto, turning the car off and on no longer leaves Android Auto silent. The sound comes back
by itself without reconnecting your phone.
-->

Sound through Android Auto now comes back after the car is turned off and on. The service and the iDrive app change.

### Fixed

- **Sound through Android Auto comes back after you turn the car off and on.** Android Auto could stay on screen with
  no sound until you disconnected and reconnected your phone. The sound now comes back by itself, and a radio you pick
  in iDrive keeps playing.

## [0.1.8] - 2026-11-23

<!-- Flare Drive:
With Sound through Android Auto, the instrument cluster and Media/Radio tile now show the song and album art, music
plays through short pauses, and the steering-wheel roller skips tracks. Phones on Wi-Fi reconnect after a drop, Android
Auto waits for the reverse camera to close, the volume pill stays closed until you change it, and a broken picture
mends sooner.
-->

Music played through Android Auto now shows the song and album art in the cluster and on the Media/Radio tile, keeps
playing through short pauses, and skips tracks with the steering-wheel roller. Phones on Wi-Fi reconnect after a drop,
Android Auto waits for the reverse camera to close, the volume pill stays closed until you change the volume, and a
broken picture mends sooner. The service and the iDrive app change.

### Added

- **The instrument cluster and iDrive's Media/Radio tile show the current song and album art while music plays through
  Android Auto.** With Sound through Android Auto turned on in Flare Drive (or the `audio-wifi` file in place), the
  cluster shows its media menu with the song title instead of "no active entertainment source", and iDrive's main menu
  Media/Radio tile shows the song, artist, album and album art instead of "Unknown artist / Unknown track". This starts
  from the next head-unit restart after turning Sound through Android Auto on.

### Changed

- **With Sound through Android Auto, a short pause on the Wi-Fi or the cable no longer cuts the music.** About 0.4 s of
  music now waits in the car, where 0.1 s did, and the phone sends it at the speed the car plays, so a phone whose
  clock runs a little fast or slow no longer skips or leaves a gap. Pause, play and a new track are heard about 0.3 s
  later. With logging on, the service log has a line for each gap as it happens (`media ran dry`).

### Fixed

- **The steering-wheel roller changes tracks while Android Auto's sound plays through the car.** Roll down and push for
  the next track, or roll up and push for the previous one, whether Android Auto or iDrive is on screen. Once you roll
  in either direction, each push skips another track without rolling again (after a head-unit restart with Sound
  through Android Auto on; before that, roll again before each push). During a call, and with Bluetooth audio, the
  roller stays iDrive's as before.
- **A phone on Wi-Fi whose connection drops starts Android Auto again by itself.** Some phones kept their Bluetooth
  link to the car after the Wi-Fi went quiet or the connection broke, and never asked again, so iDrive stayed on
  Connecting until Android Auto was stopped and started. The car now drops that link and the phone connects again.
  Leaving Android Auto on the phone still ends it as before.
- **Android Auto waits for the reverse camera to close.** The gear can leave reverse a moment before iDrive closes the
  camera, and Android Auto came back over a camera still on screen. It now comes back once iDrive leaves the camera,
  when you pick Android Auto yourself, or after 15 seconds at most.
- **The volume pill no longer opens by itself.** It opens when you change the volume. Some cars sent another source's
  volume, or the same level again, as the phone connected, and the pill opened with nothing pressed.
- **A picture broken by a lost video frame mends sooner.** When the car loses a frame, the phone is now asked to
  restart its picture from a whole new frame, at most once every 10 seconds, instead of waiting for its next one.

## [0.1.7] - 2026-11-18

<!-- Flare Drive:
With sound over Wi-Fi, the MEDIA button now brings Android Auto back on its media app. A phone on the cable gets a
few seconds to finish its old session before it reconnects.
-->

MEDIA brings Android Auto back while its sound plays over Wi-Fi, and phones on the cable reconnect more calmly. The
service and the iDrive app change.

### Changed

- **The wheel diagnostic tool now records the roller too.** `evo-mfl` reads `RotarySteps` (0xC30) and `RotaryPress`
  (0xC31) on ControlElementsMFL 0x15.02, alongside its existing button reads. These are plain Get requests, with no
  settings changed. Its output names each function. This prepares diagnosis of the cluster's "no entertainment
  source selected" message with Wi-Fi audio; it does not fix the wheel controls yet.

### Fixed

- **MEDIA opens Android Auto while its sound plays over Wi-Fi.** With sound over Wi-Fi, pressing MEDIA on the iDrive
  controller from an iDrive screen did nothing, because iDrive took the music for CarPlay's and asked CarPlay to open.
  Now Android Auto comes back on its media app, as MEDIA already does while Android Auto is on screen. With the radio
  or Bluetooth playing, MEDIA is iDrive's as before. The steering-wheel roller still shows an empty list in the
  cluster.
- **A phone on the cable gets time to finish its old session before reconnecting.** After a session ends, the same
  phone waits 5 seconds before another attempt. If the session could not start, it waits 10 seconds. During either
  pause, unplugging and plugging it in again is noticed, and a phone can connect over Wi-Fi. Before, a session that
  started and then ended could be retried immediately, while a failed start stopped USB polling for 10 seconds.

## [0.1.6] - 2026-11-17

<!-- Flare Drive:
Announcements over Wi-Fi now turn the radio down instead of stopping it. Your phone can show the car as "1 Series
(F20)", and Fill in from the car in Flare Drive 2.1.6 sets its details for you. On the cable, Android Auto's
first-time prompt on the phone can now be accepted, and a frozen picture now reconnects by itself.
-->

Announcements that lower the radio instead of stopping it, the car's series and details on the phone, first-time
setup over the cable, and fixes for a frozen picture, the car's GPS speed, the volume pill and the cluster's arrows.
The service and the iDrive app change.

### Added

- **The phone can show the car's series.** Set it under Vehicle in Flare Drive 2.1.5, or with a `series=F20` line in
  the `vehicle` file, and the phone shows "1 Series (F20)", or "M140i (F20)" when the model is M140i. MINIs,
  Rolls-Royces and the Toyota Supra show their own make. Every series code iDrive knows is covered.
- **Right-hand drive and diesel cars can say so.** Set them under Vehicle in Flare Drive, or with a `side=right` or
  `fuel=diesel` line in the `vehicle` file. Android Auto then knows the driver sits on the right, and Maps looks for
  diesel at fuel stops.
- **Flare Drive can fill in the car's details for you.** The iDrive app reads the car's series and driver's side from
  iDrive and gives the car an ID made from its VIN, so a phone used in two cars keeps them apart. Fill in from the
  car, under Vehicle in Flare Drive 2.1.6, puts them in. Nothing the phone is told changes until you choose. The fuel
  isn't read, because iDrive's fuel setting is the one its navigation uses to find filling stations, often "all
  fuels".

### Changed

- **Navigation announcements over Wi-Fi lower the radio instead of stopping it.** With sound over Wi-Fi on and Android
  Auto's music not playing, an announcement plays the way CarPlay's Maps does, on its own channel, so the car turns the
  radio down for it and back up after. Before, the first announcement took the radio's place.

### Fixed

- **A frozen picture now ends the session.** When the encrypted stream from the phone broke, Android Auto froze on its
  last picture until the cable was pulled or the Wi-Fi dropped. After 16 frames in a row that can't be read, the
  session ends and the phone connects again.
- **Maps no longer sees a petrol car that also charges at Tesla Superchargers.** Android Auto was told what Google's
  test head unit says. Without a fuel set it is now told petrol only.
- **The car's GPS speed stays inside what Android Auto accepts.** A speed of 70 m/s or more, which a speed worked out
  from two positions could reach, made Android Auto ignore the car's speed for the rest of the drive. Such a speed is
  no longer sent.
- **No straight arrow at 0 in the cluster when a route starts.** Maps sends a short "depart" step at the start of a
  route and after each reroute. The cluster no longer draws it, and after a reroute the last arrow stays until the
  next turn.
- **The next turn in the cluster no longer flashes up with a few metres to go.** Right after a turn, the next step
  can come before its own distance. The cluster now waits up to 3 s for that distance, so a turn that is still far off
  stays hidden.
- **Roundabout exit numbers in the cluster from phones that send only the older turn message.**
- **A phone on the cable that doesn't answer yet is given time.** Android Auto's first-time prompt on the phone could
  never be accepted over the cable, because the phone was reset and asked again every few seconds, which closed the
  prompt. Now the car waits for it, and after a few tries leaves the phone alone for two minutes, or until it's
  plugged in again.
- **A phone on the cable is told when Android Auto stops.** It used to be left waiting on a connection nobody read.
  Now it ends its Android Auto session cleanly.
- **The volume pill over Android Auto shows only the music volume.** A navigation prompt, a call or a chime could make
  it pop up with that sound's own level.

## [0.1.5] - 2026-11-14

<!-- Flare Drive:
Android Auto's music can now come over Wi-Fi and play on the car's speakers, with the volume knob, like CarPlay's. The
wireless picture is smoother, the cable gets a sharper one, and Bluetooth pairs and reconnects normally with the cable
in. It also waits for iDrive before it shows and stays connected when your phone goes quiet.
-->

Music over Wi-Fi on the car's speakers, a smoother wireless picture, a sharper one over the cable, and fixes for
Bluetooth, My BMW and the wait for iDrive. The service, the supervisor, the iDrive app and the installer change.

### Added

- **Music over Wi-Fi plays on the car's speakers.** With sound over Wi-Fi switched on in Flare Drive, Android Auto's
  music and announcements come over Wi-Fi instead of Bluetooth, and the car plays them as it plays CarPlay's: Android
  Auto asks the car for CarPlay's music route before it plays, and the volume knob sets the level. Calls stay on
  Bluetooth.
- **Turn arrows on the X3 G01's instrument cluster.** Its cluster refuses the full route Android Auto sends, so no
  arrows appeared. Create `/var/local/data/AA/kombi-single-maneuver` over SSH and restart Android Auto, and only the
  next turn is sent. Cars without the file are unchanged.
- **More detail in the service's log**, when logs are on: which Bluetooth device connects or disconnects (the phone
  or another device, never its address), why a Bluetooth link closed, the phone's longest silence every 10 seconds,
  and how many picture frames came close together.

### Changed

- **A sharper picture over the cable.** A phone on the cable draws Android Auto at 1920x1080 instead of 1280x720, and
  the head unit shrinks it onto the screen, so the map arrow, roads and text have smoother edges. Over Wi-Fi the
  picture stays at 1280x720, because a phone asked for the larger one there hung up straight away. To keep the cable
  at 1280x720 too, create `/var/local/data/AA/video-720` over SSH and restart Android Auto.
- **Android Auto installs on any iDrive software version.** The installer no longer stops on a version it hasn't
  been tried on. It still checks that the car has what Android Auto needs, and stops if it doesn't. Installing on any
  car is at your own risk.

### Fixed

- **A smoother wireless picture.** Over Wi-Fi the phone's frames often arrive in pairs, and the screen showed only the
  second of each, so about a quarter of them were lost and the picture juddered. A frame that arrives right behind
  another now waits up to 25 ms, so both are shown. Frames that arrive evenly aren't held, so touch and the iDrive
  controller respond as before.
- **Android Auto leaves My BMW's Bluetooth slot alone.** It used to take over the slot the My BMW app connects to, so
  My BMW couldn't find the car while Android Auto ran. Android Auto now takes a spare slot, and falls back to the old
  one only on a car that doesn't have it.
- **Bluetooth pairs and reconnects with a phone on the cable.** Android Auto told the phone the car's Bluetooth
  address, which made the phone hide its own pairing code and wait for one from the car that never came, and stopped
  it reconnecting after its Bluetooth dropped. The phone now pairs through iDrive's new device screen with its own
  code, cable in or not.
- **Android Auto stays connected when your phone goes quiet.** Some phones stop sending anything while iDrive has the
  screen and no music plays, and the car ended the session 5 seconds after you pressed MENU or MAP. The car now checks
  on a quiet phone itself and ends the session only if the phone doesn't answer.
- **Android Auto waits for iDrive before it shows.** When your phone connected while one of iDrive's own windows was
  on top, such as right after unlocking the car, Android Auto came up, couldn't hold the screen and left after about
  4 seconds. It now waits until iDrive is ready, then comes up, and by itself after 15 seconds at the latest. No empty
  window is left over a call or the reversing camera meanwhile, and iDrive's own messages, such as Check Control
  warnings, still show over Android Auto.
- **iDrive's menu comes back if your phone leaves at the wrong moment.** If your phone disconnected just before
  Android Auto came up, iDrive could stay on a blank screen until you pressed a button. It now goes back to the
  Android Auto menu after 3 seconds.
- **Wireless Android Auto no longer drops when My BMW connects.** When the car connects to the My BMW app, Android
  Auto's Bluetooth link to the phone drops with it, and the phone kept closing and reopening it every 12 seconds,
  sometimes ending the session. The car now answers that link with the Wi-Fi the phone is already on, and if the phone
  then starts a new connection, the car takes it instead of leaving you on Connecting.
- **Android Auto starts again after it stops while your phone is reconnecting.** On stopping, Android Auto hands its
  Bluetooth slot back to the car. A phone trying to reconnect could get there first every time, so the handover never
  finished and Android Auto refused to start until the head unit restarted. It now waits for the slot to come free
  and tries again.
- **MENU and MAP leave Android Auto straight after BACK.** Pressed within 2 seconds of BACK or a left tilt, they could
  leave you in Android Auto.
- **The iDrive controller works on cars with the older controller, such as the i3.** It did nothing in Android Auto
  there, because one stray message from the car stopped Android Auto from trying the older controller's protocol.
- **The compatibility check looks at the right Bluetooth slot.** Before installing, it checked the slot My BMW uses,
  so it warned when My BMW was connected and missed a busy Android Auto slot.

## [0.1.4] - 2026-11-13

<!-- Flare Drive:
Connect Flare Drive by scanning a QR code in iDrive's Android Auto menu. Connecting over the USB cable is more
reliable, tilting the controller left no longer closes Android Auto, and several picture, microphone and Bluetooth
problems are fixed.
-->

Connect your phone with a QR code in iDrive, more reliable connecting over the USB cable, and fixes for the picture,
the microphone, Bluetooth, the instrument cluster and the controller. The service and the iDrive app change.

### Added

- **Connect your phone by scanning a QR code in iDrive.** A new row in iDrive's Android Auto menu shows a QR code
  that joins your phone to the car's Wi-Fi and tells the phone app where to find the head unit. It never contains the
  root password. A PIN of 4 to 8 digits, chosen the first time and typed with the iDrive controller, protects it. The
  code is hidden while the car is moving. After five wrong PINs the page locks for a minute, and each wrong PIN after
  that doubles the wait, up to an hour; restarting doesn't reset the count. Forgot the PIN? Delete
  `/var/local/data/AA/flare-pin` over SSH and choose a new one.

### Fixed

- **Tilting the iDrive controller left no longer closes Android Auto.** iDrive also takes a left tilt, and the BACK
  button, as its own back, which could close Android Auto while you moved between its apps. Both now stay in Android
  Auto.
- **Over a USB cable, Android Auto no longer hangs on a blank screen while connecting.** If the phone stops answering
  partway through, Android Auto tries again by itself after 10 seconds. It used to wait until you unplugged the cable.
- **Over a USB cable, replugging the phone works straight away.** If the phone turned down the first request to start
  Android Auto, the car ignored it for 30 seconds, even after you unplugged it and plugged it in again. It now asks
  again after 2 seconds, and at once when you replug it.
- **Over a USB cable, a connection that fails to start no longer retries every second.** It now waits 10 seconds
  before asking the phone again.
- **The picture recovers by itself if the car's video decoder stops.** It used to stay frozen until you stopped
  Android Auto. It now comes back within a few seconds.
- **The picture no longer stays smeared after a lost video frame.** Android Auto now asks the phone for a fresh
  picture.
- **The Assistant's microphone no longer gets stuck.** If the car's microphone stopped sending sound, stopping
  Android Auto could hang, and the microphone could restart over and over while the Assistant listened. It now gives
  up after a few tries and waits for the phone to ask again.
- **A slow answer from the car's Bluetooth no longer stalls Android Auto.** It could hold up everything the phone
  sent for up to 25 seconds, and drop the connection.
- **No more garbled road names or distances on the instrument cluster.** Odd text or numbers from the phone are
  cleaned up before they reach it.

## [0.1.3] - 2026-11-12

<!-- Flare Drive:
Android Auto stays on screen when you change driving mode. Touches and the COM button no longer get stuck, and the picture no longer freezes after a video glitch.
-->

Android Auto stays on screen when you change driving mode, and several fixes for touches, the COM button, the picture
and BMW's USB music. The service, the supervisor and the iDrive app change.

### Changed

- **Android Auto stays on screen when you change driving mode.** Switching between Comfort, Sport and the other modes
  used to hide it for a few seconds. It now only steps aside if iDrive is set to show driving mode changes.

### Fixed

- **Touches no longer get stuck.** If you lifted your finger just as something else took the screen, such as the
  volume bar, a popup, reverse gear or MENU, the phone could think you were still touching it.
- **The COM button no longer gets stuck** when a call comes in while you're holding it.
- **Android Auto no longer steps aside for no reason.** It should only do that when you press a button such as MENU.
- **BMW's USB music and apps work with Android phones again after Android Auto crashes.** They could stay blocked
  until the head unit restarted.
- **iDrive no longer says "Connecting..." while Android Auto is stopping.**
- **The picture no longer freezes after a glitch in the video.** A few damaged video frames could freeze it for the
  rest of the drive.

## [0.1.2] - 2026-11-12

<!-- Flare Drive:
Over a USB cable, Android Auto reconnects by itself if it drops right after starting. When wireless is off, its entry in iDrive's Media menu tells you to plug in your phone.
-->

Two improvements for Android Auto over a USB cable. The service and the iDrive app change.

### Changed

- **iDrive tells you to plug in your phone.** With wireless off and no phone on the cable, Android Auto's entry in
  iDrive's Media menu says to plug it in, instead of "Connecting...".

### Fixed

- **Android Auto over a USB cable reconnects by itself.** If it dropped just after the Android Auto logo appeared on
  the phone, it could stay stuck until you stopped it. It now tries again within a few seconds.

## [0.1.1] - 2026-11-12

<!-- Flare Drive:
Android Auto's entry in the iDrive menu now shows the colour Android Auto logo.
-->

The colour Android Auto logo in iDrive. Only the iDrive app's images (`AAEntryNav_ALL_Images.zip`) change.

### Changed

- **The colour Android Auto logo in iDrive.** Android Auto's entry in the iDrive menu, and the Start Android Auto
  button on its page, show the colour Android Auto logo instead of a white outline.

## [0.1.0] - 2026-11-12

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

[Unreleased]: https://github.com/CallMeLewis/AA-EntryNav/compare/v0.1.8...HEAD
[0.1.8]: https://github.com/CallMeLewis/AA-EntryNav/tree/v0.1.8
[0.1.7]: https://github.com/CallMeLewis/AA-EntryNav/tree/v0.1.7
[0.1.6]: https://github.com/CallMeLewis/AA-EntryNav/tree/v0.1.6
[0.1.5]: https://github.com/CallMeLewis/AA-EntryNav/tree/2b475f0c2594c6b7b4849ddca0093c84648b9385
[0.1.4]: https://github.com/CallMeLewis/AA-EntryNav/tree/6d271b999ed2f2d4d23ee61f7d7cbfa5cda2a43b
[0.1.3]: https://github.com/CallMeLewis/AA-EntryNav/tree/e3c28334465d6d86722a260685ef76a23a1a5f67
[0.1.2]: https://github.com/CallMeLewis/AA-EntryNav/tree/52fa9b73ff0f92cd267796a14ddc6d2232a0140a
[0.1.1]: https://github.com/CallMeLewis/AA-EntryNav/tree/f6cb9794f800dab7a6c406ac31579f0b373aa277
[0.1.0]: https://github.com/CallMeLewis/AA-EntryNav/tree/19387f1d350ca802f3bd4d6e1b0804f362ea2591
