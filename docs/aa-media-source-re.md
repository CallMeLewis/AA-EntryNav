# Android Auto as iDrive's media source

Status: static analysis of the saved head-unit system, 18 November 2026. Nothing here has been tried on the car.
Addresses are virtual addresses in the named binary from that copy (`entrynav2-rootfs`).

## The problem

With sound over Wi-Fi, Android Auto asks the car's audio manager for connection `0x315`, CarPlay's media route
(`platform/audio_route.hpp`). The music plays, but three things fail:

1. The iDrive MEDIA key does nothing while iDrive is on screen.
2. The steering-wheel roller opens the cluster's previous/next list, which shows no track information. Earlier the
   cluster said "no active entertainment source".
3. Next and previous never reach the phone.

The 18 November capture on the car (`artifacts/car-2026-11-18/`) shows the MEDIA presses reach iDrive and our service
leaves them alone, and the roller arrives as ControlElementsMFL `0x15.02` function `0xC30` (`RotarySteps`).

## How iDrive decides what MEDIA opens

`CHmiHomeApplication::processHardKey` (HmiMain3D `0x25c24c2`) handles MEDIA like this:

1. If iDrive's media application is already in front, it asks for the source menu
   ("media application already active -> request mode").
2. Otherwise it asks `IHmiCarPlayService` (vtable slot `0x18`, `isCarPlayMediaSourceActive`). If that is true it logs
   "media application not active, carplay audio source -> open carplay" and calls slot `0x1c`
   (`requestCarPlayUI("nowplaying:")`) at `0x25c27d8`. Nothing else happens.
3. Failing that, the same check for CarLife opens CarLife.
4. Only then does it open iDrive's own media screen ("open media entry state").

`CHmiCarPlayService::isCarPlayMediaSourceActive` (`0x2677c04`) asks the audio service, not CarPlay:
`IHmiAudioService::getCurrentAudioSource(class 1, sink 0xff) == 0x82` and `isAudioSourceBuiltUp(0x82)`
(libhmiaudio.so `CHmiAudioService` vtable slots `0xd0` and `0xc8`). Audio source `0x82` is CarPlay's entertainment
source, and it is what connection `0x315` builds.

So the MEDIA failure follows from the route we borrow. iDrive believes CarPlay is playing and asks CarPlay to show its
now-playing screen. With no iPhone connected that request goes nowhere. This explains symptom 1 completely.

## How CarPlay is a source

`/usr/local/bin/CARPLAY` hosts a MOST FBlock of its own: AuxiliaryInput (FBlock `0x24`), instance `0x70`.
`CVMostCARPLAY::CVMostCARPLAY` creates it at `0x1913e4` (`mov r1, #0x70` before `CSimAuxiliaryInput(uint8_t)`).
CarLife does the same with instance `0x71` (CARLIFE `0xce7cc`). Both run as systemd services
(`CARPLAY.service`, `CARLIFE.service`).

HmiMain3D talks to that instance through `CPxyAuxiliaryInput`, using libhmil6services.so's proxy:

- `CEntPlayerCarPlay` is the entertainment player, and its device is `CDevCarPlay`, a `CDevExternalBase` /
  `CDevMusicDatabase`. It reads track data from the instance's notifications
  (`CDevExternalBase::setExternalNotifications`, `0x28141e0`).
- The cluster's roller list for CarPlay is `CICCarPlaySkipController`. HmiMain3D builds it with instance `0x70` at
  `0x2c83c8a`, and it subscribes to `AuxIn_CurrentAudioFileInfo` (`0x2c9c474`).
- Next and previous go through `CEntSkipService` to `CDevMusicDatabaseSkipDevice::skip` (`0x28029ac`). That calls the
  device's `incrementTrack` / `decrementTrack` (`0x281a56a`), which sends `TrackInformation.Increment` or
  `.Decrement`. Devices that report iAP2 support use `AuxIn_iAP2PlaybackControl.Start` with key events `3`/`0x83`
  instead.

Function IDs come from `CPxyAuxiliaryInput::init` in libhmil6services.so (`0x314a4c`), which pairs each member with its
ID:

| Function | ID | Use |
| --- | --- | --- |
| DeckStatusAuxIn | `0x200` | play and pause state |
| TrackInformation | `0x434` | Increment and Decrement are next and previous |
| AuxIn_Search | `0xC8D` | StartAck `[handle u16][direction u8]`, fast forward and rewind |
| AuxIn_CurrentAudioFileInfo | `0xC9D` | Status `[RelativeObjectPosition u16][PRMediaData]`: title, artist, album |
| AuxIn_iAP2PlaybackControl | `0xCE0` | iAP2 key events |

