# Writing commit messages

Commit messages describe the change to the project for anyone reading the history, in English.

## Not release notes

The two have different readers:

|          | Commit messages                                                   | Release notes                                  |
| -------- | ----------------------------------------------------------------- | ---------------------------------------------- |
| For      | Developers reading the history                                    | People running AA-EntryNav in their car        |
| Says     | What changed in the project and why                               | What they can now do or notice in the car      |
| Covers   | Every change, including refactoring, tests, CI, build and tooling | Only what someone using it can notice          |
| Wording  | Technical terms, file, command and function names are fine        | The car's and Flare Drive's own words          |
| How many | One per change, as it's made                                      | One CHANGELOG section and one note per release |

So a commit message is not a CHANGELOG bullet, and the other way round. A change nobody in the car sees, such as
"Tag each published release on GitHub", still needs a clear commit message but gets no release note. How to write
release notes is in [release-notes.md](release-notes.md).

## Format

```
Move the service log aside past 1 MB

With logging on, the service writes to /tmp, which is RAM, and the log
grew for as long as the head unit ran. The iDrive app now moves it to
/tmp/aa-entrynav-service.old.log past 1 MB and empties it, so the two
never hold more than about 2 MB.
```

- **Subject:** one line that completes "If applied, this commit will ...". Start with a capitalised verb in the
  imperative ("Add", "Fix", "Rename", "Move"), keep it within 72 characters and leave off the full stop. No prefixes
  such as `fix:`, `feat(hmiapp):` or `ci:`: say "Speed up CI by ...".
- **Body:** optional, after a blank line. Explain why the change was made, since the diff already shows what changed.
  Mention effects and limitations a reader wouldn't guess, and say whether it was verified in a car or only tested on PC
  and under QEMU. Leave it out when the subject says it all.
- Wrap the body at 72 characters. A long URL goes on its own line.
- A GitHub issue goes on the last line of the body, as `Fixes` and its full URL, so it still makes sense outside
  GitHub.

## One change per commit

- One logical change per commit, however many files it touches. A bug fix is one commit, a feature is one commit.
- Each commit builds and passes `make check`. A change to code only the ARM tests cover also passes them under QEMU
  first (CLAUDE.md).
- Keep refactoring separate from changes in behaviour, and unrelated changes in separate commits, even when they're
  small or made together.
- An "and" in the subject is a hint that the commit does two things. Keep it only when both halves are one change.

## Writing them

- Be specific, so the subject means something on its own: "Fix the knob going dead after a Bluetooth reconnect", not
  "Fix bug" or "Update stuff".
- Name code the way developers will search for it: files, programs (`evo-trial`, `libAAEntryNav.so`), functions, the
  files in `/var/local/data/AA`.
- Say when a change only affects one firmware or one way of connecting: "On 202111011321, ...", "Over the cable, ...".
- No details of a particular computer or car: no local paths, machine setup, the car's password, VIN, IP or MAC
  addresses, or vehicle IDs.
- Commits are authored by CallMeLewis alone, with no co-author trailers.
- Don't amend or rewrite commits that have been pushed. Fix them with a new commit.

## Releases

A release commit bumps `VERSION` and adds the release's section to [CHANGELOG.md](../CHANGELOG.md).
`tools/publish_release.py prepare X.Y.Z "<summary>"` makes it. Its subject is
`Release`, the version and a short summary of what's in it, never the bare version number:

```
Release 0.1.1: Bluetooth audio as the phone connects, a smaller log
```

`tools/publish_release.py publish` then builds that commit, publishes it to Flare Drive's catalog, tags it
`vX.Y.Z` and gives the tag a GitHub Release.

## Final check

- Does the subject complete "If applied, this commit will ...", start with a capital and stay within 72 characters,
  with no prefix or full stop?
- Is there a blank line between the subject and the body?
- Does the body explain why, rather than retell the diff, and say whether it ran in a car?
- Does the commit hold one change, with refactoring and unrelated fixes split out?
- Is it free of local paths, machine details and anything that identifies the car?
