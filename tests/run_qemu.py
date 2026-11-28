# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Run the ARM regression checks under QEMU, with the libraries of the copy of the car's system. Nothing else.

Build them first: bash toolchain/tc.sh make all test-arm-all, and python3 tools/build_rust.py for the Rust ports in
build/rust. Logs and a summary go to artifacts/validation-pc. The cases run side by side, QEMU_JOBS at once (default:
three per processor): most of their time is spent waiting.
"""

import contextlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SYSROOT = Path.home() / "entrynav2/rootfs"
QEMU = [
    "qemu-arm",
    "-L",
    str(SYSROOT),
    "-E",
    "LD_LIBRARY_PATH=/opt/hmi/lib:/usr/local/lib:/usr/lib:/lib",
    "-E",
    "LD_BIND_NOW=1",
]
# Name, program (from the repository root), arguments, and what its output must hold (None: a PASS line or " OK").
CASES = [
    ("pb", "tests/test-pb-arm", [], None),
    ("sdr-g20", "tests/test-sdr-g20-arm", [], None),
    ("elf", "tests/test-evo-elf-arm", [], None),
    ("spp-snapshot", "tests/test-spp-snapshot-arm", [], None),
    ("kombi-phone", "tests/test-kombi-phone-arm", [], None),
    ("kombi-display", "tests/test-kombi-display-arm", [], None),
    ("kombi-graph", "tests/test-kombi-graph-arm", [], None),
    ("kombi-guide", "tests/test-kombi-guide-arm", [], None),
    ("kombi-client", "tests/test-kombi-client-arm", [], None),
    ("kombi-nav", "tests/test-kombi-nav-arm", [], None),
    ("kombi-golden", "tests/test-kombi-golden-arm", [], None),
    ("session-silence", "tests/test-session-silence-arm", [], None),
    ("gps-native", "tests/test-evo-gps-arm", [], None),
    ("video-presentation", "tests/test-video-presentation-arm", [], None),
    ("input", "tests/test-evo-input-arm", [], None),
    ("supervisor", "tests/test-trial-recovery-arm", [], None),
    ("wireless-lifecycle", "tests/test-wireless-lifecycle-arm", [], None),
    ("session-phone", "tests/test-session-phone-arm", [str(ROOT / "tests/fixtures/session_phone.golden")], None),
    ("wireless-phone", "tests/test-wireless-phone-arm", [str(ROOT / "tests/fixtures/wireless_phone.golden")], None),
    ("hmi-load", "tests/test-hmiapp-load-arm", [str(ROOT / "hmiapp/libAAEntryNav.so")], None),
    ("hold-machine", "tests/test-hold-machine-arm", [], None),
    ("control", "tests/test-evo-control-arm", [], None),
    ("spawn-glibc", "tests/test-spawn-attr-arm", ["--hu-glibc"], None),
    # The developer tools: the recorder's parser on 32 bits, and evo-check's loading with every symbol bound.
    ("capture-selftest", "evo-capture", ["--selftest"], None),
    ("check-load", "evo-check", ["--help"], "--dbus-watch"),
    ("ric-load", "evo-ric", ["--help"], "--watch"),
    ("mfl-help", "evo-mfl", ["--help"], "read-only steering-wheel capture"),
    # glibc Rust (docs/rust-port.md, step 2): std and every car library the service uses, from a Rust staticlib that
    # GCC 4.7 linked against the copy of the car's system. tools/verify_binary.py checks its glibc versions.
    ("rsprobe", "evo-rsprobe", [], "PASS evo-rsprobe: "),
]
# The Rust ports (docs/rust-port.md), from build/rust. Each must be a static ARM program, and each case runs it beside
# the C++ program it replaces: the two outputs and exit codes must be the same, and the output must also be the fixture
# recorded of the C++. Then every Rust test program in build/rust/tests runs here, on the car's 32-bit ARM.
RUST = ROOT / "build/rust"
RUST_CASES = {
    # Rust program: (arguments, the C++ program, the fixture its output must equal)
    "evo-capture": (["--selftest"], "evo-capture", "tests/fixtures/evo_capture/selftest.golden"),
    "evo-ric": (["--help"], "evo-ric", "tests/fixtures/evo_ric/usage.golden"),
    "evo-check": (["--help"], "evo-check", "tests/fixtures/evo_check/usage.golden"),
}
# The D-Bus tools also run side by side on a private bus served by the car's own dbus-daemon (1.6) under QEMU:
# BUS_CASES below. Its policy lets every connection own any name and eavesdrop, as the car's session bus does.
BUS_CONFIG = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path={socket}</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""
PT_DYNAMIC, PT_INTERP, EM_ARM = 2, 3, 40
EF_ARM_EABI_VER5, EF_ARM_ABI_FLOAT_HARD = 0x05000000, 0x400


def static_arm(path):
    """Why path is not a static ARM EABI 5 hard-float executable, or None."""
    data = path.read_bytes()
    if data[:7] != b"\x7fELF\x01\x01\x01":
        return "not a 32-bit little-endian ELF"
    (machine,) = struct.unpack_from("<H", data, 18)
    (flags,) = struct.unpack_from("<I", data, 36)
    if machine != EM_ARM or flags & 0xFF000000 != EF_ARM_EABI_VER5 or not flags & EF_ARM_ABI_FLOAT_HARD:
        return f"machine {machine}, flags {flags:#x}: not ARM EABI 5 hard-float"
    (phoff,) = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    for i in range(phnum):
        (kind,) = struct.unpack_from("<I", data, phoff + i * phentsize)
        if kind in (PT_INTERP, PT_DYNAMIC):
            return "dynamically linked: it would need the car's loader and C library"
    return None


def run_rust(out, report):
    """The Rust ports against the C++ programs and their fixtures, then the Rust tests built for ARM."""
    if not (RUST / "tests").is_dir():
        raise SystemExit("FAIL ARM/QEMU rust: no build/rust. Build it with python3 tools/build_rust.py (make rust)")
    for name, (args, original, fixture) in RUST_CASES.items():
        program = RUST / name
        why = static_arm(program) if program.is_file() else "missing"
        if why:
            raise SystemExit(f"FAIL ARM/QEMU rust-{name}: build/rust/{name} {why}")
        if not static_arm(ROOT / original):
            raise SystemExit(f"FAIL ARM/QEMU rust-{name}: {original} is the Rust copy (RUST_TOOLS); build the C++ one")
        rust, cpp = (
            subprocess.run([*QEMU, str(path), *args], cwd=ROOT, capture_output=True, text=True, timeout=60)
            for path in (program, ROOT / original)
        )
        (out / f"rust-{name}.log").write_text(f"--- Rust\n{rust.stdout}{rust.stderr}--- C++\n{cpp.stdout}{cpp.stderr}")
        expected = (ROOT / fixture).read_text(encoding="utf-8")
        if rust.returncode != cpp.returncode or rust.stdout != cpp.stdout or rust.stdout != expected:
            raise SystemExit(
                f"FAIL ARM/QEMU rust-{name}: exit {rust.returncode} against the C++'s {cpp.returncode}, output "
                f"{'the same as' if rust.stdout == cpp.stdout else 'unlike'} the C++'s, "
                f"{'the same as' if rust.stdout == expected else 'unlike'} {fixture}\n{rust.stdout}{rust.stderr}"
            )
        report["tests"][f"rust-{name}"] = {"result": "PASS", "binary_sha256": sha256(program.read_bytes()).hexdigest()}
        print(f"PASS ARM/QEMU rust-{name}: the same as the C++ {original} and {fixture}", flush=True)
    with tempfile.TemporaryDirectory(prefix="evo-bus-") as folder:
        for case in BUS_CASES:
            case(Path(folder), out, report)
    tests = sorted((RUST / "tests").iterdir())
    if not tests:
        raise SystemExit("FAIL ARM/QEMU rust-tests: build/rust/tests is empty")
    for test in tests:
        result = subprocess.run([*QEMU, str(test)], cwd=ROOT, capture_output=True, text=True, timeout=120)
        (out / f"rust-test-{test.name}.log").write_text(result.stdout + result.stderr)
        if result.returncode != 0 or "test result: ok" not in result.stdout:
            raise SystemExit(f"FAIL ARM/QEMU rust-test {test.name} (exit {result.returncode})\n{result.stdout}")
        report["tests"][f"rust-test-{test.name}"] = {"result": "PASS"}
        print(f"PASS ARM/QEMU rust-test {test.name}", flush=True)


@contextlib.contextmanager
def private_bus(folder, name):
    """The car's dbus-daemon under QEMU on a socket in folder: yields (the environment pointing both buses at it, the
    daemon's process). Stopped on the way out."""
    work = folder / name
    work.mkdir()
    socket = work / "bus"
    (work / "bus.conf").write_text(BUS_CONFIG.format(socket=socket))
    daemon = subprocess.Popen(
        [*QEMU, str(SYSROOT / "bin/dbus-daemon"), f"--config-file={work / 'bus.conf'}", "--nofork"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        begin = time.monotonic()
        while not socket.exists():
            if daemon.poll() is not None or time.monotonic() - begin > 20:
                raise SystemExit(f"FAIL ARM/QEMU {name}: the car's dbus-daemon did not start\n{daemon.stderr.read()}")
            time.sleep(0.05)
        address = f"unix:path={socket}"
        yield {**os.environ, "DBUS_SESSION_BUS_ADDRESS": address, "DBUS_SYSTEM_BUS_ADDRESS": address}, daemon
    finally:
        if daemon.poll() is None:
            daemon.terminate()
            daemon.wait(timeout=20)


def run_pair(name, args, env, timeout=60):
    """The Rust program of build/rust and the C++ one of the repository root, one after the other."""
    return [
        subprocess.run([*QEMU, str(path), *args], cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout)
        for path in (RUST / name, ROOT / name)
    ]


def same_or_fail(case, rust, cpp, out, fixture=None, normalize=lambda text: text):
    """Fails case unless the two runs gave the same exit code and normalized output, and the fixture's text if any."""
    got, expected = normalize(rust.stdout), normalize(cpp.stdout)
    (out / f"rust-{case}.log").write_text(f"--- Rust\n{rust.stdout}{rust.stderr}--- C++\n{cpp.stdout}{cpp.stderr}")
    recorded = (ROOT / fixture).read_text(encoding="utf-8") if fixture else expected
    if rust.returncode != cpp.returncode or got != expected or got != recorded:
        raise SystemExit(
            f"FAIL ARM/QEMU rust-{case}: exit {rust.returncode} against the C++'s {cpp.returncode}, output "
            f"{'the same as' if got == expected else 'unlike'} the C++'s"
            + (f", {'the same as' if got == recorded else 'unlike'} {fixture}" if fixture else "")
            + f"\n--- Rust\n{rust.stdout}{rust.stderr}--- C++\n{cpp.stdout}{cpp.stderr}"
        )


def ric_stdout(text):
    """evo-ric's output with the path of its log, which differs between runs, written FILE."""
    return re.sub(r" recorded in \S+", " recorded in FILE", text)


def ric_log(text):
    """An evo-ric log without what differs between two recorders of the same traffic: each line's time, the header
    with the date and uptime, the heartbeats, and how long the watch lasted."""
    lines = []
    for line in text.splitlines():
        if line.startswith(("# evo-ric watch:", "# alive ")):
            continue
        if line.startswith("# end after "):
            line = re.sub(r"after \d+ ms", "after N ms", line)
        elif not line.startswith("#"):
            line = line.split(" ", 1)[1]
        lines.append(line)
    return "\n".join(lines) + "\n"


def wait_for(condition, seconds, what):
    begin = time.monotonic()
    while not condition():
        if time.monotonic() - begin > seconds:
            raise SystemExit(f"FAIL ARM/QEMU {what}")
        time.sleep(0.1)


def ric_bus(folder, out, report):
    """evo-ric (docs/rust-port.md, step 3): both self tests, then both listing the names and recording the D-Bus corpus
    played on the bus, until the bus goes away."""
    with private_bus(folder, "ric-selftest") as (env, _):
        rust, cpp = run_pair("evo-ric", ["--selftest"], env)
        same_or_fail("evo-ric-selftest", rust, cpp, out, "tests/fixtures/evo_ric/selftest.golden", ric_stdout)
    with private_bus(folder, "ric-watch") as (env, daemon):
        player = subprocess.Popen(
            [
                *QEMU,
                str(RUST / "examples/ric_play"),
                env["DBUS_SESSION_BUS_ADDRESS"],
                "tests/fixtures/dbus/messages.bin",
            ],
            cwd=ROOT,
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )
        if player.stdout.readline().strip() != "ready":
            raise SystemExit("FAIL ARM/QEMU rust-evo-ric-watch: the player did not start")
        rust, cpp = run_pair("evo-ric", ["--names"], env)
        same_or_fail("evo-ric-names", rust, cpp, out)
        logs = {kind: folder / f"ric-{kind}" for kind in ("rust", "cpp")}
        watchers = {
            kind: subprocess.Popen(
                [*QEMU, str(path), "--watch", "5", "--out", str(logs[kind])],
                cwd=ROOT,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            for kind, path in (("rust", RUST / "evo-ric"), ("cpp", ROOT / "evo-ric"))
        }

        def log_text(kind):
            files = sorted(logs[kind].glob("ric-*.log"))
            return files[0].read_text(encoding="utf-8", errors="replace") if files else ""

        wait_for(lambda: all(log_text(k).count("# match ") == 8 for k in logs), 30, "evo-ric watchers: no rules")
        player.stdin.write("go\n")
        player.stdin.flush()
        played = player.communicate(timeout=300)[0]
        if "played" not in played:
            raise SystemExit(f"FAIL ARM/QEMU rust-evo-ric-watch: the player stopped\n{played}")
        time.sleep(1)
        daemon.terminate()  # the bus goes away: both recorders must see it and end
        daemon.wait(timeout=20)
        results = {kind: watcher.communicate(timeout=60) for kind, watcher in watchers.items()}
        runs = {
            kind: subprocess.CompletedProcess([], watchers[kind].returncode, ric_stdout(o) + ric_log(log_text(kind)), e)
            for kind, (o, e) in results.items()
        }
        same_or_fail("evo-ric-watch", runs["rust"], runs["cpp"], out)
        # Every RIC call the player sent recorded, and the bus's going seen.
        sent = re.search(r"played (\d+) messages, (\d+) RIC calls", played)
        calls = re.search(r"^evo-ric: (\d+) calls", runs["rust"].stdout, re.MULTILINE)
        if not sent or not calls or calls.group(1) != sent.group(2) or "# the bus closed" not in runs["rust"].stdout:
            raise SystemExit(
                f"FAIL ARM/QEMU rust-evo-ric-watch: the corpus was not recorded ({played.strip()})\n"
                f"{runs['rust'].stdout[-2000:]}"
            )
    report["tests"]["rust-evo-ric-bus"] = {"result": "PASS"}
    print(
        f"PASS ARM/QEMU rust-evo-ric-bus: self test, names and {sent.group(1)} played messages as the C++, "
        f"{sent.group(2)} RIC calls recorded by both",
        flush=True,
    )


def without_log_times(text):
    """Output with the uptime that starts each line of the service's log, and --dbus-watch's clock, left out."""
    return re.sub(r"^\[ *\d+\.\d{3}\] ", "", text, flags=re.MULTILINE)


def check_alone(folder, case, kind, args, orders=(), fake=True, timeout=60):
    """One evo-check, alone on a fresh private bus with the fake Alps service (rust/evo-check/examples/alps_fake.rs),
    so that both programs meet the same unique names. Each order is (seconds after the start, line for the fake)."""
    program = RUST / "evo-check" if kind == "rust" else ROOT / "evo-check"
    with private_bus(folder, f"{case}-{kind}") as (env, _):
        alps = None
        if fake:
            alps = subprocess.Popen(
                [*QEMU, str(RUST / "examples/alps_fake"), env["DBUS_SESSION_BUS_ADDRESS"]],
                cwd=ROOT,
                env=env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                text=True,
            )
            if alps.stdout.readline().strip() != "ready":
                raise SystemExit(f"FAIL ARM/QEMU rust-{case}: the fake Alps service did not start")
        run = subprocess.Popen(
            [*QEMU, str(program), *args], cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        begin = time.monotonic()
        for at, order in orders:
            time.sleep(max(0.0, begin + at - time.monotonic()))
            alps.stdin.write(order + "\n")
            alps.stdin.flush()
        stdout, stderr = run.communicate(timeout=timeout)
        if alps:
            alps.stdin.write("quit\n")
            alps.stdin.flush()
            alps.wait(timeout=20)
        return subprocess.CompletedProcess(args, run.returncode, stdout, stderr)


def check_bus(folder, out, report):
    """evo-check (docs/rust-port.md, step 4): its Bluetooth and D-Bus modes against the fake Alps service, each
    program alone on its own bus, and --scene against a stand-in LayerManagerControl."""
    cases = [
        ("check-no-alps", ["--bluetooth"], (), False),
        ("check-bluetooth", ["--bluetooth"], (), True),
        ("check-monitor", ["--monitor", "5"], ((2, "script"),), True),
        ("check-monitor-release", ["--monitor", "8"], ((2, "script"), (3.5, "release")), True),
        ("check-dbus-names", ["--dbus-names"], (), True),
        ("check-dbus-introspect", ["--dbus-introspect", "org.alps.evolution", "/org/alps/evolution/hli"], (), True),
        ("check-dbus-introspect-nobody", ["--dbus-introspect", "org.nobody.here", "/x"], (), True),
        ("check-dbus-watch", ["--dbus-watch", "/org/alps/evolution/hli", "5"], ((2, "script"),), True),
    ]
    for case, args, orders, fake in cases:
        rust, cpp = (check_alone(folder, case, kind, args, orders, fake) for kind in ("rust", "cpp"))
        # Each run has its own fake service, so its own process: the pid --dbus-names gives differs.
        same_or_fail(
            f"evo-{case}", rust, cpp, out, normalize=lambda t: re.sub(r"  pid \d+  ", "  pid N  ", without_log_times(t))
        )
    # The stand-in for LayerManagerControl, run by the host's sh: an executable copy, wherever the tree came from.
    lmc = folder / "fake-lmc.sh"
    shutil.copy(ROOT / "tests/fixtures/evo_check/fake-lmc.sh", lmc)
    lmc.chmod(0o755)
    rust, cpp = run_pair("evo-check", ["--scene", str(lmc)], os.environ)
    same_or_fail("evo-check-scene", rust, cpp, out)
    if "truncated=1" not in rust.stdout or "timed_out=1" not in rust.stdout or "signal=9" not in rust.stdout:
        raise SystemExit(f"FAIL ARM/QEMU rust-evo-check-scene: a failure the stand-in makes is missing\n{rust.stdout}")
    # A program that is not there: the car's glibc 2.15 reports it as a child that exited with 127.
    rust, cpp = run_pair("evo-check", ["--scene", "/nonexistent/LayerManagerControl"], os.environ)
    same_or_fail("evo-check-scene-missing", rust, cpp, out)
    if "code=127 signal=0 spawn_error=0" not in rust.stdout:
        raise SystemExit(f"FAIL ARM/QEMU rust-evo-check-scene-missing: not glibc 2.15's exit 127\n{rust.stdout}")
    report["tests"]["rust-evo-check-bus"] = {"result": "PASS"}
    print(
        f"PASS ARM/QEMU rust-evo-check-bus: {len(cases)} Bluetooth and D-Bus cases and --scene as the C++", flush=True
    )


BUS_CASES = [ric_bus, check_bus]


def passed(result, expected):
    """True when the program exited 0 and printed what its case expects."""
    if result.returncode != 0:
        return False
    if expected is not None:
        return expected in result.stdout
    return "PASS" in result.stdout or result.stdout.rstrip().endswith(" OK")


def main():
    out = ROOT / "artifacts/validation-pc"
    out.mkdir(parents=True, exist_ok=True)
    report = {"vehicle_connected": False, "visual_result": "NOT TESTED", "tests": {}}

    def run_case(case):
        program, args = case[1], case[2]
        return subprocess.run([*QEMU, str(ROOT / program), *args], cwd=ROOT, capture_output=True, text=True, timeout=60)

    jobs = int(os.environ.get("QEMU_JOBS") or 3 * (os.cpu_count() or 1))
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(run_case, CASES))
    for (name, program, _, expected), result in zip(CASES, results):
        path = ROOT / program
        (out / f"{name}.log").write_text(result.stdout + result.stderr)
        if not passed(result, expected):
            raise SystemExit(f"FAIL ARM/QEMU {name} (exit {result.returncode})\n{result.stdout}{result.stderr}")
        report["tests"][name] = {"result": "PASS", "binary_sha256": sha256(path.read_bytes()).hexdigest()}
        print("PASS ARM/QEMU " + name, flush=True)
    run_rust(out, report)
    # The car's own LayerManagerControl, with a stub in place of the compositor (tests/lm_cli_stub.c).
    result = subprocess.run([sys.executable, str(ROOT / "tests/test_real_lm_cli.py")], capture_output=True, text=True)
    (out / "lm-cli.log").write_text(result.stdout + result.stderr)
    if result.returncode != 0:
        raise SystemExit(f"FAIL ARM/QEMU lm-cli (exit {result.returncode})\n{result.stdout}{result.stderr}")
    report["tests"]["lm-cli"] = {"result": "PASS"}
    print("PASS ARM/QEMU lm-cli", flush=True)
    (out / "qemu.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
