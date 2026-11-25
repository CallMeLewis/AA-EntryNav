# Copyright (c) 2026 CallMeLewis
"""Check a Flare Drive diagnostic report taken after a test drive, before a build that changes hmiapp/ goes further.

0.1.9-test.1 and test.2 passed every check on the PC and under QEMU, yet iDrive died seconds after each connection in
the car. This reads what a drive leaves behind and fails on what that bug, or one like it, would leave:
  - HmiMain3D not running, the app's last start in another HmiMain3D, or more than one start since the head unit
    started (iDrive restarted by itself);
  - no connected session of at least ten minutes;
  - the service's "iDrive app silent" (platform/app_heartbeat.hpp) or any "thread guard:" line (hmi_affinity.hpp);
  - no gear or track lines from the app while connected;
  - with the cluster player (audio-wifi): no reading of iDrive's entertainment source on HmiMain3D's thread, a warning
    that readings stopped, or no switch away from Android Auto's source and back during the drive.

Take the report with logging on, after driving connected for ten minutes or more and switching once to the radio and
back to Android Auto, without restarting the head unit.
  python tools/smoke_check.py report.txt
"""

import re
import sys
from pathlib import Path

MIN_SESSION_S = 600
SECTION = re.compile(r"^===== (.*?) =====$")
MONO = re.compile(r"\[\s*(\d+(?:\.\d+)?)\]")


def sections(text):
    """{title: [lines]} of the report, a title's "(last 128 KiB)" left off."""
    found, current = {}, None
    for line in text.splitlines():
        match = SECTION.match(line)
        if match:
            current = re.sub(r" \(last .*\)$", "", match.group(1))
            found[current] = []
        elif current is not None:
            found[current].append(line)
    return found


def mono(line):
    """The monotonic seconds a log line carries, or None."""
    match = MONO.search(line)
    return float(match.group(1)) if match else None


def longest_session(service):
    """The longest stretch in seconds the service said "connected", up to its last timestamped line."""
    longest, since, last = 0.0, None, None
    for line in service:
        t = mono(line)
        if t is None:
            continue
        last = t
        if "[Control] state -> " in line:
            connected = "state -> connected" in line
            if connected and since is None:
                since = t
            elif not connected and since is not None:
                longest, since = max(longest, t - since), None
        elif "[ServiceApp] session ended" in line and since is not None:
            longest, since = max(longest, t - since), None
    if since is not None and last is not None:
        longest = max(longest, last - since)
    return longest


def check(text):
    """[(passed, message)] for a report's text."""
    report = sections(text)
    results = []

    def result(passed, message):
        results.append((passed, message))

    ids = "\n".join(report.get("RUNNING PROCESS IDS", []))
    pid_match = re.search(r"^HmiMain3D: (\d+)\s*$", ids, re.M)
    pid = pid_match.group(1) if pid_match else None
    result(pid is not None, f"HmiMain3D runs (pid {pid})" if pid else "HmiMain3D is not running")

    app = report.get("/var/local/data/AA/app.old.log", []) + report.get("/var/local/data/AA/app.log", [])
    creates = [i for i, line in enumerate(app) if "] create(name=" in line]
    if not creates:
        result(False, "the app log holds no start of the iDrive app: was logging on?")
        app_now = []
    else:
        start = app[creates[-1]]
        app_now = app[creates[-1] :]
        same = pid is not None and start.rstrip().endswith(f"HMI pid {pid}")
        result(
            same,
            "the iDrive app started in this HmiMain3D"
            if same
            else f"the app's last start was in another HmiMain3D: {start.strip()}",
        )

    # The app log outlives a restart of the head unit; its monotonic clock does not. Each start of the app since the
    # clock last went back is one HmiMain3D: more than one is iDrive restarting by itself.
    boot = 0
    previous = None
    for i, line in enumerate(app):
        t = mono(line)
        if t is not None:
            if previous is not None and t < previous:
                boot = i
            previous = t
    starts = sum(1 for i in creates if i >= boot)
    if creates:
        result(
            starts == 1,
            "the iDrive app started once since the head unit started"
            if starts == 1
            else f"the iDrive app started {starts} times since the head unit started: iDrive restarted",
        )

    service = report.get("/tmp/aa-entrynav-service.log", [])

    longest = longest_session(service)
    result(
        longest >= MIN_SESSION_S, f"longest connected session {longest / 60:.1f} min (at least {MIN_SESSION_S // 60})"
    )

    silent = [line.strip() for line in service if "iDrive app silent" in line]
    result(not silent, "the iDrive app's heartbeat never stopped" if not silent else f"heartbeat stopped: {silent[0]}")

    guard = [line.strip() for line in app_now if "thread guard:" in line]
    result(not guard, "no lookup on a wrong thread" if not guard else f"{len(guard)} thread guard line(s): {guard[0]}")

    gear = any("CDS driving.gear =" in line for line in app_now)
    result(gear, "the app saw the car's gear" if gear else "no gear line from the app since it started")
    track = any("track info: " in line for line in app_now)
    result(track, "the app saw the phone's track" if track else "no track info line from the app since it started")

    if any("cluster player: installed" in line for line in app_now):
        read = any("entertainment source read on HmiMain3D's thread" in line for line in app_now)
        result(
            read,
            "iDrive's entertainment source read on HmiMain3D's thread"
            if read
            else "no source reading on HmiMain3D's thread",
        )
        stopped = [line.strip() for line in app_now if "no fresh reading of iDrive's entertainment source" in line]
        result(
            not stopped, "source readings never stopped" if not stopped else f"source readings stopped: {stopped[0]}"
        )
        sources = [line for line in app_now if "cluster player: iDrive's entertainment source is 0x" in line]
        # Android Auto's source playing, then another one (the radio before the first route is not the driver's), then
        # Android Auto's again.
        state, back = 0, False
        for line in sources:
            playing = "(CarPlay's, built up" in line
            if state == 0 and playing:
                state = 1
            elif state == 1 and "(CarPlay's" not in line:
                state = 2
            elif state == 2 and playing:
                back = True
        result(
            back,
            "the source went to another one and back to Android Auto's"
            if back
            else "no switch to another source and back to Android Auto's during the drive",
        )
    return results


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    results = check(Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace"))
    for passed, message in results:
        print(f"{'PASS' if passed else 'FAIL'} {message}")
    failed = sum(1 for passed, _ in results if not passed)
    print(
        f"{'FAIL' if failed else 'PASS'}: {failed} of {len(results)} check(s) failed"
        if failed
        else f"PASS: all {len(results)} checks"
    )
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
