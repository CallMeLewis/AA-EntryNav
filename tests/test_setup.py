# Copyright (c) 2026 CallMeLewis
"""tools/setup.sh and tools/backup_headunit.sh on a pretend head unit: a chroot with BusyBox, as root.

    python3 tests/test_setup.py [--busybox PATH]   (default: busybox on the PATH; the car runs 1.21)

The BusyBox must behave like the car's (unfit_busybox): tests/build_busybox.sh builds one.

The pretend unit has the parts setup.sh reads: /proc/mounts with a read-only system partition (mount flips it),
/etc/version, /opt/hmi/data/ro, the data partition, the unit's loader, df, pidof, and iDrive holding its app loaded
(/proc/<pid>/maps, which follows a renamed folder as the kernel does). Each case checks what was written, and that
nothing else was. The fault cases stop mv, cp or rm at each of their calls, by failing or by killing the whole run
(a power cut), then restart the unit and check that iDrive finds either no app or a complete one beside its own
programs, and that running the command again finishes the job.
"""

import argparse
import hashlib
import io
import os
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SETUP = ROOT / "tools/setup.sh"
SOFTWARE = "202111011321"
PLUGIN = "opt/hmi/data/ro/Plugins/AAEntryNav"
WORKING = (
    "opt/.aa-entrynav-aa.new",
    "opt/.aa-entrynav-aa.old",
    "opt/.aa-entrynav-plugin.new",
    "opt/.aa-entrynav-plugin.old",
)
HMI_PID = "412"
CHROOT = shutil.which("chroot") or "/usr/sbin/chroot"
TOOLS = ("bin/", "lib/", "sbin/", "usr/bin", "usr/sbin")  # the pretend unit's commands, not part of its state
DATA_RO = ("hmi.cfg", "Plugins")  # all iDrive's data folder may ever hold
BMW_FILES = ("opt/hmi/data/ro/hmi.cfg", "opt/hmi/lib/libAppFramework.A.so", "opt/other/keep.txt", "etc/version")
PSEUDO_MOUNTS = "proc /proc proc rw 0 0\ntmpfs /tmp tmpfs rw 0 0\ntmpfs /opt/hmi/run tmpfs rw 0 0\n"
COMPAT_OK = "@ok|abi|\n@ok|vmost|\n@ok|mic|\n@ok|video|\n@ok|alps|\n@ok|uuid|0011\n@ok|spp|\n@info|hotspot|\n@ok|gps|\n"
MOUNTS = "rootfs / rootfs rw 0 0\n/dev/root / ext4 ro,relatime 0 0\n/dev/mmcblk0p7 /var/local ext4 rw,noexec 0 0\n"

# Stand-ins for the unit's commands, all reading their behaviour from /test.
FAULTY = """#!/bin/sh
# mv, cp or rm, stopped at one call when /test/fault says so: "name call fail|kill".
name=${0##*/}
n=$(($(cat /test/count.$name 2>/dev/null || echo 0) + 1))
echo $n >/test/count.$name
if [ -f /test/fault ]; then
    read -r fname fat faction </test/fault
    if [ "$fname" = "$name" ] && [ "$fat" = "$n" ]; then
        echo "$name $n $faction" >/test/fault-hit
        if [ "$faction" = kill ]; then
            kill -9 0
        fi
        exit 1
    fi
fi
if [ "$name" = mv ] && busybox mv "$@"; then
    # The kernel names a mapped file by its current path: follow renamed folders in the pretend iDrive's maps.
    if [ -f /proc/412/maps ] && [ $# = 2 ]; then
        busybox sed -i "s|$1/|$2/|" /proc/412/maps
    fi
    exit 0
elif [ "$name" = mv ]; then
    exit 1
fi
exec busybox "$name" "$@"
"""
MOUNT = """#!/bin/sh
echo "$*" >>/test/mount.log
case "$*" in
    "-o remount,rw /") mode=rw ;;
    "-o remount,ro /") mode=ro ;;
    *)
        echo "$*" >>/test/unexpected
        exit 1
        ;;
esac
if [ -f /test/mount-fail-$mode ]; then
    exit 1
fi
busybox sed -i "s|^/dev/root / ext4 [a-z]*,|/dev/root / ext4 $mode,|" /proc/mounts
"""
DF = """#!/bin/sh
avail=$(cat /test/df-avail 2>/dev/null || echo 500000)
echo "Filesystem           1K-blocks      Used Available Use% Mounted on"
if [ -f /test/df-wrap ]; then
    echo "/dev/disk/by-partlabel/system-partition"
    echo "                       1000000    500000 $avail  50% /"
else
    echo "/dev/root              1000000    500000 $avail  50% /"
fi
"""
PIDOF = """#!/bin/sh
# /test/pids.<name> holds the pids of a running program; /test/running-from-call N starts Android Auto at the Nth
# time it is asked about.
case " $* " in
    *" evo-trial "*)
        if [ -f /test/running-from-call ]; then
            n=$(($(cat /test/count.pidof 2>/dev/null || echo 0) + 1))
            echo $n >/test/count.pidof
            if [ "$n" -ge "$(cat /test/running-from-call)" ]; then
                echo 77
                exit 0
            fi
        fi
        ;;
esac
found=1
for name in "$@"; do
    if [ -f /test/pids.$name ]; then
        cat /test/pids.$name
        found=0
    fi
done
exit $found
"""
LOADER = """#!/bin/sh
if [ -f /test/ld-output ]; then
    cat /test/ld-output
fi
exit 0
"""
TRIAL = """#!/bin/sh
# evo-trial {version}, pretend: --compat prints /test/compat, or a car where everything passes.
if [ -f /test/compat ]; then
    cat /test/compat
else
    printf '%s' '{compat}'
fi
"""


