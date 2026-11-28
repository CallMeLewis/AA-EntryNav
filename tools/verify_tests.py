# Copyright (c) 2026 CallMeLewis
"""Check that every test runs somewhere: none forgotten in the Makefile, none built and never run.

A test is run by the PC suite (make test-host, which CI runs) or by tests/run_qemu.py (the ARM tests under QEMU, which
the pre-push hook and publish_release.py run through tools/verify_arm.py). Four things are checked:
  - every tests/test_* source is run by one of them, or listed in MANUAL with the reason it can't be;
  - every program the PC suite builds is also run by it;
  - every ARM program test-arm-all builds is run by tests/run_qemu.py, and everything run_qemu.py runs is built;
  - every Rust program in rust/ is compared with its C++ original by run_qemu.py's RUST_CASES, every case names a
    Rust program, and every crate has tests of its own in its tests/ folder. tools/build_rust.py builds them all, and
    run_qemu.py runs each test program it builds.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Tests run by hand only, with why.
MANUAL = {
    "tests/test_evo_touch_live.cpp": "needs root and raw sockets on the PC: sudo make test-touch-live",
}
TARGET = re.compile(r"^([A-Za-z0-9_./-]+):(?!=)(.*)$")


def make_targets():
    """{target: (prerequisites, recipe text)} from the Makefile, continuation lines joined."""
    targets, current = {}, None
    lines = (ROOT / "Makefile").read_text(encoding="utf-8").splitlines()
    joined, buffer = [], ""
    for line in lines:
        buffer += line[:-1] + " " if line.endswith("\\") else line
        if not line.endswith("\\"):
            joined.append(buffer)
            buffer = ""
    # NAME = words, as test-host's list of tests is written: $(NAME) among a target's prerequisites stands for them.
    lists = {m.group(1): m.group(2) for m in (re.match(r"^([A-Z_]+) = (.*)$", line) for line in joined) if m}
    for line in joined:
        line = re.sub(r"\$\(([A-Z_]+)\)", lambda m: lists.get(m.group(1), m.group(0)), line)
        if line.startswith("\t"):
            if current:
                targets[current][1].append(line.strip())
            continue
        match = TARGET.match(line)
        if match and not line.startswith((".", "#")):
            current = match.group(1)
            targets[current] = (match.group(2).split(), [])
        elif line.strip() and not line.startswith("#"):
            current = None
    return {name: (prereqs, "\n".join(recipe)) for name, (prereqs, recipe) in targets.items()}


def closure(targets, start):
    """The recipe text of a target and of every target it depends on."""
    seen, todo, text = set(), [start], []
    while todo:
        name = todo.pop()
        if name in seen or name not in targets:
            continue
        seen.add(name)
        prereqs, recipe = targets[name]
        text.append(recipe)
        todo.extend(prereqs)
    return "\n".join(text)


def main():
    targets = make_targets()
    host = closure(targets, "test-host")
    arm = closure(targets, "test-arm-all")
    qemu = (ROOT / "tests/run_qemu.py").read_text(encoding="utf-8")
    problems = []

    # What run_qemu.py runs: its CASES programs, and the Python tests it starts itself.
    qemu_programs = set(re.findall(r'^\s*\("[^"]+",\s*"([^"]+)"', qemu, re.MULTILINE))
    qemu_scripts = set(re.findall(r'"(tests/test_[a-z0-9_]+\.py)"', qemu))

    # Programs the PC suite builds must be run by it.
    for program in re.findall(r"-o (tests/[A-Za-z0-9_-]+)(?=\s|$)", host):
        if not program.endswith(".so") and f"./{program}" not in host:
            problems.append(f"{program} is built by make test-host but never run")

    # ARM programs must be run under QEMU; what QEMU runs must be built.
    arm_programs = set(re.findall(r"-o (tests/[A-Za-z0-9_-]+-arm)(?=\s|$)", arm))
    for program in sorted(arm_programs - qemu_programs):
        problems.append(f"{program} is built by make test-arm-all but tests/run_qemu.py never runs it")
    built = arm_programs | set(targets)
    for program in sorted(qemu_programs - built):
        problems.append(f"tests/run_qemu.py runs {program}, which make test-arm-all doesn't build")

    # Every test source runs somewhere.
    for source in sorted((ROOT / "tests").glob("test_*")):
        if source.suffix not in (".c", ".cpp", ".py"):
            continue
        name = source.relative_to(ROOT).as_posix()
        if name in MANUAL:
            continue
        in_host = name in host
        in_qemu = name in qemu_scripts or any(
            re.search(rf"{re.escape(name)}\b.*-o ({re.escape(program)})(?=\s|$)", arm) for program in qemu_programs
        )
        if not (in_host or in_qemu):
            problems.append(f"{name} is run neither by make test-host nor by tests/run_qemu.py (or list it in MANUAL)")
    for name in sorted(MANUAL):
        if not (ROOT / name).is_file():
            problems.append(f"MANUAL lists {name}, which no longer exists")

    # The Rust ports: each program compared with its C++ by run_qemu.py, each crate with tests of its own.
    rust_programs, crates = rust_workspace()
    block = re.search(r"^RUST_CASES = \{(.*?)^\}", qemu, re.MULTILINE | re.DOTALL)
    cases = set(re.findall(r'^\s*"([a-z0-9-]+)":', block.group(1), re.MULTILINE)) if block else set()
    for program in sorted(rust_programs - cases):
        problems.append(f"rust/ builds {program}, which tests/run_qemu.py's RUST_CASES never compares with its C++")
    for program in sorted(cases - rust_programs):
        problems.append(f"tests/run_qemu.py's RUST_CASES names {program}, which no crate in rust/ builds")
    for crate in crates:
        if not any((ROOT / "rust" / crate / "tests").glob("*.rs")):
            problems.append(f"rust/{crate} has no tests in rust/{crate}/tests")

    if problems:
        raise SystemExit("FAIL:\n  " + "\n  ".join(problems))
    sources = len([s for s in (ROOT / "tests").glob("test_*") if s.suffix in (".c", ".cpp", ".py")])
    print(
        f"PASS: {sources} test sources, each run by the PC suite or under QEMU ({len(MANUAL)} by hand); "
        f"{len(arm_programs)} ARM test programs, all run by tests/run_qemu.py; "
        f"Rust programs compared with their C++: {len(rust_programs)}"
    )


def rust_workspace():
    """The programs the crates of rust/Cargo.toml build ([[bin]] names), and the crates."""
    workspace = ROOT / "rust/Cargo.toml"
    if not workspace.is_file():
        return set(), []
    members = re.search(r"^members = \[(.*?)\]", workspace.read_text(encoding="utf-8"), re.MULTILINE | re.DOTALL)
    crates = re.findall(r'"([^"]+)"', members.group(1)) if members else []
    programs = set()
    for crate in crates:
        manifest = (ROOT / "rust" / crate / "Cargo.toml").read_text(encoding="utf-8")
        programs.update(re.findall(r'^\[\[bin\]\]\s*\nname = "([^"]+)"', manifest, re.MULTILINE))
    return programs, crates


if __name__ == "__main__":
    main()
