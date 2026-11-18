# Notes for coding agents

Write commit messages following [docs/commit-messages.md](docs/commit-messages.md). Write the CHANGELOG and each release's Flare Drive note following [docs/release-notes.md](docs/release-notes.md). Run all generated prose through the `unslop` skill before finalizing.

## Publishing

`tools/publish_release.py publish` sends each release to Flare Drive's plugin catalog through `flare-publish` (Flare Drive's `tools/flare_publish.py`, in the repo beside this one). The publishing Worker checks and stores it, and the per-plugin list for Flare Drive 2.2.1 and earlier is rebuilt from the catalog. `notes` and `stability` change the catalog the same way. Read [Flare Drive's docs/publishing.md](../flare-drive/docs/publishing.md) (https://github.com/CallMeLewis/flare-drive/blob/main/docs/publishing.md) before publishing or changing a release: it describes the whole process, and Flare Drive's `CLAUDE.md` asks to keep it current.

* Never upload an archive or `index.json` to the bucket any other way, for example with Wrangler. Flare Drive 2.2.2 and later read only the catalog, so they would never see it.
* `retire` is refused: the catalog would keep listing the release. Hide the plugin or change a release on the publishing page (`publish.darkzone.dev`) instead, as that guide describes.
* `plugins/aa-entrynav.toml` is the plugin's manifest. Its name, summary, description and order only describe a plugin new to the catalog; edit them on the publishing page.

## Tests

CI runs only the checks that need no copy of the car's system. `make check` runs style checks, content checks, `verify_tests`, and the PC suite. Everything else runs in `python tools/verify_arm.py`, including the ARM build, the ARM tests under QEMU, and the release program checks.

* The pre-push hook runs `verify_arm.py` on every commit pushed to `main` or `release/*` when checked out alone. It refuses the push if the check fails. Run `git config core.hooksPath .githooks` after cloning to enable it.

* `tools/publish_release.py` runs `verify_arm.py` before packaging.

* A push that changes documentation only starts no CI run, and the hook skips `verify_arm.py` for it. Documentation is every Markdown file and everything under `docs/`, except `NOTICE.md`. The rule is in `tools/code_changes.py` and in the `paths` of `.github/workflows/ci.yml`; change both together.

Run `verify_arm.py` yourself before committing a change to anything that only builds for ARM. This applies to any `tests/test_*.cpp` file that `make test-host` does not build, such as `test_wireless_lifecycle.cpp`, or code that only those tests cover. On 11 November 2026, a test added to `test_wireless_lifecycle.cpp` passed CI and stopped a publish, which is why the hook runs it.

Wire every new test into `make test-host` or into `test-arm-all` and `tests/run_qemu.py`. `tools/verify_tests.py` fails CI when a test runs nowhere. Put any test that can only run by hand in its `MANUAL` list, along with the reason.