class Failure(Exception):
    pass


def check(condition, message):
    if not condition:
        raise Failure(message)


def md5(data):
    return hashlib.md5(data).hexdigest()


def tiny_zip(name, text):
    """The same bytes each time: a fixed date, not the current one."""
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as archive:
        archive.writestr(zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0)), text)
    return out.getvalue()


def release(version):
    """The files of a release archive, as tools/package_release.py lays them out (programs replaced by stand-ins)."""
    setup = SETUP.read_bytes()
    files = {
        "opt/aa/aa-entrynav": f"#!/bin/sh\necho aa-entrynav {version}\n".encode(),
        "opt/aa/evo-trial": TRIAL.replace("{version}", version).replace("{compat}", COMPAT_OK).encode(),
        "opt/aa/setup.sh": setup,
        "opt/aa/VERSION": f"{version}\n".encode(),
        f"{PLUGIN}/libAAEntryNav.so": b"\x7fELF pretend iDrive app " + version.encode(),
        f"{PLUGIN}/AAEntryNav.manifest": (ROOT / "hmiapp/AAEntryNav.manifest").read_bytes(),
        f"{PLUGIN}/AAEntryNav.xml": (ROOT / "hmiapp/AAEntryNav.xml").read_bytes(),
        f"{PLUGIN}/AAEntryNav_ALL_Texts.zip": tiny_zip("texts.txt", f"texts {version}"),
        f"{PLUGIN}/AAEntryNav_ALL_Images.zip": tiny_zip("images.txt", f"images {version}"),
        "setup.sh": setup,
    }
    files["MD5SUMS"] = "".join(f"{md5(data)}  {name}\n" for name, data in sorted(files.items())).encode()
    return files


def unfit_busybox(busybox):
    """Why this BusyBox cannot stand in for the car's, or None. It must be static, to run alone in the chroot (a
    dynamic one also obeys the loader settings meant for the unit's fake loader); its tar must leave folders out (-X);
    and its shell must run a command found on the PATH rather than its own of that name, as the car's does: with
    FEATURE_SH_STANDALONE (Debian's busybox-static) the unit's fake df, mv, cp and rm would never run, and the cases
    would pass without testing anything. tests/build_busybox.sh builds one that fits."""
    ldd = subprocess.run(["ldd", busybox], capture_output=True, text=True)
    if "not a dynamic executable" not in ldd.stdout + ldd.stderr:
        return "it is not static"
    tar = subprocess.run([busybox, "tar", "--help"], capture_output=True, text=True)
    if "-X" not in tar.stdout + tar.stderr:
        return "its tar cannot leave folders out (-X)"
    with tempfile.TemporaryDirectory() as folder:
        fake = Path(folder) / "df"
        fake.write_text("#!/bin/sh\necho fake-df\n")
        fake.chmod(0o755)
        env = {"PATH": f"{folder}:/usr/bin:/bin"}
        result = subprocess.run([busybox, "sh", "-c", "df"], capture_output=True, text=True, env=env)
        if result.stdout.strip() != "fake-df":
            return "its shell runs its own commands before the PATH (FEATURE_SH_STANDALONE)"
    return None


