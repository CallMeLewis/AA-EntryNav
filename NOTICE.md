# Notice

AA-EntryNav is under the PolyForm Noncommercial License 1.0.0 ([LICENSE.md](LICENSE.md)). It holds code from earlier
projects under the same licence.

## Code from earlier projects

AA-EntryNav started from [entry-aad](https://github.com/Jiraiya43/entry-aad) by Jiraiya43, a port of
[Supra-iDrive6-Android-Auto](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto) by Sq00ky, which began as NBT-AAD by
nanaisu. All of them are under the [PolyForm Noncommercial License
1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0): noncommercial use only, and anyone given a copy of
any part of that code must also get the licence's URL and these lines:

> Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
>
> Required Notice: Copyright (c) 2026 Jiraiya43 - entry-aad

A file that still holds entry-aad's code starts with its header, `Copyright (c) 2026 Jiraiya43` and
`SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0`. `git grep -l "Copyright (c) 2026 Jiraiya43"` lists them, and
`tools/verify_core.py` counts them. A file written for AA-EntryNav starts with `Copyright (c) 2026 CallMeLewis`.

## Material from others

A few files hold data neither project wrote. They keep their own terms whatever happens to the code:

- `core/include/hu_certificate.h`: the Android Auto head unit certificate and key, issued by the Google Automotive Link
  CA, as open Android Auto head units share it.
- `core/include/dhu_sdp.h`: the service description sent by Google's Desktop Head Unit.
- `hmiapp/res/spinner/`: the waiting animation of Supra-iDrive6-Android-Auto.
- `hmiapp/res/aa_row_icon_63x54.png`: Google's Android Auto icon, 2026 version (its partner brand site, via Wikimedia
  Commons' "Android Auto icon (2026 update).png"), scaled to 54 px. Flare Drive shows the same icon. Android Auto is a
  trademark of Google LLC.
- `tests/fixtures/gps_e7/LocationData.proto`: an unmodified file of [aasdk](https://github.com/opencardev/aasdk), under
  the GNU General Public License v3.0. It is read by one PC test and is not part of the programs.
- `platform/kombi_graph_template.hpp` and the car readings of `tests/fixtures/`: bytes and outputs recorded on a BMW
  head unit, kept for interoperability and tests. See `tests/fixtures/README.md`.

## Release archives

Flare Drive expects a `LICENSE.md` in each release archive. `tools/package_release.py` copies the repository's
`LICENSE.md` there, so the licence and every required notice go with each copy of a build.
