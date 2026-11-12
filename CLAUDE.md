# Notes for coding agents

Write commit messages following [docs/commit-messages.md](docs/commit-messages.md), and the CHANGELOG and each
release's Flare Drive note following [docs/release-notes.md](docs/release-notes.md).

## Tests

CI runs only what needs no copy of the car's system: `make check` (style, content checks, `verify_tests`, the PC
suite). Everything else, the ARM build, the ARM tests under QEMU and the checks of the release programs, runs in
`python tools/verify_arm.py`:

- the pre-push hook runs it on every commit pushed to `main` or `release/*`, checked out alone, and refuses the push if
  it fails (`git config core.hooksPath .githooks` after cloning);
- `tools/publish_release.py` runs it before packaging.

Run it yourself before committing a change to anything that only builds for ARM (a `tests/test_*.cpp` that
`make test-host` does not build, such as `test_wireless_lifecycle.cpp`, or code only those tests cover). On 11 November
2026 a test added to `test_wireless_lifecycle.cpp` passed CI and stopped a publish, which is why the hook runs it.

A new test is wired into `make test-host` or into `test-arm-all` and `tests/run_qemu.py`: `tools/verify_tests.py` fails
CI when one runs nowhere. A test that can only run by hand goes in its `MANUAL` list, with the reason.
