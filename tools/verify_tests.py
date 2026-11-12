# Copyright (c) 2026 CallMeLewis
"""Check that every test runs somewhere: none forgotten in the Makefile, none built and never run.

A test is run by the PC suite (make test-host, which CI runs) or by tests/run_qemu.py (the ARM tests under QEMU, which
the pre-push hook and publish_release.py run through tools/verify_arm.py). Three things are checked:
  - every tests/test_* source is run by one of them, or listed in MANUAL with the reason it can't be;
  - every program the PC suite builds is also run by it;
  - every ARM program test-arm-all builds is run by tests/run_qemu.py, and everything run_qemu.py runs is built.
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
    for line in joined:
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

    if problems:
        raise SystemExit("FAIL:\n  " + "\n  ".join(problems))
    sources = len([s for s in (ROOT / "tests").glob("test_*") if s.suffix in (".c", ".cpp", ".py")])
    print(
        f"PASS: {sources} test sources, each run by the PC suite or under QEMU ({len(MANUAL)} by hand); "
        f"{len(arm_programs)} ARM test programs, all run by tests/run_qemu.py"
    )


if __name__ == "__main__":
    main()
