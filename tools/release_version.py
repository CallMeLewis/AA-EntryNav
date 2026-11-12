# Copyright (c) 2026 CallMeLewis
"""Release version numbers: what tools/publish_release.py accepts, and their order.

A version is X.Y.Z for a stable build, or X.Y.Z-beta.N or X.Y.Z-test.N for a build to try before it (N from 1). Builds
of the same X.Y.Z come in this order: every test build, then every beta build, then the stable build, which is always
above them. A higher X.Y.Z is above everything of a lower one, whatever its kind:

    0.1.3 < 0.1.4-test.1 < 0.1.4-test.2 < 0.1.4-beta.1 < 0.1.4 < 0.1.5-test.1

This is SemVer's order (a pre-release is below its release), except that test comes before beta, where SemVer would sort
the two words alphabetically. Flare Drive compares versions the same way (NextStep.compareVersions).
"""

import re

_VERSION = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-(test|beta)\.([1-9]\d*))?")
_RANK = {"test": 0, "beta": 1}
_STABLE = 2

FORMAT = "X.Y.Z, X.Y.Z-beta.N or X.Y.Z-test.N"


def is_valid(version):
    return _VERSION.fullmatch(version) is not None


def is_prerelease(version):
    """A beta or test build."""
    match = _VERSION.fullmatch(version)
    return match is not None and match.group(4) is not None


def key(version):
    """A sort key in release order. ValueError for anything else than FORMAT."""
    match = _VERSION.fullmatch(version)
    if match is None:
        raise ValueError(f"{version} is not {FORMAT}")
    major, minor, patch, kind, number = match.groups()
    rank = _RANK[kind] if kind else _STABLE
    return (int(major), int(minor), int(patch), rank, int(number) if number else 0)
