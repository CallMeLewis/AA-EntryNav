# Android Auto as iDrive's media source

Status: static analysis of the saved head-unit system, 18 November 2026. Nothing here has been tried on the car.
Addresses are virtual addresses in the named binary from that copy (`entrynav2-rootfs`).

## The problem

With sound over Wi-Fi, Android Auto asks the car's audio manager for connection `0x315`, CarPlay's media route
(`platform/audio_route.hpp`). The music plays, but three things fail:

1. The iDrive MEDIA key does nothing while iDrive is on screen.
2. The steering-wheel roller shows "no active entertainment source" in the cluster (0.1.7, music playing). On
   18 November it opened an empty previous/next list instead, while `evo-mfl` ran.
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

## Why the cluster says "no active entertainment source"

Static analysis, 18 November 2026, after 0.1.7 showed the message with music playing, MEDIA pressed or not.

The roller is iDrive's: `CHmiControlElementsMFLController::onRotaryStepsStatus` (`0x2567178`) turns RotarySteps into
event `0x36` (value below `0x7F`, `0x7F - value` steps) or `0x37` (value above, `value - 0x7F` steps), ignoring `0x7F`
and `0xFF`. The cluster's lists move one row down for `0x36` and up for `0x37` (`CHmiICListInputHandler::handleInput`,
`0x35fdcb4`). `CHmiInstrumentClusterApp::onICControllerEvent` (`0x2c828d0`) calls `openCurrentState`
(`0x2c8226c`) for both, which shows the menu of the cluster's current audio mode for 4 seconds.

That mode does not come from the audio source. It is the IC audio mode (`IICAudioModeConstants::EMode`, names from
`getICModeString`: `CDMM_CarPlay` is `0x2c`, `CDMM_NO_AUDIO` is `0x32`), which
`CICEntertainmentSourcesListController::PlayerTypeToICMode` (`0x2ca5f7c`) derives from the active player in
`CHmiModePlayerEvent`. `CEntModeButtonService` sends that event (`notifyModeSequenceListeners`, `0x26e9338`) from its
list of registered mode handlers. `alignModeHandler` (`0x26e979c`) looks for the handler whose device plays; when none
does it logs "Mode handler for currently playing audio source ... not found", and the cluster mode becomes
`CDMM_NO_AUDIO`.

CarPlay's handler (`CEntModeHandlerCarPlay`, made by `CEntPlayerCarPlay::initModeHandler`) only registers once
`CEntModeHandlerBase::calculateModeHandlerAvailability` (`0x26e580c`) finds its device connected.
`CDevCarPlay::isRemovableDeviceConnected` (`0x2682e24`) reads that from CARPLAY's `AuxIn_DeviceInfo2` on `0x24.0x70`:
DeviceStatus must be 2. Without an iPhone it never is. So the audio service sees source `0x82` (MEDIA's check), while
the entertainment side has no player for it.

The cluster's "now playing" text follows the audio source instead (`CEntCurrentUpdaterHelper::setupUpdaterForAudioSource`,
`0x2882240`): CarPlay's updater for `0x81` to `0x83`, the "no audio" one (`CEntInfoUpdaterNoAudio`, text `0x2000f550`)
for source 0.

What does not fix it:

- Announcing a CarPlay device by sending `AuxIn_DeviceInfo2` as if from `0x24.0x70`. It would register the handler,
  but CARPLAY owns that instance and answers iDrive's reads with the truth. When the status falls back to
  disconnected while CarPlay's source plays, `CDevMediaPlayerAbstract::handleRemovableDeviceState` (`0x2803230`) asks
  the audio service to drop it, which would cut Android Auto's sound. iDrive would also list CarPlay as a source.
- The app-media path (A4A, `CICA4AController`). It has cluster text and skip actions for BMW Apps, but only for A4A
  audio sources (`0x39` to `0x3e`, `0x7e`, `0x7f`, `0x8a`, `0x8c`), so it needs another route and a registered BMW
  Apps audio session.

What remains possible for the message: a mode handler of our own, registered from the iDrive app inside HmiMain3D
through `IEntModeButtonService::registerModeHandler` (vtable slot `0x18`), active while route `0x315` is ours. The
`IEntModeHandler` layout is known from `CEntModeHandlerCarPlay`'s vtable (`0x3843058`, 22 slots). It would put
Android Auto into iDrive's source list and the MODE sequence too, so it needs care and a car to try it on.

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

Implemented differently on 18 November 2026 (`platform/wheel_roller.hpp`), not yet tried on the car. The cluster's
mode is `CDMM_NO_AUDIO` (see above), so the roller never reaches CarPlay's skip list and iDrive sends no skip. The
service reads RotarySteps and RotaryPress (`0xC31`, 1 pressed, 0 released; `onRotaryPressStatus`, `0x2567348`,
event `0x38`, which plays the row the roll reached) itself. As with iDrive's own lists, a roll then a push within 4
seconds sends Android Auto's next or previous track key, in the direction iDrive's lists use. With no list to show
how far the roll went, one push is one skip. The rest of this section is the earlier plan, for the steering-wheel skip buttons some wheels have.

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

`RotarySteps` (`0x15.02`, `0xC30`, Status) answered our read with `0x80`, its last value. Each movement sends a
short burst about 120 ms apart, such as `7e 7d` then `81 80`: below `0x7F` is down iDrive's lists, above is up (see
above). Which way is physically up on the wheel has not been checked. `RotaryPress` (`0xC31`), `SearchSwitch1`
(`0x511`) and `SpecialFunction` (`0x513`) did not change. Whether the roller was pushed during that capture is not
recorded; if it was, its push reaches iDrive some other way and the roll-then-push skip needs that message instead.