class Unit:
    """A pretend head unit in a folder, entered with chroot."""

    def __init__(self, base, busybox):
        self.root = Path(tempfile.mkdtemp(prefix="unit-", dir=base))
        self.busybox = busybox
        self.build()

    def path(self, name):
        return self.root / name

    def write(self, name, data, mode=0o644):
        path = self.path(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode() if isinstance(data, str) else data)
        path.chmod(mode)

    def build(self):
        for folder in ("bin", "sbin", "usr/bin", "usr/sbin", "lib", "tmp", "test", "proc", "dev", "var/local/data"):
            self.path(folder).mkdir(parents=True, exist_ok=True)
        os.mknod(self.path("dev/null"), 0o666 | stat.S_IFCHR, os.makedev(1, 3))
        self.path("dev/null").chmod(0o666)
        shutil.copy(self.busybox, self.path("bin/busybox"))
        self.path("bin/busybox").chmod(0o755)
        applets = subprocess.run([self.busybox, "--list"], capture_output=True, text=True, check=True).stdout.split()
        for applet in applets:
            link = self.path("bin") / applet
            if applet != "busybox" and not link.exists():
                link.symlink_to("busybox")
        for name in ("mv", "cp", "rm"):
            self.path(f"bin/{name}").unlink()
            self.write(f"bin/{name}", FAULTY, 0o755)
        for name, script in (("mount", MOUNT), ("df", DF), ("pidof", PIDOF)):
            self.path(f"bin/{name}").unlink()
            self.write(f"bin/{name}", script, 0o755)
        self.write("lib/ld-linux-armhf.so.3", LOADER, 0o755)
        self.write("proc/mounts", MOUNTS)
        self.write("etc/version", f"{SOFTWARE}\nbuild 1\n")
        # BMW files that must never change.
        self.write("opt/hmi/data/ro/hmi.cfg", "iDrive data\n")
        self.write("opt/hmi/lib/libAppFramework.A.so", b"\x7fELF BMW framework")
        self.write("opt/other/keep.txt", "BMW\n")
        self.write("usr/local/lib/libvmost.so", b"\x7fELF BMW vmost")
        self.pristine = self.snapshot()

    def unpack(self, version):
        """The release archive unpacked in /tmp, as the installation guide does it."""
        top = f"tmp/aa-entrynav-{version}"
        for name, data in release(version).items():
            executable = name.endswith(("aa-entrynav", "evo-trial", "libAAEntryNav.so"))
            self.write(f"{top}/{name}", data, 0o755 if executable else 0o644)
        return f"/{top}"

    def snapshot(self, skip=("tmp", "test", "proc", "dev")):
        """Every file and folder of the unit with its content's digest and mode, but the scratch folders."""
        state = {}
        for path in sorted(self.root.rglob("*")):
            relative = path.relative_to(self.root).as_posix()
            if relative.split("/", 1)[0] in skip or relative.startswith(TOOLS):
                continue
            if path.is_symlink():
                state[relative] = "link"
            elif path.is_dir():
                state[relative] = "dir"
            else:
                state[relative] = (md5(path.read_bytes()), path.stat().st_mode & 0o777)
        return state

    def run(self, *args, script="/tmp/aa-entrynav-2.0.0/setup.sh"):
        for counter in self.path("test").glob("count.*"):
            counter.unlink()
        hit = self.path("test/fault-hit")
        if hit.exists():
            hit.unlink()
        mount_log = self.path("test/mount.log")
        if mount_log.exists():
            mount_log.unlink()
        command = [CHROOT, str(self.root), "/bin/sh", script, *args]
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env={"PATH": "/bin", "HOME": "/tmp"},
            start_new_session=True,
        )
        try:
            out, _ = process.communicate(timeout=120)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            raise Failure(f"setup.sh {' '.join(args)} hung") from None
        return process.returncode, out.decode(errors="replace")

    def fault_hit(self):
        return self.path("test/fault-hit").exists()

    def mounts_log(self):
        path = self.path("test/mount.log")
        return path.read_text() if path.exists() else ""

    def root_mode(self):
        for line in self.path("proc/mounts").read_text().splitlines():
            if line.startswith("/dev/root / "):
                return line.split()[3].split(",")[0]
        return "?"

    def hmi_loads(self, folder):
        """iDrive, running, with the app of that folder loaded."""
        self.write(f"proc/{HMI_PID}/maps", f"b6a00000-b6a40000 r-xp 00000000 b3:02 1234 /{folder}/libAAEntryNav.so\n")
        self.write("test/pids.HmiMain3D", f"{HMI_PID}\n")

    def restart(self):
        """The unit restarts: system read-only, nothing running, iDrive loads the app if its folder is there."""
        self.write("proc/mounts", MOUNTS)
        for name in (
            "pids.evo-trial",
            "pids.aa-entrynav",
            "fault",
            "mount-fail-rw",
            "mount-fail-ro",
            "running-from-call",
        ):
            path = self.path(f"test/{name}")
            if path.exists():
                path.unlink()
        for pid in ("600", "601"):
            shutil.rmtree(self.path(f"proc/{pid}"), ignore_errors=True)
        maps = self.path(f"proc/{HMI_PID}/maps")
        if maps.exists():
            maps.unlink()
        if self.path(f"{PLUGIN}/libAAEntryNav.so").exists():
            self.hmi_loads(PLUGIN)

    def installed_version(self):
        """The version iDrive would load, checked whole: None when no app folder, or a Failure when mixed."""
        plugin = self.path(PLUGIN)
        if not plugin.exists():
            return None
        check((plugin / ".installed-by-aa-entrynav").is_file(), "the app folder lacks its marker")
        version = self.path("opt/aa/VERSION").read_text().strip() if self.path("opt/aa/VERSION").exists() else None
        check(version is not None, "iDrive app present without /opt/aa")
        check(self.path("opt/aa/.installed-by-aa-entrynav").is_file(), "/opt/aa lacks its marker")
        for name, data in release(version).items():
            if name.startswith(("opt/aa/", PLUGIN + "/")):
                path = self.path(name)
                check(path.is_file() and path.read_bytes() == data, f"{name} is not that of version {version}")
        return version

    def expect_untouched(self, before, what):
        after = self.snapshot()
        changed = sorted(set(before.items()) ^ set(after.items()))
        check(not changed, f"{what}: changed {changed[:6]}")

    def expect_bmw_files(self):
        for name in BMW_FILES:
            check(self.pristine.get(name) == self.snapshot().get(name), f"BMW file {name} changed")
        extra = sorted(path.name for path in self.path("opt/hmi/data/ro").iterdir() if path.name not in DATA_RO)
        check(not extra, f"iDrive's data folder gained {extra}")

    def expect_clean_install(self, version, plugins_created="yes"):
        check(self.installed_version() == version, f"version {version} expected, found {self.installed_version()}")
        check(self.root_mode() == "ro", "system left writable")
        for name in WORKING:
            check(not self.path(name).exists(), f"{name} left behind")
        modes = {"opt/aa/aa-entrynav": 0o755, "opt/aa/evo-trial": 0o755, f"{PLUGIN}/libAAEntryNav.so": 0o755}
        modes.update({"opt/aa/setup.sh": 0o644, f"{PLUGIN}/AAEntryNav.xml": 0o644})
        for name, mode in modes.items():
            check(self.path(name).stat().st_mode & 0o777 == mode, f"{name}: mode {oct(mode)} expected")
        record = self.path("var/local/data/AA/install-record").read_text()
        check(f"version={version}" in record, "install record lacks the version")
        created = f"plugins_folder_created={plugins_created}"
        check(created in record, f"{created} expected")
        self.expect_bmw_files()


