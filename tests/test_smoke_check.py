# Copyright (c) 2026 CallMeLewis
"""tools/smoke_check.py: a report from a good drive passes, and each thing test.1 and test.2 left behind fails it."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import smoke_check as sc  # noqa: E402

failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {what}")


APP = "[AAEntryNavApp]"
SOURCE = "cluster player: iDrive's entertainment source is"
PLAYS = f"{SOURCE} 0x82 (CarPlay's, built up: Android Auto's sound)"
GOOD_APP = [
    f"2026-11-24 09:00:00 [  700.000] {APP} create(name=AAEntryNav) aa-entrynav-app-12, HMI pid 200",  # last boot
    f"2026-11-25 09:00:00 [   11.200] {APP} create(name=AAEntryNav) aa-entrynav-app-12, HMI pid 265",
    f"2026-11-25 09:00:00 [   11.300] {APP} cluster player: installed on CarPlay's player 0x1 (...)",
    f"2026-11-25 09:00:20 [   30.000] {APP} cluster player: iDrive's entertainment source read on HmiMain3D's thread",
    f"2026-11-25 09:00:21 [   31.000] {APP} {SOURCE} 0x1",
    f"2026-11-25 09:00:25 [   35.000] {APP} {PLAYS}",
    f"2026-11-25 09:00:30 [   40.000] {APP} CDS driving.gear = '{{\"gear\":9}}' -> gear 9",
    f"2026-11-25 09:00:31 [   41.000] {APP} track info: Song|Artist|Album|3ebd6ba2",
    f"2026-11-25 09:05:00 [  310.000] {APP} {SOURCE} 0x1",
    f"2026-11-25 09:05:10 [  320.000] {APP} {PLAYS}",
]
GOOD_SERVICE = [
    "=== 1795623625: the iDrive app starts /opt/aa/evo-trial --service (aa-entrynav-app-12)",
    "[   37.199] [Supervisor] started by the iDrive app (pid 3322); restore journal in /tmp/aa-entrynav-service",
    "[   45.777] [Control] state -> connecting 3568 0",
    "[   59.576] [Control] state -> connected 3568 2",
    "[AASession] Frame RX: Ch=0, Flags=0x03, Len=23",
    "[  700.000] [Control] state -> connected 3568 1",
]


def report(
    app=GOOD_APP,
    service=GOOD_SERVICE,
    service_old=(),
    service_section="/var/local/data/AA/service.log",
    ids="HmiMain3D: 265\nevo-trial: 3322\naa-entrynav: 3568",
):
    lines = [
        "Diagnostic report",
        "===== CONTROLLER DIAGNOSTICS =====",
        "===== RUNNING PROCESS IDS =====",
        ids,
        "===== /var/local/data/AA/app.old.log (last 128 KiB) =====",
        *app[:1],
        "===== /var/local/data/AA/app.log (last 128 KiB) =====",
        *app[1:],
    ]
    if service_old:
        lines += [
            "===== /var/local/data/AA/service.old.log (last 128 KiB) =====",
            *service_old,
        ]
    lines += [
        f"===== {service_section} (last 128 KiB) =====",
        *service,
        "===== LOAD =====",
        "===== END CONTROLLER DIAGNOSTICS =====",
    ]
    return "\n".join(lines)


def failed(text):
    return [message for passed, message in sc.check(text) if not passed]


def main():
    check(failed(report()) == [], f"a good drive passes: {failed(report())}")

    def fails(text, words, what):
        found = failed(text)
        check(any(words in message for message in found), f"{what}: {found}")

    fails(report(ids="evo-trial: 3322"), "HmiMain3D is not running", "HmiMain3D gone (test.2)")
    restarted = GOOD_APP + [
        f"2026-11-25 09:06:00 [  371.000] {APP} create(name=AAEntryNav) aa-entrynav-app-12, HMI pid 266"
    ]
    fails(report(app=restarted, ids="HmiMain3D: 266"), "started 2 times", "iDrive restarted by itself")
    fails(report(ids="HmiMain3D: 999"), "another HmiMain3D", "the app not started in the running HmiMain3D")
    short = GOOD_SERVICE[:4] + [
        "[  120.000] [ServiceApp] session ended",
        "[  700.000] [Control] state -> connecting 3568 0",
    ]
    fails(report(service=short), "longest connected session 1.0 min", "a one-minute session")
    silent = GOOD_SERVICE + ["[  705.000] [ServiceApp] iDrive app silent 5 s (clock at 705 s): HmiMain3D runs"]
    fails(report(service=silent), "heartbeat stopped", "the app went silent (test.1, test.2)")
    guarded = GOOD_APP + [f"2026-11-25 09:06:00 [  371.000] {APP} thread guard: getHmiProcUnit on thread 'x', ..."]
    fails(report(app=guarded), "thread guard line", "a lookup on a wrong thread")
    fails(report(app=[line for line in GOOD_APP if "track info" not in line]), "no track info", "no track line")
    fails(report(app=[line for line in GOOD_APP if "driving.gear" not in line]), "no gear line", "no gear line")
    fails(report(app=[line for line in GOOD_APP if "read on HmiMain3D" not in line]), "no source reading", "no reading")
    stopped = GOOD_APP + [
        f"2026-11-25 09:06:00 [  371.000] {APP} cluster player: no fresh reading of iDrive's entertainment source"
    ]
    fails(report(app=stopped), "source readings stopped", "readings stopped")
    fails(report(app=GOOD_APP[:-2]), "no switch to another source", "no switch and back")
    fails(report(app=[], service=[]), "no start of the iDrive app", "logging off")
    # Without the cluster player (no audio-wifi), its checks are not asked for.
    plain = [line for line in GOOD_APP if "cluster player" not in line]
    check(failed(report(app=plain)) == [], f"no cluster player: {failed(report(app=plain))}")
    # A persistent service log may carry an earlier boot's stall or session before a monotonic clock reset.
    prior_stall = ["[  900.000] [ServiceApp] iDrive app silent 5 s (clock at 900 s): HmiMain3D is not running"]
    check(failed(report(service_old=prior_stall)) == [], "a stall from a prior boot is ignored")
    prior_long = [
        "[   10.000] [Control] state -> connected 3568 2",
        "[  900.000] [Control] state -> connected 3568 1",
    ]
    fails(
        report(service_old=prior_long, service=short), "longest connected session 1.0 min", "prior boot session ignored"
    )
    # Older reports with /tmp/aa-entrynav-service.log still pass.
    check(
        failed(report(service_section="/tmp/aa-entrynav-service.log")) == [],
        "legacy /tmp/aa-entrynav-service.log report passes",
    )

    if failures:
        sys.exit(1)
    print(
        "PASS smoke check: a good drive passes; HmiMain3D gone or restarted, a short session, a silent app, a thread "
        "guard line, no gear, track or source reading, no source switch, and logging off each fail it"
    )


main()
