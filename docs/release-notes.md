# Writing release notes

Follow these when writing or editing release notes, which every version needs.

## Where they live

Everything is written in one place: the release's section of `CHANGELOG.md`, newest at the top, kept outside git until
the repository goes public. `tools/publish_release.py` takes three texts from it:

- **The CHANGELOG section itself.** It is for people running AA-EntryNav, whether through Flare Drive or by hand with
  `setup.sh`, not a record of every change: that record is the git history, written following
  [commit-messages.md](commit-messages.md).
- **The Flare Drive note**: one short paragraph, written in the section as an HTML comment, `<!-- Flare Drive: ... -->`.
  Flare Drive 2.0.10 and earlier show it on the release's page. Later versions show it only for a release whose list
  entry has no full notes.
- **The GitHub Release's text**, which `publish` makes from the section: an `Action needed:` paragraph becomes a
  `> [!IMPORTANT]` callout, `###` headings become `##`, the verified or unverified wording is left out, and each
  paragraph or bullet goes on one line, since GitHub shows hard line breaks. `publish` also puts it in Flare Drive's
  release list as the release's full notes, which Flare Drive opens from View release notes. Flare Drive shows the
  callout, `##` headings, bullets, bold and inline code; a link shows as its text only.

`publish_release.py preview` shows the Flare Drive note and the GitHub Release's text as `publish` will use them.

Changes collect under `## [Unreleased]` as they land. `publish_release.py prepare X.Y.Z "<summary>"` turns that into
the version's section and makes the release commit.

## The CHANGELOG section

```
## [0.1.1] - 2026-11-11 · Unverified

<!-- Flare Drive:
As your phone connects, the iDrive can switch to Bluetooth audio by itself (a switch in Flare Drive 2.0.6). The
service's log can no longer fill the head unit's memory.
-->

The iDrive switches to Bluetooth audio as the phone connects, and the service's log can no longer fill the head unit's
memory. Only the iDrive app (`libAAEntryNav.so`) changes. This version is unverified.

### Added

- **The iDrive switches to Bluetooth audio as the phone connects**, with the `select-bt-audio` file, a switch in Flare
  Drive. ... Unverified.
```

- Verified means this exact build has run in a car. Unverified means it has only been tested on PC and under QEMU.
- The heading is `## [X.Y.Z]`, ` - `, the release date (YYYY-MM-DD), then ` · Unverified`, or ` · Verified` once this
  exact build has run in a car. `prepare` writes it; `publish` sets the date to the day it published the release.
- A build to try before X.Y.Z is `X.Y.Z-test.N` or `X.Y.Z-beta.N`, with a section of its own. Test builds come before
  beta builds, and X.Y.Z itself is above both, so Flare Drive offers it as an update over them
  (`tools/release_version.py`). A beta or test build is always experimental, and a pre-release on GitHub. X.Y.Z's
  section says everything that changed since the previous stable build, betas included: most people never saw them.
- The Flare Drive note comes next, as `<!-- Flare Drive: ... -->` (see The Flare Drive note below).
- Then a short paragraph: what the release does for someone in the car, which programs change, and whether it is
  verified. Anything that needs the reader to act goes in a paragraph of its own before it (see Action needed below).
- Then [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) categories as `### ` headings, in this order, skipping
  empty ones: Added, Changed, Fixed, Removed, Security.
- Each bullet opens with its point in bold, then the detail. Markdown is fine here: GitHub renders it.
- Each bullet says whether that change is verified or unverified, in a closing sentence that starts with "Verified" or
  "Unverified", which the GitHub Release leaves out. Nothing counts as working until a car has shown it.
- A version links to the commit its archive was built from, in the links at the end of the file. `publish` adds the
  link once it has published the release, and `prepare` adds any that are still missing.

## The Flare Drive note

```
<!-- Flare Drive:
As your phone connects, the iDrive can switch to Bluetooth audio by itself (a switch in Flare Drive 2.0.6). The
service's log can no longer fill the head unit's memory.
-->
```

- It goes right under the section's heading. Wrap it like the rest of the file: `publish` joins its lines into one.
- One paragraph of plain text, one to three sentences. Flare Drive shows it as it is, so no Markdown, bullets or
  headings.
- Only what someone in the car notices, with the most important change first.
- Name the Flare Drive switch or screen, never the file behind it: "a switch in Flare Drive", not `select-bt-audio`.
- Say which Flare Drive version a new switch needs, when an older one doesn't have it.
- Whether it is stable or experimental is not part of the note: `publish` records it (`--stable`) and Flare Drive
  shows it. Whether it is verified is not part of the note either: only the CHANGELOG records that.
- To change a published release's texts, edit its CHANGELOG section, then run `publish_release.py notes X.Y.Z` (several
  versions at once are fine): it updates the note and full notes in Flare Drive's release list and the GitHub Release's
  text. `stability` changes the label.

## Writing them

- Describe what changed from the point of view of someone in the car: what they can now do, what behaves differently,
  or what problem is gone. Lead with the result, not how it was built.
  Prefer "The steering wheel voice button starts your phone's assistant on a short hold." over "Map the PTT key's long
  press to the assistant request."
- Say where to find anything new: a switch in Flare Drive, the Android Auto row in iDrive's Media menu, a file in
  `/var/local/data/AA` for someone installing by hand.
- Use plain language and the car's and Flare Drive's own words. In the CHANGELOG, file names and commands are fine
  where the reader acts on them by hand (creating `usb`, running `setup.sh status`); internal function, class and
  variable names are not.
- When a change only affects one firmware or one way of connecting, say so: "Over the cable, ...", "On
  202111011321, ...".
- Keep each bullet to a sentence or two, and be specific and factual. No "various improvements", "general bug fixes"
  or "improved performance" when you know what actually improved, no marketing language, and don't exaggerate how big
  a change is.
- Leave out anything nobody in the car can notice: refactoring, tests, CI, build, packaging and release tooling.
- Nothing that identifies a car: no password, VIN, addresses or vehicle IDs.

## One bullet per change

A change appears once, however much work went into it. A new feature gets one bullet that says what it does, including
its knock-on effects elsewhere (a new switch in Flare Drive, a different iDrive screen). The same kind of change in
several places is one bullet too.

The test: would someone in the car consider this a separate change? If not, merge it. Don't combine unrelated changes
just because they were made together, either.

## Action needed

A change that needs the reader to do something, such as restarting the head unit, turning a setting on again or
updating Flare Drive first, goes first: in a paragraph of its own at the start of the CHANGELOG section, which the
GitHub Release shows as a callout, and at the start of the Flare Drive note. Start it with "Action needed:" and say what
changed, who it affects and what to do. For example: "Action needed: this version needs Flare Drive 2.0.6 or later, so
update Flare Drive before installing it."

## Crediting contributors

When a change came from a pull request by someone other than the maintainer, credit them at the end of its bullet with
their GitHub username: "Thanks to @username." Credit the people whose pull request made the change, not those who
reported or suggested it, and keep the credit when the change was reworked before it was merged.

## Final check

- Is every bullet something a person in the car can notice, in the car's and Flare Drive's own words?
- Does every new feature say where to find it, and which Flare Drive version it needs?
- Does every bullet, and the section's heading, say whether it is verified or unverified?
- Is anything that needs the reader to act first, starting with "Action needed:"?
- Is the Flare Drive note one plain paragraph, with no Markdown and no file names?
- Is anything that identifies a car left out?
- Does `publish_release.py preview` show the note and the GitHub Release's text you expect?