def expect(result, code, *texts):
    status, out = result
    if code == 0:
        check(status == 0, f"exit 0 expected, got {status}:\n{out}")
    else:
        check(status != 0, f"a failure expected, got exit 0:\n{out}")
    for text in texts:
        check(text in out, f"{text!r} missing from:\n{out}")
    return out


# ---- the cases -------------------------------------------------------------------------------------------------------
def case_check_writes_nothing(unit):
    unit.unpack("2.0.0")
    before = unit.snapshot()
    expect(unit.run("check"), 0, "Every check passed", "Nothing was written")
    unit.expect_untouched(before, "check")
    check(unit.mounts_log() == "", "check remounted the system")


def case_fresh_install(unit):
    unit.unpack("2.0.0")
    expect(unit.run("install"), 0, "Installed and checked: version 2.0.0", "System partition back to read-only")
    unit.expect_clean_install("2.0.0")
    check(unit.mounts_log() == "-o remount,rw /\n-o remount,ro /\n", f"mounts: {unit.mounts_log()!r}")
    expect(unit.run("status", script="/opt/aa/setup.sh"), 0, "version 2.0.0", "all match the checksums")
    # Later damage to an installed file shows in status, which writes nothing.
    unit.write(f"{PLUGIN}/AAEntryNav.xml", "<damaged/>")
    before = unit.snapshot()
    expect(unit.run("status", script="/opt/aa/setup.sh"), 0, "DIFFER", "AAEntryNav.xml")
    unit.expect_untouched(before, "status")
    check(unit.mounts_log() == "", "status remounted the system")


def case_update_while_idrive_holds_the_app(unit):
    unit.unpack("1.0.0")
    expect(unit.run("install", script="/tmp/aa-entrynav-1.0.0/setup.sh"), 0)
    unit.restart()
    unit.write("var/local/data/AA/app-guard", "1\n2\n3\n")
    unit.unpack("2.0.0")
    out = expect(unit.run("install"), 0, "Installed and checked: version 2.0.0", "until the restart")
    check("crash guard counted" in out, "the tripped crash guard is not reported")
    check(not unit.path("var/local/data/AA/app-guard").exists(), "crash guard not reset for the new version")
    check(unit.installed_version() == "2.0.0", "new version not in place")
    check(unit.path("opt/.aa-entrynav-plugin.old").is_dir(), "the app in use was deleted")
    check(unit.root_mode() == "ro", "system left writable")
    # A second update before the restart: the earlier app is still loaded, so nothing is done.
    before = unit.snapshot()
    expect(unit.run("install"), 1, "restart the unit")
    unit.expect_untouched(before, "install while an earlier version is loaded")
    unit.restart()
    expect(unit.run("tidy", script="/opt/aa/setup.sh"), 0, "Tidied")
    unit.expect_clean_install("2.0.0")


def case_uninstall_in_two_steps(unit):
    unit.unpack("2.0.0")
    expect(unit.run("install"), 0)
    unit.restart()
    unit.write("var/local/data/AA/autostart", "1\n")
    out = expect(unit.run("uninstall", "--purge", script="/opt/aa/setup.sh"), 0, "restart the unit (reboot)")
    check("sh /opt/aa/setup.sh uninstall --purge" in out, "second step not given")
    check(not unit.path(PLUGIN).exists(), "the app is still where iDrive loads it")
    check(not unit.path("opt/hmi/data/ro/Plugins").exists(), "the Plugins folder this project made is still there")
    check(unit.root_mode() == "ro", "system left writable")
    unit.restart()
    expect(unit.run("uninstall", "--purge", script="/opt/aa/setup.sh"), 0, "AA-EntryNav is uninstalled")
    unit.expect_untouched(unit.pristine, "full uninstall")
    check(unit.root_mode() == "ro", "system left writable")


