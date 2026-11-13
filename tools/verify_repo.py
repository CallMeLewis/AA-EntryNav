# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Check what git would publish: no password, no compiled program, nothing large, no local-only folder.

Every file of every commit is read from git itself, not from the working tree: what is checked is what leaves the PC.
  python tools/verify_repo.py             the history of HEAD and the staged files (CI, by hand)
  python tools/verify_repo.py --pre-push  the commits being pushed, as git lists them on the hook's input
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MAX_BYTES = 1024 * 1024
FORBIDDEN_PREFIXES = ("artifacts/", ".secrets/", "rootfs/")
NO_COMMIT = "0" * 40


def git(*args, data=None):
    return subprocess.run(["git", *args], cwd=ROOT, input=data, check=True, capture_output=True).stdout


def pushed_revisions(hook_input):
    """The local commits named on the pre-push hook's input; a deleted branch sends none."""
    revisions = []
    for line in hook_input.splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[1] != NO_COMMIT:
            revisions.append(fields[1])
    return revisions


def files_of(revisions, staged):
    """(path, blob id) of every file in the history of the revisions, plus the index when asked."""
    found = {}
    if revisions:
        listing = git("rev-list", "--objects", *revisions).decode("utf-8", "replace").splitlines()
        candidates = [line.split(" ", 1) for line in listing if " " in line]
        query = "".join(f"{sha}\n" for sha, _ in candidates).encode()
        types = git("cat-file", "--batch-check=%(objecttype)", data=query).decode().split()
        for (sha, path), kind in zip(candidates, types):
            if kind == "blob":
                found[(path, sha)] = None
    if staged:
        for line in git("ls-files", "-s", "-z").split(b"\0"):
            if line:
                meta, path = line.decode("utf-8", "replace").split("\t", 1)
                found[(path, meta.split()[1])] = None
    return list(found)


def problems_in(path, data, secrets):
    if path.startswith(FORBIDDEN_PREFIXES):
        yield "this folder stays local"
    if len(data) > MAX_BYTES:
        yield f"{len(data)} bytes"
    if data.startswith((b"\x7fELF", b"MZ")):
        yield "compiled program"
    for what, value in secrets:
        if value in data:
            yield f"contains {what}"


def local_secrets():
    """(what it is, its bytes) for each secret this PC holds, read from where it lives, never from the repository."""
    found = []
    secret_file = ROOT / ".secrets/hu-password.txt"
    if secret_file.exists() and secret_file.read_text(encoding="utf-8").strip():
        found.append(("the car's password", secret_file.read_text(encoding="utf-8").strip().encode()))
    # Flare Drive's release signing key (tools/publish_release.py): whoever holds it can sign a release list the app
    # trusts.
    key_file = Path(os.environ.get("FLARE_DRIVE_SIGNING_KEY", Path.home() / ".flare-drive" / "release-signing-key"))
    if key_file.is_file() and key_file.read_text(encoding="utf-8").strip():
        found.append(("the release signing key", key_file.read_text(encoding="utf-8").strip().encode()))
    # The Cloudflare API token publish_release.py retire purges the cache with.
    token_file = Path(
        os.environ.get("FLARE_DRIVE_PURGE_TOKEN", Path.home() / ".flare-drive" / "cloudflare-purge-token")
    )
    if token_file.is_file() and token_file.read_text(encoding="utf-8").strip():
        found.append(("the Cloudflare purge token", token_file.read_text(encoding="utf-8").strip().encode()))
    return found


def main():
    pre_push = "--pre-push" in sys.argv[1:]
    revisions = pushed_revisions(sys.stdin.read()) if pre_push else ["HEAD"]
    if (
        not pre_push
        and subprocess.run(["git", "rev-parse", "-q", "--verify", "HEAD"], cwd=ROOT, capture_output=True).returncode
    ):
        revisions = []  # no commit yet: only the index
    secrets = local_secrets()
    files = files_of(revisions, staged=not pre_push)
    problems = []
    for path, sha in files:
        data = git("cat-file", "blob", sha)
        problems.extend(f"{path} ({sha[:8]}): {problem}" for problem in problems_in(path, data, secrets))
    if problems:
        print("\n".join(f"FAIL {problem}" for problem in sorted(set(problems))))
        return 1
    state = "searched for " + ", ".join(what for what, _ in secrets) if secrets else "no local secret to search for"
    print(f"PASS: {len(files)} file versions, nothing private or compiled ({state})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
