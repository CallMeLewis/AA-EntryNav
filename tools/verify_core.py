# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Check that every source file says whose it is.

A file still holding code from earlier projects carries their licence line; a file written for AA-EntryNav carries the
project's copyright. The count of the first kind is how much is left to rewrite.
"""

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = "SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0"
OWN = re.compile(r"Copyright \(c\) 20\d\d CallMeLewis")
SOURCE_DIRS = ("core", "platform", "hmiapp", "tools", "tests", "toolchain")
SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".py", ".sh"}
# Data neither project wrote: these files say where they come from instead of carrying a copyright.
THIRD_PARTY = {"core/include/dhu_sdp.h", "core/include/hu_certificate.h"}
ORIGIN = "Not written by this project"


def source_files():
    """The source files git tracks or would add."""
    listing = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *SOURCE_DIRS],
        cwd=ROOT,
        check=True,
        capture_output=True,
    ).stdout
    for name in sorted({item.decode() for item in listing.split(bytes(1)) if item}):
        path = ROOT / name
        if path.suffix.lower() in SUFFIXES and path.is_file() and "fixtures" not in path.parts:
            yield name, path


def main():
    upstream, unmarked = [], []
    for name, path in source_files():
        if name in THIRD_PARTY:
            continue
        head = path.read_text(encoding="utf-8", errors="replace")[:600]
        if UPSTREAM in head:
            upstream.append(name)
        elif not OWN.search(head):
            unmarked.append(name)
    if unmarked:
        raise SystemExit("FAIL: neither the project's copyright nor the earlier licence line in " + ", ".join(unmarked))
    unsigned = [name for name in sorted(THIRD_PARTY) if ORIGIN not in (ROOT / name).read_text(encoding="utf-8")[:600]]
    if unsigned:
        raise SystemExit("FAIL: no origin stated in " + ", ".join(unsigned))
    print(f"PASS: every source file has its header; {len(upstream)} still hold code from earlier projects")


if __name__ == "__main__":
    main()
