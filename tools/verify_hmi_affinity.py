# Copyright (c) 2026 CallMeLewis
"""Check that every per-thread iDrive lookup in the iDrive app goes through its thread guard.

hmiplugin::get<...>(), getHmiProcUnit() and getServiceProxy() answer from per-thread state in HmiMain3D: made on one of
the app's posted tasks, such a lookup stopped that thread in 0.1.9-test.1 and test.2 and iDrive died after it. Every
file under hmiapp/ that names one of them must include hmiapp/hmi_affinity.hpp and ask hmiThreadAllowed() at least
as often as it names them.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The mangled names the app looks up with dlsym.
LOOKUPS = re.compile(r"_ZN9hmiplugin3getI|_Z14getHmiProcUnitv|getServiceProxyE")
GUARD = "hmiThreadAllowed("
HEADER = '#include "hmi_affinity.hpp"'
EXEMPT = {"hmi_affinity.hpp", "hmi_affinity.cpp"}


def problems():
    found = []
    for path in sorted((ROOT / "hmiapp").glob("*.[ch]pp")):
        if path.name in EXEMPT:
            continue
        text = path.read_text(encoding="utf-8")
        code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("//"))
        lookups = len(LOOKUPS.findall(code))
        if not lookups:
            continue
        name = path.relative_to(ROOT).as_posix()
        if HEADER not in code:
            found.append(f"{name}: looks up iDrive's per-thread objects without including hmi_affinity.hpp")
        guards = code.count(GUARD)
        if guards < lookups:
            found.append(f"{name}: {lookups} per-thread lookup(s) but only {guards} hmiThreadAllowed() check(s)")
    return found


def main():
    found = problems()
    for line in found:
        print(f"FAIL {line}")
    if found:
        sys.exit(1)
    print("PASS iDrive's per-thread lookups in hmiapp/ all ask the thread guard (hmi_affinity.hpp)")


if __name__ == "__main__":
    main()
