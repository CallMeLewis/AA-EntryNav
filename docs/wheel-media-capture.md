# Capture the steering-wheel media controls

The reported fault happens with sound over Wi-Fi: the cluster says something like "no entertainment source selected"
and scrolling or changing tracks does nothing. The wheel fix is awaiting a car capture. The USB cooldown is a
separate change and is not needed for this capture.

The updated `evo-mfl` is a developer tool, not an AA installation. It copies steering-wheel messages and sends plain
Get reads on ControlElementsMFL 0x15.02 for telephone (0x400), volume (0x401), search (0x511), voice (0x512), special
function (0x513), roller steps (0xC30) and roller press (0xC31). It never selects a player, forwards a button, or
changes a notification subscription. The status reads can elicit replies; iDrive keeps receiving its controls.

## Prepare on the PC

The built `evo-mfl` is in the repository root. With the toolchain and the car's saved system available, rebuild it
with `bash toolchain/tc.sh make evo-mfl` on Linux. On Windows, use:

```powershell
docker run --rm -v "${PWD}:/work" -v entrynav2-rootfs:/sysroot:ro -w /work entrynav2-tc:4.7 make evo-mfl
```

Connect through ENET as for installation, with the car parked. Copy only this tool into memory on the head unit:

```powershell
scp ./evo-mfl root@169.254.1.0:/tmp/evo-mfl
ssh root@169.254.1.0 "chmod 700 /tmp/evo-mfl"
```

Use the head-unit address from your usual connection if it differs. Use your existing SSH authentication. The tool
does not require a release update or a writable system partition.

## Record the Wi-Fi-audio failure first

Start Android Auto with sound over Wi-Fi enabled and play music. Keep AA visible. Run this in PowerShell; it shows
the capture and saves a copy on the PC:

```powershell
ssh root@169.254.1.0 "LD_LIBRARY_PATH=/usr/local/lib:/usr/lib:/lib /tmp/evo-mfl 90" | Tee-Object -FilePath wheel-wifi.txt
```

During the 90 seconds, leave about 5 seconds between each action:

1. Leave the controls alone briefly to establish the idle values.
2. Scroll the steering-wheel roller up one notch, then down one notch.
3. Press and release the roller once.
4. If fitted, tap the dedicated next-track button, then the previous-track button.
5. Repeat each failing action once. Leave volume, phone and voice buttons alone during this part.

Write down the order, approximate timestamps and the exact cluster message. Say which actions, if any, changed the
track or opened a list. Note the car's iDrive software version and whether the phone was connected by cable or Wi-Fi.
The tool prints named lines such as `RotarySteps`, `RotaryPress` and `SearchSwitch1`, with the raw payload in hex.
Read errors or missing messages are useful evidence too. Do not change the selected source halfway through a run.

## Compare with Bluetooth audio

When convenient, repeat the same actions using AA with sound over Bluetooth, saving the output as `wheel-aa-bt.txt`.
Choose Bluetooth audio in iDrive's Media menu so it is actually the active source. The existing settings guide
describes changing `audio-wifi`; stop and start AA after changing it. Restore the Wi-Fi-audio setting afterwards.

A third capture, `wheel-native-bt.txt`, with AA stopped and ordinary Bluetooth music selected, gives the native
baseline. Record which actions work and whether a track list appears. These comparisons are optional for the first
capture; `wheel-wifi.txt` and the action order are enough to start the analysis.

The tool exits after 90 seconds and deregisters its copies. Keep the SSH connection open until it prints `done`.
It leaves no settings behind; `/tmp/evo-mfl` disappears when the head unit restarts. Do not include passwords, a VIN
or phone identifiers with the capture.