def case_uninstall_keeps_a_plugins_folder_it_did_not_make(unit):
    unit.write("opt/hmi/data/ro/Plugins/Other/Other.manifest", "<plugin/>\n")
    unit.pristine = unit.snapshot()
    unit.unpack("2.0.0")
    expect(unit.run("install"), 0)
    unit.expect_clean_install("2.0.0", plugins_created="no")
    expect(unit.run("uninstall", script="/opt/aa/setup.sh"), 0, "AA-EntryNav is uninstalled", "Settings kept")
    check(unit.path("opt/hmi/data/ro/Plugins/Other/Other.manifest").is_file(), "another app was touched")
    check(not unit.path("opt/aa").exists() and not unit.path(PLUGIN).exists(), "not removed")
    check(unit.path("var/local/data/AA").is_dir(), "settings deleted without --purge")


def case_uninstall_while_something_holds_the_programs(holder):
    """A program with a file of /opt/aa open, or a shell standing in it: the folder is kept until the restart."""

    def case(unit):
        unit.unpack("2.0.0")
        expect(unit.run("install"), 0)
        if holder == "open file":
            unit.path("proc/600/fd").mkdir(parents=True)
            unit.path("proc/600/fd/5").symlink_to("/opt/.aa-entrynav-aa.old/aa-entrynav")  # as the kernel names it then
        else:
            unit.path("proc/601").mkdir(parents=True)
            unit.path("proc/601/cwd").symlink_to("/opt/.aa-entrynav-aa.old")
        expect(unit.run("uninstall", script="/opt/aa/setup.sh"), 0, "Kept until a restart", "restart the unit (reboot)")
        check(unit.path("opt/.aa-entrynav-aa.old/aa-entrynav").is_file(), "a folder in use was deleted")
        check(not unit.path(PLUGIN).exists(), "the app is still where iDrive loads it")
        check(unit.root_mode() == "ro", "system left writable")
        unit.restart()
        expect(unit.run("uninstall", "--purge"), 0, "AA-EntryNav is uninstalled")
        unit.expect_untouched(unit.pristine, "uninstall after the restart")

    return case


def case_never_renames_into_an_existing_folder(unit):
    """A leftover that cannot be deleted keeps its name taken: the app folder stays where it is, whole."""
    prepare_update(unit)
    expect(unit.run("install"), 0)
    unit.restart()  # the previous app is still in its working folder, no longer in use
    unit.write("test/fault", "rm 1 fail\n")  # deleting that leftover fails
    expect(unit.run("uninstall", script="/opt/aa/setup.sh"), 1, "could not be moved")
    check(unit.installed_version() == "2.0.0", "the app folder was moved although its new name was taken")
    check(
        not unit.path("opt/.aa-entrynav-plugin.old/AAEntryNav").exists(), "the app folder was moved into the leftover"
    )
    check(unit.root_mode() == "ro", "system left writable")


def case_android_auto_starts_during_the_installation(unit):
    unit.unpack("2.0.0")
    unit.write("test/running-from-call", "2\n")  # stopped at the checks, running just before the switch
    before = unit.snapshot()
    expect(unit.run("install"), 1, "Android Auto started meanwhile")
    unit.expect_untouched(before, "an installation given up")
    check(unit.root_mode() == "ro", "system left writable")


def case_other_filesystem_warns(unit):
    unit.write("proc/mounts", MOUNTS.replace("/ ext4 ro", "/ ext2 ro"))
    unit.unpack("2.0.0")
    expect(unit.run("check"), 0, "not known to keep a rename whole")


def plugins_link_elsewhere(unit):
    unit.path("var/local/plugins").mkdir(parents=True)
    unit.path("opt/hmi/data/ro/Plugins").symlink_to("/var/local/plugins")


def programs_link(unit):
    unit.write("opt/elsewhere/.installed-by-aa-entrynav", "")
    unit.path("opt/aa").symlink_to("/opt/elsewhere")


def refusal(setup, *texts, args=("install",)):
    """A case where install stops before writing anything."""

    def case(unit):
        unit.unpack("2.0.0")
        setup(unit)
        before = unit.snapshot()
        expect(unit.run(*args), 1, *texts)
        unit.expect_untouched(before, "a refused installation")
        check(unit.mounts_log() == "", "the system was remounted although a check failed")

    return case


def write(name, data):
    return lambda unit: unit.write(name, data)


def damage(unit):
    unit.write(f"tmp/aa-entrynav-2.0.0/{PLUGIN}/AAEntryNav.xml", "<broken")


def other_mount(unit):
    unit.write("proc/mounts", MOUNTS + "/dev/mmcblk0p3 /opt/hmi ext4 ro 0 0\n")


def foreign_folder(name):
    return lambda unit: unit.write(f"{name}/something", "not ours\n")


