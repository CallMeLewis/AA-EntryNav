# Copyright (c) 2026 CallMeLewis
"""tools/release_version.py: which versions a release may have, and their order (a stable build above its betas)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import release_version as rv  # noqa: E402

failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {what}")


def order():
    ordered = [
        "0.1.3",
        "0.1.4-test.1",
        "0.1.4-test.2",
        "0.1.4-test.10",  # numbers, not text: 10 after 2
        "0.1.4-beta.1",
        "0.1.4-beta.2",
        "0.1.4",
        "0.1.5-test.1",
        "0.2.0-beta.1",
        "0.2.0",
        "0.10.0",
        "1.0.0-test.1",
        "1.0.0",
    ]
    for lower, higher in zip(ordered, ordered[1:]):
        check(rv.key(lower) < rv.key(higher), f"{lower} < {higher}")
    shuffled = list(reversed(ordered))
    check(sorted(shuffled, key=rv.key) == ordered, "sorting puts every build in release order")


def stable_above_its_builds():
    for build in ("0.1.4-test.1", "0.1.4-test.999", "0.1.4-beta.1", "0.1.4-beta.999"):
        check(rv.key("0.1.4") > rv.key(build), f"0.1.4 above {build}")
        check(rv.key("0.1.3") < rv.key(build), f"0.1.3 below {build}")


def kinds():
    for stable in ("0.0.0", "0.1.3", "10.20.30"):
        check(rv.is_valid(stable) and not rv.is_prerelease(stable), f"{stable} is stable")
    for build in ("0.1.4-beta.1", "0.1.4-test.1", "1.2.3-beta.42"):
        check(rv.is_valid(build) and rv.is_prerelease(build), f"{build} is a beta or test build")


def refused():
    for bad in (
        "",
        "0.1",
        "0.1.3.4",
        "v0.1.3",
        "01.1.3",  # no leading zero
        "0.1.4-beta",  # the number is needed
        "0.1.4-beta.0",
        "0.1.4-beta.01",
        "0.1.4-rc.1",  # only beta and test
        "0.1.4-Beta.1",
        "0.1.4-beta.1.2",
        "0.1.4+build.1",
        "0.1.4-beta.1 ",
    ):
        check(not rv.is_valid(bad) and not rv.is_prerelease(bad), f"{bad!r} refused")
        try:
            rv.key(bad)
            check(False, f"key({bad!r}) raises")
        except ValueError:
            pass


order()
stable_above_its_builds()
kinds()
refused()
if failures:
    sys.exit(f"test_release_version: {failures} failure(s)")
print("PASS release versions: X.Y.Z, -beta.N and -test.N; test < beta < stable for one X.Y.Z; malformed ones refused")
