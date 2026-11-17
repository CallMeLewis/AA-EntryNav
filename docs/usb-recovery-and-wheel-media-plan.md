# USB recovery and steering-wheel media controls

Status: USB cooldown implemented, 17 November 2026. Wheel decoding and native player integration await a car capture.
The diagnostic tool now includes the roller's reads; [capture steps](wheel-media-capture.md) are ready for the next
car session.

The car is unavailable for capture in this session. Media forwarding and source activation are therefore pending,
not implemented. Local library inspection confirms separate roller functions 0xC30 and 0xC31, which the previous
diagnostic tool did not query. Native CarPlay has an entertainment player in addition to its audio route; selecting
it for AA has not been validated and is not part of the implemented changes.

## Implementation and validation record

- USB: implemented in `usb_link`, `usb_loop.hpp` and `service_app`. The polling turn is shared with the fake-phone
  tests, including its 10-second failed-start delay, 5-second session-end delay and wireless ownership checks.
- Focused fake-phone tests: passed, including both deadline boundaries, repeated cleanup, clock wraparound, an
  observed unplug/replug immediately after setup, another port and a device-number change on the same port.
- ARM build, ARM tests under QEMU and release-binary checks: passed with `python tools/verify_arm.py`. The diagnostic
  tool's `--help` runs under QEMU too. The local transcript is `artifacts/usb-wheel-arm-check.log`.
- Full PC checks: passed with `make check HOST_CXX=g++ STYLE_TOOLS=/tmp/style/bin RUFF=/tmp/style/bin/ruff
  BUSYBOX="$(sh tests/build_busybox.sh)"` in the temporary test container. This includes style, content checks and
  the installer harness with the expected BusyBox. The local transcript is `artifacts/usb-wheel-host-check.log`.
- Hardware: not tested. USB behaviour, wheel input mapping and native player integration need car validation. No
  release has been published or installed.

## Scope and evidence

Two independently testable changes, delivered separately:

1. Give a phone time to finish a cable session before asking it to start another.
2. Restore steering-wheel media control with experimental Wi-Fi audio, and resolve the cluster's source warning.

The reported wheel failure happens with `audio-wifi` enabled: scrolling or trying to change tracks produces a cluster
message like "no entertainment source selected", and the track does not change. The exact message is unconfirmed.

`EvoKnob::onVmost` handles steering-wheel volume, telephone and voice events. It has no search-switch or scroll
decoder and no next/previous-track forwarding. `tools/evo_mfl.cpp` already reads the named search-switch and special
functions for diagnosis. `EvoAudio` requests an audio route, but this does not establish that iDrive recognises an
entertainment player. `BtAudioPlan` deliberately does not select Bluetooth audio when `audio-wifi` exists.

Missing input handling is established from the code. The relationship between player selection and the cluster
warning remains a hypothesis to test. Receiving copied buttons does not stop iDrive from processing them too.

## Change 1: USB cooldown

### Behaviour

- After a session ends while its phone remains connected, wait 5 seconds before resetting or reconnecting that phone.
- If `startSession` fails, retain a 10-second delay instead. Both delays start from cleanup of the failed attempt;
  they must not add together or restart on each poll.
- Use the bus's monotonic clock and unsigned elapsed-time comparisons, including clock wraparound.
- Continue enumerating devices during the pause. Report the phone as present, and make an observed unplug/replug
  eligible immediately. A replug shorter than the observation interval cannot be distinguished reliably.
- Scope the pause to its port; another phone must not inherit it. Explicit stop must complete without waiting out it.
- Preserve the existing refusal handling, first-time-prompt grace period, reset budget and mode-change detection.
- Keep the current reset/re-enumeration sequence. Do not copy QNX device handling or add a fixed post-reset sleep.

### Implementation

1. In `platform/usb_link.hpp` and `.cpp`, add an explicit way for the caller to end an attempt with its retry delay.
   Reuse the existing per-port rest mechanism where possible. Keep repeated cleanup idempotent: `connect()` calls
   `close()` itself, and shutdown also calls it.
2. In `platform/service_app.cpp`, apply the 5-second delay when a carried session ends, and close a failed startup
   attempt with a 10-second delay. Replace the current blocking startup-failure sleep with that deadline.
3. Keep `usbPhone`, `usbBusy` and the wireless handoff consistent during the pause. The idle cable must not block a
   wireless connection, and a running wireless session must not be replaced when the pause expires.
4. Log one line when a cooldown begins, including its reason and duration. Avoid a line on every poll and exclude
   phone identifiers and addresses.

### Verification