def case_other_software_accepted_on_request(unit):
    unit.write("etc/version", "202203011200\n")
    unit.unpack("2.0.0")
    expect(unit.run("check", "--other-software"), 0, "not tried yet: accepted")
    expect(unit.run("install", "--other-software"), 0, "Installed and checked")


def case_wrapped_df_line(unit):
    unit.write("test/df-wrap", "")
    unit.unpack("2.0.0")
    expect(unit.run("check"), 0, "free space: 500000 KiB available")


def case_mount_rw_refused(unit):
    unit.unpack("2.0.0")
    unit.write("test/mount-fail-rw", "")
    before = unit.snapshot()
    out = expect(unit.run("install"), 1, "could not be made writable")
    unit.expect_untouched(before, "a refused remount")
    check("Installed" not in out, "claims an installation")


def case_mount_ro_refused(unit):
    unit.unpack("2.0.0")
    unit.write("test/mount-fail-ro", "")
    expect(unit.run("install"), 1, "still writable", "reboot")
    unit.restart()
    unit.expect_clean_install("2.0.0")


def case_bad_arguments(unit):
    unit.unpack("2.0.0")
    expect(unit.run("install", "--purge"), 1, "Usage")
    expect(unit.run("frobnicate"), 1, "Usage")
    expect(unit.run(), 1, "Usage")


def faults(prepare, command, target_after, script="/tmp/aa-entrynav-2.0.0/setup.sh"):
    """Stops each mv, cp and rm call of the command in turn, then restarts and runs the command again."""

    def case(unit, base, busybox):
        tried = 0
        for name in ("mv", "cp", "rm"):
            for action in ("kill", "fail"):
                call = 1
                while True:
                    unit = Unit(base, busybox)
                    prepare(unit)
                    unit.write("test/fault", f"{name} {call} {action}\n")
                    unit.run(*command, script=script)
                    if not unit.fault_hit():
                        break
                    tried += 1
                    where = f"{' '.join(command)}, {name} call {call} {action}"
                    if action == "fail":
                        check(unit.root_mode() == "ro", f"{where}: system left writable")
                    unit.restart()
                    try:
                        version = unit.installed_version()  # whole, or Failure
                    except Failure as error:
                        raise Failure(f"{where}: {error}") from None
                    record = unit.path("var/local/data/AA/install-record")
                    if action == "fail" and version and record.exists():
                        text = record.read_text()
                        check(f"version={version}\n" in text, f"{where}: the record names another version")
                    unit.expect_bmw_files()
                    # Then as the messages say: run it again, restart, tidy, restart (twice for a two-step uninstall).
                    out = ""
                    for _ in range(3):
                        again = script if unit.path(script[1:]).exists() else "/tmp/aa-entrynav-2.0.0/setup.sh"
                        out = unit.run(*command, script=again)[1]
                        unit.restart()
                        if unit.path("opt/aa/setup.sh").exists():
                            unit.run("tidy", script="/opt/aa/setup.sh")
                            unit.restart()
                        if target_after(unit, check_only=True):
                            break
                    try:
                        target_after(unit)
                    except Failure as error:
                        raise Failure(f"{where}: not finished by running again: {error}\n{out}") from None
                    call += 1
                    shutil.rmtree(unit.root)
                shutil.rmtree(unit.root)
        return tried

    return case


def installed_2(unit, check_only=False):
    try:
        unit.expect_clean_install("2.0.0", plugins_created="yes")
        return True
    except Failure:
        if not check_only:
            raise
        return False


def uninstalled(unit, check_only=False):
    leftovers = [name for name in ("opt/aa", PLUGIN, "opt/hmi/data/ro/Plugins", *WORKING) if unit.path(name).exists()]
    done = not leftovers and unit.root_mode() == "ro"
    if not done and not check_only:
        raise Failure(f"still there: {leftovers}, system {unit.root_mode()}")
    unit.expect_bmw_files()
    return done


def backup_case(tar_excludes):
    """tools/backup_headunit.sh's part on the car: reads only, leaves out pseudo file systems and the data partition."""

    def case(unit):
        unit.unpack("2.0.0")
        expect(unit.run("install"), 0)
        unit.write("proc/mounts", MOUNTS + PSEUDO_MOUNTS)
        unit.write("var/local/data/AA/vehicle", "id=personal\n")
        unit.write("var/lib/keep.txt", "system\n")
        unit.write("opt/hmi/run/volatile", "memory only\n")
        if not tar_excludes:
            unit.path("bin/tar").unlink()
            unit.write("bin/tar", '#!/bin/sh\ncase " $* " in *" -X "*) exit 1 ;; esac\nexec busybox tar "$@"\n', 0o755)
        unit.path("test/mount.log").unlink()  # the installation's
        before = unit.snapshot()
        script = subprocess.run(
            ["sh", str(ROOT / "tools/backup_headunit.sh"), "--print-remote"], capture_output=True, check=True
        ).stdout
        result = subprocess.run(
            [CHROOT, str(unit.root), "/bin/sh", "-s"], input=script, capture_output=True, env={"PATH": "/bin"}
        )
        check(result.returncode == 0, f"backup failed: {result.stderr.decode(errors='replace')}")
        with tarfile.open(fileobj=io.BytesIO(result.stdout)) as archive:
            names = set(archive.getnames())
        for name in (
            "etc/version",
            "opt/hmi/lib/libAppFramework.A.so",
            f"{PLUGIN}/libAAEntryNav.so",
            "var/lib/keep.txt",
        ):
            check(name in names, f"{name} missing from the copy")
        for name in ("tmp/aa-entrynav-backup/system.txt", "tmp/aa-entrynav-backup/mounts.txt"):
            check(name in names, f"{name} missing from the copy")
        check(not any(n.startswith("var/local") for n in names), "personal data (/var/local) copied")
        check(not any(n.startswith(("proc/", "dev/")) for n in names), "pseudo file systems copied")
        if tar_excludes:
            check("opt/hmi/run/volatile" not in names, "a memory file system inside /opt copied")
        else:
            check(b"cannot leave folders out" in result.stderr, "no warning without tar -X")
        unit.expect_untouched(before, "the backup")
        check(not unit.path("tmp/aa-entrynav-backup").exists(), "facts left in /tmp")
        check(unit.mounts_log() == "", "the backup remounted the system")

    return case


