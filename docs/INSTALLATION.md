# Installing by hand

Flare Drive installs AA-EntryNav for you. This guide does the same job over SSH with `setup.sh`, the script that comes
in every release archive. The script adds two folders to the head unit's system partition, `/opt/aa` and
`/opt/hmi/data/ro/Plugins/AAEntryNav`, and replaces none of BMW's files. It makes the partition writable only after
every check has passed, and only for the few seconds the copy takes.

Every command below was run with the car's BusyBox 1.21 under QEMU. `setup.sh` was also tested with the power cut at
each of its steps, as [how setup.sh protects the head unit](#how-setupsh-protects-the-head-unit) explains.

> [!CAUTION]
> Park the car with the ignition on and the engine off, and connect a battery charger if you have one.
> `setup.sh install` prints a line when it starts writing. Leave the ignition on until it finishes a few seconds
> later. If you're ever unsure what state the system is in, restart the head unit: it always starts read-only.

## What you need

- An **EntryNav2** head unit. [COMPATIBILITY.md](COMPATIBILITY.md) lists the cars that have one.
- A PC with an SSH client. Windows 10 and 11 have OpenSSH built in, and Linux and macOS work too.
- An **ENET cable** from the car's OBD port to the PC. Leave the PC's network adapter on automatic addressing.
- The head unit's **root password**. This project doesn't supply it.
- A release archive, `aa-entrynav-<version>-armhf.tar`, built by `tools/package_release.py`
  ([building it yourself](../README.md#building-it-yourself)). Its SHA-256 must match the `.sha256` file next to it.

The commands use version `0.1.0` and the address `169.254.1.0`. Put your archive's version in place of `0.1.0`.
`169.254.1.0` is where the reference car's head unit answers on the ENET cable, so use your head unit's address if it
differs.

## Back up the head unit first

Do this once, before the first install. `tools/backup_headunit.sh` copies the head unit's system to your PC. You can
compare against the copy or restore a file from it, and the ARM build and the QEMU tests need it. Run it from the
repository on Linux, WSL, macOS or Git Bash:

```sh
sh tools/backup_headunit.sh                 # asks for the root password, once
sh tools/backup_headunit.sh --print-remote  # shows what runs on the car, and runs nothing
```

The script only reads from the car. It saves a few facts about the system, such as the mounts, the partitions and the
BusyBox applets, to `/tmp`, which lives in RAM. `tar` then streams the files back over the SSH session. Nothing is
written to flash, and nothing is started or stopped. The copy leaves out virtual file systems (`/proc`, `/sys`,
`/dev`...), RAM file systems (`/tmp`, `/run`...), removable media, and `/var/local`. That last one is the data
partition, which holds paired phones and settings.

The copy is saved as `~/entrynav2/backups/headunit-<software>-<date>.tar`, with a `.sha256` beside it. It contains
BMW's files and the car's own settings, the Wi-Fi password included. Keep it on your PC and don't share it. The script
refuses to save it inside a git repository. To use it for the build, unpack it on Linux or WSL:

```sh
mkdir -p ~/entrynav2/rootfs && tar -xpf <copy>.tar -C ~/entrynav2/rootfs
```

## Step 1: copy the release to the head unit

On the PC, from the folder that holds the archive:

```sh
scp aa-entrynav-0.1.0-armhf.tar root@169.254.1.0:/tmp/
ssh root@169.254.1.0
```

Every command from here on runs in that SSH session, on the head unit. Unpack the archive in `/tmp`. It's a RAM file
system, so nothing reaches flash yet:

```sh
cd /tmp && rm -rf aa-entrynav-0.1.0 && tar -xf aa-entrynav-0.1.0-armhf.tar && cd aa-entrynav-0.1.0
```

## Step 2: run the checks

```sh
sh setup.sh check
```

This step changes nothing. Each line of the output starts with its level:

| Level | Meaning |
|---|---|
| `ok` | The point passed |
| `info` | Shown for your information |
| `warn` | Doubtful, but it doesn't stop the install |
| `STOP` | Blocks the install |

A single `STOP` line means don't install. `setup.sh install` would refuse anyway. The checks run in this order:

1. You're root, the BusyBox commands the script uses exist, the system partition is read-only, and Android Auto isn't
   running.
2. The archive matches its `MD5SUMS`, and its version has the right form.
3. The iDrive software version from `/etc/version`. It's printed for your report and decides nothing: any version
   installs if everything else passes ([what the result means](COMPATIBILITY.md#what-the-result-means)).
4. The folders the script will add sit on the system partition, with any symbolic link followed. If
   `/opt/aa` or `/opt/hmi/data/ro/Plugins/AAEntryNav` already exists, it must carry this project's marker. Otherwise
   the script leaves it alone and stops.
5. The system partition's file system. A `warn` line appears if it isn't one that keeps a rename whole through a power
   cut, as ext3, ext4 and ubifs do.
6. Working folders left by an earlier update. The script deletes them later, unless iDrive still has them loaded, in
   which case you have to restart first.
7. The data partition is writable. Settings, logs and the crash guard live there.
8. There's room on the system partition for the release plus 16 MB.
9. If `/var/local/data/AA/app.disable` exists, a `warn` line says Android Auto will stay off until you remove it.
10. The head unit's own loader, in list mode, resolves every library and function the programs use. Nothing runs.
11. `evo-trial --compat` rehearses an Android Auto start without changing anything, and has to reach the end
    ([what it looks at](COMPATIBILITY.md#what-setupsh-check-looks-at)).

For a car that isn't in the [table of cars tried](COMPATIBILITY.md#cars-tried-so-far), send this output together with
the first line of `/etc/version`. Each run is logged to `/tmp/aa-entrynav-setup.log`.

## Step 3: install

If an older version is running, stop it first with *Stop Android Auto* in its iDrive menu. Then:

```sh
sh setup.sh install
```

`install` runs every check again and writes nothing unless they all pass. Then it:

1. Writes `/var/local/data/AA/install-record` on the data partition, with the version and whether the
   `/opt/hmi/data/ro/Plugins` folder existed before.
2. Makes the system partition writable and copies the release into two working folders,
   `/opt/.aa-entrynav-aa.new` and `/opt/.aa-entrynav-plugin.new`, away from iDrive's own folders. It checks the copy
   against `MD5SUMS`.
3. Makes sure Android Auto hasn't started in the meantime, then swaps the folders in by renaming them: the old iDrive
   app out, the programs in, the new app in. A rename never lands on a name that's already taken.
4. Deletes the old version. If iDrive still has the old app loaded, it moves it aside until the next restart.
5. Makes the system partition read-only again and checks the installed files once more.

It also copies `setup.sh` to `/opt/aa/setup.sh`, so `status`, `tidy` and `uninstall` work later without the archive.

### Naming the car (optional)

The phone shows the car's model and year, and stores its settings for the car under an identifier. The `vehicle` file
sets all three. Without its own `id=`, a phone used in two cars with AA-EntryNav mixes them up:

```sh
mkdir -p /var/local/data/AA && printf 'model=320d\nyear=2020\nid=%s\n' "$(cat /proc/sys/kernel/random/uuid)" > /var/local/data/AA/vehicle
```

If the car has run Android Auto before, adding `id=` makes the phone see a new car, and you have to accept it on the
phone again.

### Restarting

```sh
reboot
```

Once the head unit is back, connect over SSH again and run `sh /opt/aa/setup.sh status`. It compares the installed
files with their checksums, and its `files` line should say they all match. This is the read-back that counts, because
the check during the install may have read the files from RAM rather than flash.

## Step 4: start Android Auto

On iDrive, open *Media*, *Android Auto*, *Start Android Auto*. The first time, pair the phone with the car over
Bluetooth if it isn't paired already, and accept the car on the phone. The phone then joins the car's Wi-Fi by itself.
Tick *Start automatically* in the same menu to have Android Auto start with iDrive.

## Updating

Stop Android Auto from its menu, then follow steps 1 to 3 with the new archive. Your settings in `/var/local/data/AA`
stay, and the crash guard's count starts again for the new version. iDrive keeps the old app loaded until it
restarts, so `install` moves it aside. After the restart, delete it:

```sh
sh /opt/aa/setup.sh tidy
```

Until that restart, `install` refuses a second update and writes nothing.

## Checking an installation

```sh
sh /opt/aa/setup.sh status
```

`status` writes nothing. It shows the installed version, the system partition's state, whether the installed files
match `/opt/aa/MD5SUMS`, any folders left over and whether they're in use, whether iDrive has loaded the app, whether
Android Auto is running, and the crash guard's count.

## Uninstalling

Stop Android Auto from its menu, then run one of these:

```sh
sh /opt/aa/setup.sh uninstall           # keeps the settings in /var/local/data/AA
sh /opt/aa/setup.sh uninstall --purge   # deletes the settings too
```

The script moves the iDrive app out of `/opt/hmi/data/ro/Plugins` first, so iDrive won't load it on its next start.
iDrive holds on to the app until then, so the script sets it aside and tells you to restart the head unit and run the
same command again to delete the last files. The same happens if a program has a file in `/opt/aa` open, or an SSH
session is sitting in that folder, so run `cd /` first. The script removes the `Plugins` folder only if this project
created it and it's empty.

## If something goes wrong

**To switch the app off without touching the system partition,** run `touch /var/local/data/AA/app.disable`, then
`reboot`. iDrive starts without it. To switch it back on, run `rm /var/local/data/AA/app.disable` and `reboot` again.

**An install or uninstall was cut short** by a power cut or a lost SSH session. Restart the head unit and run the same
command again. Each step picks up where it stopped, and in the meantime iDrive finds either no app or a complete app
next to its own programs.

**`setup.sh` reports that the system partition is still writable.** Run `reboot`. The head unit always starts
read-only.

**The iDrive screen stops responding.** Hold the iDrive on/off button for about 30 seconds to restart the head unit.

**There's no Android Auto row in *Media*.** Run `sh /opt/aa/setup.sh status`, then restart the head unit. iDrive
reads its apps only when it starts.

**iDrive restarts several times right after loading the app.** After the third failed start the app stays off and
iDrive carries on without it ([crash guard](MAINTENANCE.md#crash-guard)). Once the cause is fixed, run
`rm -f /var/local/data/AA/app.disable /var/local/data/AA/app-guard` and restart to switch it back on.

**Android Auto doesn't start.** Turn on the logs ([MAINTENANCE.md](MAINTENANCE.md)), try again, then read
`/tmp/aa-entrynav-service.log` and `/var/local/data/AA/app.log`.

## How setup.sh protects the head unit

- If any check fails, the script writes nothing to either partition.
- The system partition is writable only during the copy and the renames. The script ignores Ctrl-C and a dropped SSH
  session in that window, and makes the partition read-only again on the way out, after an error too. If that fails,
  it says so, and a restart fixes it.
- The two folders change only when the script renames a complete, checked folder into place, and a rename is atomic.
  Whenever the power cuts out, iDrive finds no app, the whole old version or the whole new one, each with its own
  programs. Running the command again after a restart finishes the job.
- The script never deletes a file that's in use: loaded, open, or the current folder of a program. Deleting it would
  stop the system partition from going read-only. It sets the file aside instead, and a later `tidy`, `install` or
  `uninstall` deletes it.
- It touches only folders marked `.installed-by-aa-entrynav`, plus its own hidden working folders
  (`/opt/.aa-entrynav-*`). It stops if `/opt/aa` or an `AAEntryNav` folder came from somewhere else or is a symbolic
  link. In iDrive's data folder, `/opt/hmi/data/ro`, it only ever adds `Plugins/AAEntryNav`.
- It replaces no BMW file and sends nothing on the car's network.

`tests/test_setup.py` tests these points on a PC. It runs a fake head unit in a `chroot` with the car's BusyBox 1.21,
and makes `mv`, `cp` and `rm` fail or cut the power at each of their calls during an install, an update and an
uninstall. None of this has been tried in a car yet.

The script can't promise two things. On a file system that doesn't keep a rename whole, a power cut could still
damage a folder, which is why the checks print a `warn` line there. And the check after copying may have read the
files from RAM rather than flash, which is why you run `status` after the restart.
