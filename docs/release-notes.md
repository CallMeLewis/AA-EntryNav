# Writing release notes

Follow these when writing or editing release notes, which every version needs.

## Where they live

Each release has two:

- **Its section of `CHANGELOG.md`**, newest at the top, kept outside git until the repository goes public. It is for people running AA-EntryNav, whether
  through Flare Drive or by hand with `setup.sh`, not a record of every change: that record is the git history,
  written following [commit-messages.md](commit-messages.md).
- **Its note in Flare Drive's release list**: one short paragraph, given to `tools/publish_release.py publish --notes`.
  Flare Drive shows it on the release's page, and the release's GitHub Release shows the same text.

Changes collect under `## [Unreleased]` as they land. At release, that becomes the version's section.

## The CHANGELOG section

```
## [0.1.1] - 2026-11-11 · PC

The iDrive switches to Bluetooth audio as the phone connects, and the service's log can no longer fill the head unit's
memory. Only the iDrive app (`libAAEntryNav.so`) changes. Nobody has tried this version in a car yet.

### Added

- **The iDrive switches to Bluetooth audio as the phone connects**, with the `select-bt-audio` file, a switch in Flare
  Drive. ... Not tried in a car yet.
```

- The heading is `## [X.Y.Z]`, ` - `, the release date (YYYY-MM-DD), then ` · PC` when it has only been tested on PC
  and under QEMU, or ` · car` once this exact build has run in a car.
- A short paragraph comes first: what the release does for someone in the car, which programs change, and whether it
  has been tried in a car. Anything that needs the reader to act goes at its start (see Action needed below).
- Then [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) categories as `### ` headings, in this order, skipping
  empty ones: Added, Changed, Fixed, Removed, Security.
- Each bullet opens with its point in bold, then the detail. Markdown is fine here: GitHub renders it.
- Each bullet says whether that change was tried in a car or only tested on PC. Nothing counts as working until a car
  has shown it.
- A version links to the commit its archive was built from, in the links at the end of the file. That commit is known
  only once `publish` has run, so the link is added at the next release, as 0.1.1's was for 0.1.0.

## The Flare Drive note

```
As your phone connects, the iDrive can switch to Bluetooth audio by itself (a switch in Flare Drive 2.0.6). The
service's log can no longer fill the head unit's memory.
```

- One paragraph of plain text, one to three sentences. Flare Drive and GitHub show it as it is, so no Markdown,
  bullets or headings.
- Only what someone in the car notices, with the most important change first.
- Name the Flare Drive switch or screen, never the file behind it: "a switch in Flare Drive", not `select-bt-audio`.
- Say which Flare Drive version a new switch needs, when an older one doesn't have it.
- Whether it is stable or experimental is not part of the note: `publish` records it (`--stable`) and Flare Drive
  shows it. Whether it is verified is not part of the note either: only the CHANGELOG records that.
  `publish_release.py notes` and `stability` change them later; edit the GitHub Release to match.

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
updating Flare Drive first, goes first: at the start of the CHANGELOG section's paragraph and of the Flare Drive note,
starting with "Action needed:" and saying what changed, who it affects and what to do. For example: "Action needed:
this version needs Flare Drive 2.0.6 or later, so update Flare Drive before installing it."

## Crediting contributors

When a change came from a pull request by someone other than the maintainer, credit them at the end of its bullet with
their GitHub username: "Thanks to @username." Credit the people whose pull request made the change, not those who
reported or suggested it, and keep the credit when the change was reworked before it was merged.

## Final check

- Is every bullet something a person in the car can notice, in the car's and Flare Drive's own words?
- Does every new feature say where to find it, and which Flare Drive version it needs?
- Does every bullet, and the section's heading, say whether it was tried in a car?
- Is anything that needs the reader to act first, starting with "Action needed:"?
- Is the Flare Drive note one plain paragraph, with no Markdown and no file names?
- Is anything that identifies a car left out?