The CarPlay stub handles `onTrackInformation_increment` / `_decrement`, `onDeckStatusAuxIn_set` and
`onAuxIn_Search_startAck`. `PRMediaData` starts with a 16-bit field mask; `readPRMediaData` (libhmil6services.so
`0x494f80`) and `writePRMediaData` (`0x496108`) give the layout. That layout has not been decoded yet.

A cluster with an empty list therefore means CARPLAY has no track to report. The roller list asks instance `0x70`, and
nothing behind it knows about Android Auto.

## Proposal

Keep `0x315`. Moving to another route would lose the radio switch-off and the volume behaviour that already work.
Make the three symptoms work around the fact that iDrive thinks CarPlay is playing, in three separate changes.

### 1. MEDIA opens Android Auto's media app (smallest, do first)

Implemented on 18 November 2026 (`platform/media_key_jump.hpp`), not yet tried on the car.

In the service, when MEDIA is decoded with iDrive on screen, a session running and our media route built, bring
Android Auto back and send it `AAK_MEDIA`. That is what MEDIA already does while AA is on screen. iDrive's own
handling stays as it is: it calls `requestCarPlayUI("nowplaying:")`, which the car showed does nothing without an
iPhone. With the route given back, iDrive's MEDIA behaves as before. This needs no new car traffic.

Verify on the car: no CarPlay popup or error appears, the hold machine does not take the screen straight back, and
MEDIA with the radio or Bluetooth playing is unchanged.

### 2. Next and previous reach the phone

Shadow FBlock `0x24` instance `0x70` for `TrackInformation` (`0x434`), the way the knob code shadows the iDrive
controller. Turn Increment into AA's next-track key and Decrement into previous. Do it only while the session runs and
the route is ours, and leave the request to reach CARPLAY as today. Copy `AuxIn_iAP2PlaybackControl` (`0xCE0`) too, in
case iDrive takes that path. Advertise the media keys in service discovery, with an intentional golden-file update.

Verify on the car, with a read-only tool first:

- Does vmostd copy requests addressed to an FBlock hosted on the head unit itself (CARPLAY's), as it does for the
  controller's? This is the main unknown.
- Which function and operation the roller list's previous and next actually send.
- What CARPLAY answers, and whether iDrive shows an error for it.

### 3. Track information in the cluster

Send `AuxIn_CurrentAudioFileInfo` (`0xC9D`) status for `0x24.0x70` to HmiMain3D with the title and artist from AA's
media status. This writes as if from CARPLAY's instance, so it is the riskiest part. Gate it like change 2, and make it
an opt-in experiment first.

Before building it:

- Decode `PRMediaData`. A capture is quicker than static work: shadow Bluetooth audio's AuxIn instance for `0xC9D`
  while phone music plays over Bluetooth, which gives a real payload in the same format.
- Find out whether HmiMain3D accepts a status from a sender other than CARPLAY, and whether CARPLAY's own empty status
  later overwrites it, for example when iDrive does a Get.

### What this could affect

- **CarPlay with an iPhone:** all three changes act only while an AA session runs and holds `0x315`, which CarPlay
  then cannot hold. A car shared between an iPhone and an Android phone should still be tested.
- **Source restoration:** unchanged. Giving back `0x315` ends iDrive's belief that CarPlay is playing.
- **Bluetooth audio, radio, calls:** the shadows and the status are scoped to instance `0x70`. Bluetooth audio's own
  next and previous keep working through Bluetooth.

### Not recommended

- Registering our own AuxIn stub on `0x70` or `0x71`. CARPLAY and CARLIFE already own those instances, and the MEDIA
  path would still open the CarPlay or CarLife UI.
- Selecting Bluetooth audio as the source. The plan in `usb-recovery-and-wheel-media-plan.md` already rules this out.
- Hooking `processHardKey` inside HmiMain3D. Change 1 gets the same result from the service, without patching
  iDrive's code.

## Roller capture (18 November)

`RotarySteps` (`0x15.02`, `0xC30`, Status) idles at `0x80`. Each movement sends a short burst of values around it,
such as `81 80 7e 7d`, and up and down have not been told apart yet. `RotaryPress` (`0xC31`), `SearchSwitch1`
(`0x511`) and `SpecialFunction` (`0x513`) did not change. iDrive handles the roller itself (it opened the cluster
list), so change 2 hooks iDrive's skip request rather than decoding the roller.