def prepare_fresh(unit):
    unit.unpack("2.0.0")


def prepare_update(unit):
    unit.unpack("1.0.0")
    unit.run("install", script="/tmp/aa-entrynav-1.0.0/setup.sh")
    unit.restart()
    unit.unpack("2.0.0")


def prepare_uninstall(unit):
    prepare_update(unit)
    unit.run("install")
    unit.restart()
    unit.run("tidy", script="/opt/aa/setup.sh")


def prepare_uninstall_second_step(unit):
    prepare_uninstall(unit)
    unit.run("uninstall", script="/opt/aa/setup.sh")
    unit.restart()


def case_version_order(unit):
    """version_key and version_order, run by the unit's shell: the order of tools/release_version.py, a stable build
    above its beta and test builds, and malformed versions refused."""
    text = SETUP.read_text()
    functions = text[text.index("version_key() {") : text.index("# The archive's version:")]
    ordered = ["0.1.3", "0.1.4-test.1", "0.1.4-test.2", "0.1.4-test.10", "0.1.4-beta.1", "0.1.4", "0.1.10", "1.0.0"]
    pairs = list(zip(ordered, ordered[1:]))
    bad = ["2.0", "2.0.0.1", "v2.0.0", "02.0.0", "2.0.0-rc.1", "2.0.0-beta", "2.0.0-beta.0", "2.0.0-beta.1.2", ""]
    lines = [functions]
    for lower, higher in pairs:
        lines.append(f'echo "{lower} {higher} $(version_order {lower} {higher}) $(version_order {higher} {lower})"')
    lines.append('echo "same $(version_order 0.1.4-beta.2 0.1.4-beta.2)"')
    for version in bad:
        lines.append(f"version_key '{version}' >/dev/null && echo 'accepted {version}' || echo 'refused {version}'")
    unit.write("tmp/order.sh", "\n".join(lines) + "\n")
    out = expect(unit.run(script="/tmp/order.sh"), 0)
    for lower, higher in pairs:
        check(f"{lower} {higher} older newer" in out, f"{lower} not below {higher}:\n{out}")
    check("same same" in out, f"a version is not the same as itself:\n{out}")
    for version in bad:
        check(f"refused {version}\n" in out, f"{version!r} not refused:\n{out}")


def case_versions_reported(unit):
    """check and install name a beta or test build, and say whether they update, reinstall or go back."""
    beta = unit.unpack("2.0.0-beta.1") + "/setup.sh"
    out = expect(unit.run("install", script=beta), 0, "2.0.0-beta.1 is a beta build", "version 2.0.0-beta.1")
    check("updates the installed" not in out, "a fresh install reported an update")
    unit.restart()
    unit.unpack("2.0.0")
    expect(unit.run("check"), 0, "updates the installed version, 2.0.0-beta.1", "Every check passed")
    test = unit.unpack("2.0.0-test.3") + "/setup.sh"
    expect(unit.run("check", script=test), 0, "is a test build", "goes back from the installed version, 2.0.0-beta.1")
    expect(unit.run("check", script=beta), 0, "installs 2.0.0-beta.1 again")
    expect(
        unit.run("install"), 0, "updates the installed version, 2.0.0-beta.1", "Installed and checked: version 2.0.0"
    )


def case_malformed_version_refused(unit):
    for version in ("2.0", "2.0.0-rc.1", "v2.0.0"):
        script = unit.unpack(version) + "/setup.sh"
        before = unit.snapshot()
        expect(unit.run("install", script=script), 1, f"'{version}', is not X.Y.Z")
        unit.expect_untouched(before, f"an archive of version {version}")


