# Notes for coding agents

Write commit messages following [docs/commit-messages.md](docs/commit-messages.md). Write the CHANGELOG and each release's Flare Drive note following [docs/release-notes.md](docs/release-notes.md). Run all generated prose through the `unslop` skill before finalizing.

## Tests

CI runs only the checks that need no copy of the car's system. `make check` runs style checks, content checks, `verify_tests`, and the PC suite. Everything else runs in `python tools/verify_arm.py`, including the ARM build, the ARM tests under QEMU, and the release program checks.

* The pre-push hook runs `verify_arm.py` on every commit pushed to `main` or `release/*` when checked out alone. It refuses the push if the check fails. Run `git config core.hooksPath .githooks` after cloning to enable it.

* `tools/publish_release.py` runs `verify_arm.py` before packaging.

Run `verify_arm.py` yourself before committing a change to anything that only builds for ARM. This applies to any `tests/test_*.cpp` file that `make test-host` does not build, such as `test_wireless_lifecycle.cpp`, or code that only those tests cover. On 11 November 2026, a test added to `test_wireless_lifecycle.cpp` passed CI and stopped a publish, which is why the hook runs it.

Wire every new test into `make test-host` or into `test-arm-all` and `tests/run_qemu.py`. `tools/verify_tests.py` fails CI when a test runs nowhere. Put any test that can only run by hand in its `MANUAL` list, along with the reason.