Extend `tests/test_usb_link.cpp` using its fake clock and phone:

- A session ending on the same plug-in cannot cause a reset or accessory request before 5 seconds; it can at expiry.
- A failed startup waits 10 seconds total, including repeated cleanup and polling.
- Unplug observed during the pause, followed by replug, permits an immediate attempt.
- A different port remains eligible; mere device renumbering after a mode change does not clear the pause.
- Stop, clock wraparound, first-time setup and the existing silent-phone reset budget still behave correctly.

Exercise the caller's choice of delay and wireless ownership with an appropriate existing service test seam; a test
of the helper alone is insufficient. Wire any new test into the host or ARM suite as required by `verify_tests.py`.

Car acceptance: phone connected at service startup, normal plug-in, a failed or short-lived session, unplug/replug
during recovery, and Stop during the pause. Confirm one fresh session starts after the delay without repeated resets,
and that normal connection and Bluetooth pairing still work. PC results do not establish car success.

## Change 2: Wi-Fi audio media controls

### Establish the event and source behaviour

1. With the car parked, capture read-only `evo-mfl` output for each physical action separately: scroll up/down,
   wheel press, and any dedicated next/previous buttons fitted. Record whether Android Auto is visible and whether
   music is playing. Treat scrolling a track list and skipping a track as different actions until observed.
2. Compare native Bluetooth audio, AA using Bluetooth audio, and AA using Wi-Fi audio. Record whether tracks change
   and whether the cluster warning appears. Changing the selected native source is diagnostic, not a proposed fix.
3. Inspect the existing local BMW library evidence for `SearchSwitch1` and `SpecialFunction` and compare it with the
   captures. If scrolling produces no message on that interface, identify its actual source before adding mappings.
4. Trace native CarPlay's player activation and media-control registration separately from its speaker route. Check
   whether native playback metadata or a track-list provider is required for the cluster interaction.

### Implement the confirmed control path

- Add strict decoders beside the existing MFL decoders in `platform/evo_knob.hpp` and `.cpp`: validate instance,
  function, operation, length and state. Handle release, repeated status and held controls without duplicate skips.
- Add the required Android Auto media key definitions in `core/aa_input.hpp`, after checking the protocol contract.
  Confirm the phone is told those keys are supported by service discovery; change its advertised input capabilities
  only if necessary, with a documented intentional golden-file update. Preserve `vehicleId`.
- Send confirmed media events through the existing session input queue. Initially forward only when AA owns the
  screen, a session is active, and Wi-Fi audio is enabled. Leave native Bluetooth media commands alone to avoid one
  press skipping twice through Bluetooth and AA.
- Do not map the steering-wheel roller to the iDrive controller's rotary navigation. Define scroll/press behaviour
  from the captures and the available AA media interface. If a native track list is required, scope that explicitly.
- Prevent stale pressed state across reconnects, focus changes and stop. Preserve volume, phone, voice, calls and
  native controls while AA is inactive.

### Resolve the cluster warning

Direct key forwarding and native player integration are separate acceptance conditions. If iDrive still shows the
warning after tracks change, the issue is not complete.

Use native CarPlay as the reference for the minimum player/source integration needed. Introduce a source-selection
or registration change only after confirming its calls, ownership, restoration and failure behaviour on EntryNav2.
Keep any experiment opt-in. Do not select Bluetooth audio as a Wi-Fi-audio workaround, suppress unrelated cluster
messages, or restart a BMW service. Document any new car-network traffic in the CHANGELOG and installation guide.

### Verification

- Add captured, anonymised event fixtures and decoder tests for valid actions, releases, repeats, malformed packets
  and unrelated functions. Check one intended skip per action through the real input queue and encoded AA message.
- Verify that inactive AA, Bluetooth audio, calls, focus changes and reconnects do not receive duplicate or stale
  commands. Add service-discovery checks if input capabilities change.
- Car acceptance: each supported action changes the intended track exactly once with Wi-Fi audio, the cluster does
  not show the source warning, and exiting AA restores native control. Retest volume, voice and phone controls.
- Test scrolling/selection separately from next/previous. Do not claim native track-list support from a skip test.

## Delivery and checks

Implement and validate USB recovery first. Keep steering-wheel diagnosis and its eventual fixes in separate changes.
Fragment reassembly and general audio-fallback work remain outside this plan.

Run `make check` for each implementation. Run `python tools/verify_arm.py` before committing changes covered only by
ARM builds/tests, and complete ARM/release verification before publishing. Record PC and car results separately in
release notes. Keep an existing release available for rollback; no installation or publication is part of this plan.
