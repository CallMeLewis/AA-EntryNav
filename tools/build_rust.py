# Copyright (c) 2026 CallMeLewis
"""Build the Rust ports (rust/) for the head unit, after running their tests on this PC.

  python3 tools/build_rust.py      (make rust runs it; tools/verify_arm.py too)

build/rust gets each program, a static ARMv7 hard-float musl binary that needs nothing from the car's glibc, and
build/rust/tests each Rust test program built for the same target, which tests/run_qemu.py runs under QEMU. cargo runs
in rust/, where rust-toolchain.toml pins the toolchain; CARGO names another one, such as cargo.exe from WSL. The target
links with Rust's own rust-lld, so no cross compiler and no copy of the car's system is needed.

The glibc Rust (GLIBC_CRATES) is a staticlib for armv7-unknown-linux-gnueabihf, put in build/rust as lib<name>.a. It
needs no linker here either: the Makefile links it in the build image with GCC 4.7 against the copy of the car's
system (make evo-rsprobe).
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGET = "armv7-unknown-linux-musleabihf"
GLIBC_TARGET = "armv7-unknown-linux-gnueabihf"
# The crates built as glibc staticlibs, each linked by a Makefile rule with a C main.
GLIBC_CRATES = ("evo-rsprobe",)


def cargo():
    found = os.environ.get("CARGO") or shutil.which("cargo")
    if not found:
        raise SystemExit(
            "FAIL: no cargo. rustup (https://rustup.rs) installs the toolchain rust/rust-toolchain.toml pins"
        )
    return found


def built(root, *args):
    """What `cargo <args> --message-format=json` builds: (name, path, kind), kind "bin", "test", "example" or
    "staticlib"."""
    print("+ cargo", " ".join(args), flush=True)
    result = subprocess.run(
        [cargo(), *args, "--message-format=json-render-diagnostics"],
        cwd=root / "rust",
        stdout=subprocess.PIPE,
        text=True,
        check=True,
    )
    found = []
    for line in result.stdout.splitlines():
        message = json.loads(line) if line.startswith("{") else {}
        if message.get("reason") != "compiler-artifact":
            continue
        target = message["target"]
        if message.get("executable"):
            kind = "test" if message["profile"]["test"] else "example" if "example" in target["kind"] else "bin"
            found.append((target["name"], Path(message["executable"]), kind))
        elif "staticlib" in target["crate_types"]:
            found += [(target["name"], Path(f), "staticlib") for f in message["filenames"] if f.endswith(".a")]
    return found


def build(root=ROOT):
    """Tests on this PC, then the programs, the glibc staticlibs, the examples and the ARM tests into build/rust.
    Returns the programs' names."""
    print("+ cargo test --locked", flush=True)
    subprocess.run([cargo(), "test", "--locked"], cwd=root / "rust", check=True)
    out = root / "build/rust"
    shutil.rmtree(out, ignore_errors=True)
    (out / "tests").mkdir(parents=True)
    (out / "examples").mkdir()
    musl = ("build", "--release", "--locked", "--target", TARGET)
    programs = [(n, p) for n, p, kind in built(root, *musl, "--bins") if kind == "bin"]
    for name, path in programs:
        shutil.copy2(path, out / name)
    # The examples the QEMU checks run beside the programs, such as the bus players of the D-Bus tools.
    examples = [p for _, p, kind in built(root, *musl, "--examples") if kind == "example"]
    for path in examples:
        shutil.copy2(path, out / "examples" / path.name)
    glibc = []
    for crate in GLIBC_CRATES:
        args = ("build", "--release", "--locked", "--lib", "-p", crate, "--target", GLIBC_TARGET)
        glibc += [p for _, p, kind in built(root, *args) if kind == "staticlib"]
    for path in glibc:
        shutil.copy2(path, out / path.name)
    tests = [p for _, p, kind in built(root, "test", "--no-run", "--locked", "--target", TARGET) if kind == "test"]
    for path in tests:
        shutil.copy2(path, out / "tests" / path.name)
    names = [n for n, _ in programs] + [p.name for p in glibc]
    print(
        f"build/rust: {', '.join(names)}, {len(examples)} examples and {len(tests)} test programs for ARM", flush=True
    )
    return [name for name, _ in programs]


if __name__ == "__main__":
    try:
        build()
    except subprocess.CalledProcessError as error:
        sys.exit(f"FAIL: {' '.join(str(a) for a in error.cmd)}")
