# Copyright (c) 2026 CallMeLewis
"""Build the ARM programs and run every check that needs the copy of the car's system, which CI can't have.

  python tools/verify_arm.py               the working tree as it is
  python tools/verify_arm.py --commit SHA  that commit alone, checked out in a temporary folder (the pre-push hook)

The steps: the ARM build against the car's libraries (programs, iDrive app, ARM tests), the ARM tests under QEMU
(tests/run_qemu.py), then the checks of the programs a release ships (tools/verify_binary.py, verify_wireless.py).
publish_release.py runs the same, then packages. On Linux they run here, through toolchain/tc.sh; elsewhere in Docker:
the build image, and an image with qemu-arm and the ARM binutils (QEMU_IMAGE). HU_ROOTFS gives the copy of the car's
system: a folder or a Docker volume.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN_IMAGE = "entrynav2-tc:4.7"  # toolchain/tc.sh's image
QEMU_IMAGE = os.environ.get("QEMU_IMAGE", "aa-entrynav-qemu:local")  # any image with qemu-arm and python3
CHECKS = ("tests/run_qemu.py", "tools/verify_binary.py", "tools/verify_wireless.py")
BUILD = ("make", "-j4", "all", "hmiapp", "test-arm-all")


def run(*args, cwd=ROOT):
    print("+", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def run_all(root, checks=CHECKS, clean=False):
    """The ARM build in root, then each check, each under QEMU with the car's libraries. Stops at the first failure.

    clean: `make clean` first, so that nothing built earlier in root (other flags, an older Makefile) is reused. The
    objects are git-ignored, so a clean `git status` says nothing about them; publish_release.py ships this build.
    """
    if sys.platform.startswith("linux"):
        if clean:
            run("bash", "toolchain/tc.sh", "make", "clean", cwd=root)
        run("bash", "toolchain/tc.sh", *BUILD, cwd=root)
        for check in checks:
            run(sys.executable, "-I", check, cwd=root)
        return
    rootfs = os.environ.get("HU_ROOTFS", "entrynav2-rootfs")
    docker = ["docker", "run", "--rm", "-v", f"{root}:/work", "-w", "/work"]
    if clean:
        run(*docker, "-v", f"{rootfs}:/sysroot:ro", TOOLCHAIN_IMAGE, "make", "clean", cwd=root)
    run(*docker, "-v", f"{rootfs}:/sysroot:ro", TOOLCHAIN_IMAGE, *BUILD, cwd=root)
    for check in checks:
        run(*docker, "-v", f"{rootfs}:/root/entrynav2/rootfs:ro", QEMU_IMAGE, "python3", "-I", check, cwd=root)


def run_commit(commit):
    """run_all on one commit, checked out alone: uncommitted changes in the working tree can't make it pass or fail."""
    folder = Path(tempfile.mkdtemp(prefix="aa-entrynav-arm-"))
    try:
        run("git", "worktree", "add", "--detach", "--quiet", folder, commit)
        run_all(folder)
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", str(folder)], cwd=ROOT, capture_output=True)
        shutil.rmtree(folder, ignore_errors=True)
        subprocess.run(["git", "worktree", "prune"], cwd=ROOT, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--commit", help="check this commit instead of the working tree")
    args = parser.parse_args()
    try:
        if args.commit:
            print(f"ARM build and tests under QEMU on {args.commit[:7]}, checked out alone (a few minutes)", flush=True)
            run_commit(args.commit)
        else:
            run_all(ROOT)
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"FAIL: the ARM checks stopped at: {' '.join(str(a) for a in error.cmd)}") from None
    print("PASS: ARM build, ARM tests under QEMU, release programs checked", flush=True)


if __name__ == "__main__":
    main()