CASES = {
    "version order": case_version_order,
    "versions reported": case_versions_reported,
    "malformed version refused": case_malformed_version_refused,
    "check writes nothing": case_check_writes_nothing,
    "fresh install": case_fresh_install,
    "update while iDrive holds the app": case_update_while_idrive_holds_the_app,
    "uninstall in two steps": case_uninstall_in_two_steps,
    "uninstall keeps a Plugins folder it did not make": case_uninstall_keeps_a_plugins_folder_it_did_not_make,
    "other software, accepted on request": case_other_software_accepted_on_request,
    "df with a wrapped line": case_wrapped_df_line,
    "remount rw refused": case_mount_rw_refused,
    "remount ro refused": case_mount_ro_refused,
    "bad arguments": case_bad_arguments,
    "uninstall while a program holds a file": case_uninstall_while_something_holds_the_programs("open file"),
    "uninstall while a shell stands in /opt/aa": case_uninstall_while_something_holds_the_programs("shell"),
    "Android Auto starts during the installation": case_android_auto_starts_during_the_installation,
    "never renames into an existing folder": case_never_renames_into_an_existing_folder,
    "another file system warns": case_other_filesystem_warns,
    "refused: Plugins links to another partition": refusal(plugins_link_elsewhere, "on another partition"),
    "refused: /opt/aa is a link": refusal(programs_link, "symbolic link"),
    "refused: a blocking compatibility point": refusal(write("test/compat", COMPAT_OK + "@block|video|\n"), "video"),
    "refused: rehearsal unfinished": refusal(write("test/compat", "@ok|abi|\n"), "did not finish"),
    "refused: missing function": refusal(
        write("test/ld-output", "symbol lookup: undefined symbol: _ZN3bmw4Foo (./x)\n"), "missing on this car"
    ),
    "refused: other software": refusal(write("etc/version", "202203011200\n"), "not tried yet"),
    "refused: system already writable": refusal(
        write("proc/mounts", MOUNTS.replace("ext4 ro,", "ext4 rw,")), "writable right now"
    ),
    "refused: /opt/aa not ours": refusal(foreign_folder("opt/aa"), "/opt/aa exists but was not installed"),
    "refused: app folder not ours": refusal(foreign_folder(PLUGIN), "AAEntryNav exists but was not installed"),
    "refused: Android Auto running": refusal(write("test/pids.evo-trial", "77\n"), "Android Auto is running"),
    "refused: damaged archive": refusal(damage, "archive damaged"),
    "refused: low space": refusal(write("test/df-avail", "16000\n"), "not enough free space"),
    "refused: another partition": refusal(other_mount, "on another partition"),
    "refused: check reports too": refusal(write("test/compat", "@block|abi|size 168\n"), "abi", args=("check",)),
    "backup reads only": backup_case(tar_excludes=True),
    "backup with a tar that cannot exclude": backup_case(tar_excludes=False),
}
FAULT_CASES = {
    "faults during a fresh install": faults(prepare_fresh, ("install",), installed_2),
    "faults during an update": faults(prepare_update, ("install",), installed_2),
    "faults during an uninstall": faults(prepare_uninstall, ("uninstall",), uninstalled, script="/opt/aa/setup.sh"),
    "faults during an uninstall's second step": faults(
        prepare_uninstall_second_step, ("uninstall", "--purge"), uninstalled, script="/opt/aa/setup.sh"
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--busybox", default=shutil.which("busybox"), help="BusyBox binary")
    parser.add_argument("--quick", action="store_true", help="skip the fault cases")
    options = parser.parse_args()
    if os.geteuid() != 0 or not Path(CHROOT).is_file():
        print("SKIP test_setup: needs root and chroot (CI's container, or WSL as root)")
        return 0
    if not options.busybox or not Path(options.busybox).is_file():
        print("SKIP test_setup: no BusyBox (--busybox $(sh tests/build_busybox.sh))")
        return 0
    version = subprocess.run([options.busybox], capture_output=True, text=True).stdout.splitlines()[0]
    print(f"test_setup with {version}")
    unfit = unfit_busybox(options.busybox)
    if unfit:
        print(f"FAIL test_setup: this BusyBox cannot stand in for the car's: {unfit}. Build one that can with")
        print("    make test-host BUSYBOX=$(sh tests/build_busybox.sh)")
        return 1
    failures = 0
    base = tempfile.mkdtemp(prefix="aa-entrynav-setup-")
    try:
        for name, case in CASES.items():
            unit = Unit(base, options.busybox)
            try:
                case(unit)
                print(f"PASS setup: {name}")
            except Failure as error:
                failures += 1
                print(f"FAIL setup: {name}: {error}")
            shutil.rmtree(unit.root, ignore_errors=True)
        if not options.quick:
            for name, case in FAULT_CASES.items():
                try:
                    tried = case(None, base, options.busybox)
                    if not tried:
                        raise Failure("no interruption happened: the fake mv, cp and rm never ran")
                    print(f"PASS setup: {name} ({tried} interruptions)")
                except Failure as error:
                    failures += 1
                    print(f"FAIL setup: {name}: {error}")
    finally:
        shutil.rmtree(base, ignore_errors=True)
    if failures:
        print(f"test_setup: {failures} failure(s)")
        return 1
    print("PASS test_setup")
    return 0


if __name__ == "__main__":
    sys.exit(main())
