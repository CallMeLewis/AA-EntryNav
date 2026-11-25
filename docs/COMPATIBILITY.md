# Which cars it runs on

## The head unit it needs

AA-EntryNav runs on one head unit only: BMW's **EntryNav2**, which its own system calls EntryEVO. The car also needs
the **Wi-Fi that CarPlay uses**. NBT Evo, ID7 and MGU are different systems, and the programs can't run on them.
Installing by hand also takes SSH access to the head unit as root, with its password.

## Screen sizes

The service reads the screen's size when it starts and asks the phone for a picture that fits it:

| Screen | Picture on Wi-Fi | Picture on the cable |
|---|---|---|
| 1280x480 | 1280x720, top 480 rows shown as they are | 1920x1080, top 720 rows scaled down |
| 800x480 | 800x480, shown as it is | 800x480, shown as it is |
| 800x400 | 800x480, top 400 rows shown as they are | 1280x720, top 640 rows scaled down |
| 1440x540 | 1280x720, top 480 rows stretched to the screen | 1920x1080, top 720 rows scaled down |

The phone draws nothing in the rows the screen doesn't show. Only the 1280x480 screen has been tried in a car. Other
sizes work if one of Android Auto's three pictures (800x480, 1280x720, 1920x1080) is at least as wide as the screen and
fills its height at the same scale. A phone on Wi-Fi is never asked for more than 1280x720, because phones asked for
1920x1080 on the car's Wi-Fi hung up, so a screen wider than 1280 gets 1280x720 stretched to it. On any other screen
the compatibility check stops with `video (screen WxH not supported)`.

## Cars tried so far

The **reference car** is the one AA-EntryNav is developed and tried on.

| Car | iDrive software (`/etc/version`) | Compatibility check | Last version tried | What was tried |
|---|---|---|---|---|
| 2019 G20 (reference) | `202111011321` | not yet run in a car | before 0.1.0 | everything in the [README](../README.md#what-it-does) |
| 2019 G20 M340i | `202306092158` | installed before `setup.sh` accepted any software | before 0.1.0 | wireless Android Auto, iDrive controller (older CCS protocol); the picture lags over the 2.4 GHz Wi-Fi |

Each new car joins this table with the output of its compatibility check and the first line of its `/etc/version`.

## What setup.sh check looks at

`sh setup.sh check` is step 2 of the [installation](INSTALLATION.md#step-2-run-the-checks), and `setup.sh install` runs
it again before writing anything. It changes nothing on the car: the programs it runs stay in `/tmp`, in RAM. Besides
the points below, it confirms that the system partition is read-only, that Android Auto isn't running, that the
folders it would add are on the system partition, that any of them already there came from this project, and that the
data partition is writable.

A failure in any of these stops the install:

- **The archive** has to match `MD5SUMS`. If it doesn't, copy it to the car again.
- **Free space** (`df -k /`): the system partition needs room for the release plus 16 MB.
- **Libraries and functions.** The head unit's loader, in list mode, has to find everything the programs call.
- **`abi`**: iDrive's interface for its apps exists, at the same size.
- **`vmost`**: the functions for the iDrive controller and buttons, which load at run time.
- **`video`** and **`order`**: a dry run of the start finds where the picture would go and how the layers stack.
- **`alps`**, **`uuid`** and **`spp`**: a dry run finds a Bluetooth library with the same structure, recognises the
  profile and reads the slot's state. A busy Bluetooth slot only gives a warning.

These only warn or inform:

- **`mic`** warns when the microphone functions are missing.
- **`running`** means Android Auto is already running, so the screen and Bluetooth weren't checked.
- **`hotspot`** and **`gps`** say whether the car's Wi-Fi is available and whether its GPS was recognised.

Two tools do the work. The car's own loader, in list mode, resolves every function without running anything. It
raises no false alarm on the reference car's system, and it notices when a function or a library has been removed on
purpose. `evo-trial --compat` makes the same reads as a real start and records nothing. It leaves Bluetooth as it is,
doesn't switch on the Wi-Fi and shows nothing on screen. It reads the Bluetooth state from the memory of BMW's
Bluetooth process without interrupting it.

## What the result means

- **No `STOP` line, on software `202111011321` or `202306092158`.** A car in the table above runs the same software.
  You can install.
- **No `STOP` line on other software, or some `warn` lines.** The head unit is the same kind, but nothing proves it
  behaves the same all the way through. You can install, and remove it again, at your own risk.
- **Any `STOP` line**, such as a blocking point from `evo-trial --compat` or a missing function. The car lacks
  something AA-EntryNav needs. Don't install. `setup.sh` refuses anyway.

On a car not yet in the table, expect the middle case. Every function exists and the dry run passes, but the check
can't prove that Android Auto will work end to end. `setup.sh` installs on any software version that passes its
checks, at the owner's risk. If it misbehaves, [INSTALLATION.md](INSTALLATION.md#if-something-goes-wrong) shows how to
switch the app off without touching the system partition.

## Bluetooth slots

BMW's Bluetooth stack has six serial port slots, numbered 0 to 5, and BMW sets up two of them. Slot 0 is for BMW Apps,
which the My BMW app connects to, and slot 1 is for CarPlay. Android Auto borrows slot 5 while it runs and gives it
back afterwards, so My BMW keeps slot 0. On a car where slot 5 isn't started or holds another identifier, Android Auto
falls back to slot 0, and My BMW can't find the car while Android Auto runs. The supervisor's log names the slot it
took: `Android Auto takes SPP 5` or `takes SPP 0`.

Android Auto finds the slot by reading the stack's tables in memory. On a car other than the reference one:

1. It looks the tables up in that car's library by their exported names.
2. Their three sizes have to match the reference car's. If they don't, the car is refused and the report gives the
   sizes it found.
3. Every read compares two copies of each identifier. If they disagree at all, Android Auto changes nothing.
4. After borrowing the slot, it reads the table again and looks for its own identifier. If it isn't there, Android
   Auto stops and gives the slot back.

Accepting another library by its structure has only been tried on a modified copy of the reference car's library. At
worst, restarting iDrive brings Bluetooth back.

## When the check can't tell

Some differences only show once Android Auto runs. Each has a fallback:

- If the screen or the video layer isn't what the video expects, it refuses to start.
- If the car's GPS sends a format it doesn't know, the phone's own GPS takes over.
- If the instrument cluster doesn't answer, there are no turn arrows, and everything else works.
- If the app makes iDrive crash, it switches itself off on the third attempt.
- If the Wi-Fi isn't available, the service stops with a message.

These fallbacks are tested on a PC. None has been tried on a car other than the reference one.
