# Copyright (c) 2026 CallMeLewis
"""Publish a release for Flare Drive to download: https://updates.darkzone.dev/aa-entrynav/

  uv run --no-project --with cryptography python tools/publish_release.py keygen   create the signing key, once
  uv run --no-project --with cryptography python tools/publish_release.py init     publish an empty signed index, once
  uv run --no-project --with cryptography python tools/publish_release.py verify   check the live index and list it
  uv run --no-project --with cryptography python tools/publish_release.py prepare VERSION SUMMARY   the release commit
  uv run --no-project --with cryptography python tools/publish_release.py preview [VERSION]   show its two texts
  uv run --no-project --with cryptography python tools/publish_release.py publish [--stable]
  uv run --no-project --with cryptography python tools/publish_release.py notes VERSION   resync its texts
  uv run --no-project --with cryptography python tools/publish_release.py stability VERSION stable|experimental
  uv run --no-project --with cryptography python tools/publish_release.py retire VERSION... [--delete-archives]
  uv run --no-project --with cryptography python tools/publish_release.py tags     tag releases, with GitHub Releases

A release's texts come from its section of CHANGELOG.md (docs/release-notes.md): the Flare Drive note, written in it as
<!-- Flare Drive: ... -->, and the GitHub Release's text, made from the section itself. `preview` shows both without
publishing anything; `notes` brings a published release's note and GitHub Release back in line with the section.

`prepare` raises VERSION and commits it as `Release VERSION: SUMMARY`, and turns CHANGELOG.md's [Unreleased] part into
the version's section (CHANGELOG.md is outside git, so it is not in the commit). Push that commit, wait for CI, then
publish.

Each release carries the owner's label, stable or experimental (experimental unless --stable), shown as a pill in Flare
Drive. `stability` changes it later: the list is signed again, the archive stays as it is. Whether a release has run in
a car (verified or unverified) is recorded in CHANGELOG.md only, not in the list.

A version is X.Y.Z, or X.Y.Z-beta.N or X.Y.Z-test.N for a build to try before X.Y.Z (tools/release_version.py). Its
stable build X.Y.Z is always above them, in Flare Drive too: whoever installed 0.1.4-beta.2 is offered 0.1.4 as an
update. A beta or test build is always experimental (--stable and `stability ... stable` refuse it), and its GitHub
Release is a pre-release, never Latest.

`retire` takes releases off the list, which is signed again; Flare Drive stops offering them at its next check, and a
head unit that has one installed keeps it. With --delete-archives their archives are then deleted from the bucket.

`publish` builds what CI tested: it refuses a dirty tree, a HEAD whose CI has not passed, a version already published,
and a version whose CHANGELOG.md section or Flare Drive note is missing. It then builds the ARM programs, runs the QEMU
tests, validates and packages (README.md, Building), in Docker when not on Linux, uploads the archive, and only then the
signed index, so Flare Drive never sees an entry whose archive is missing. It then dates the version's CHANGELOG.md
heading with the day it was published and links the version to the commit its archive was built from.

Once the index lists a release, `publish` tags it v<version> on GitHub, at the commit its archive was built
from (the index's sourceCommit), push the tag, and give it a GitHub Release with the published archive and its SHA-256
(with `gh`, signed in to GitHub). An existing tag is never moved. `tags` does the same for every published release still
without a tag or a Release, taking each one's text from CHANGELOG.md, or its Flare Drive note when CHANGELOG.md has no
section for it.

The index is signed with Ed25519 by a key that never leaves this PC (~/.flare-drive/release-signing-key, or
FLARE_DRIVE_SIGNING_KEY). Flare Drive accepts an index only if its signature matches one of the public keys it was
built with (flare.releasePublicKeys). Back the key up: without it, no further release can be published.
Uploads use Wrangler, signed in with `wrangler login` (the Flare apps' shared account). design/release-updates.md in
Flare Drive describes the index.
"""

import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import release_version  # noqa: E402
import verify_arm  # noqa: E402

BUCKET = "darkzone-updates"
FEED = "aa-entrynav"
FEED_URL = f"https://updates.darkzone.dev/{FEED}"
WRANGLER = "wrangler@4.146.0"  # the version the Flare apps pin
REPO = "CallMeLewis/AA-EntryNav"
# Firmware this project's releases are tried on (tools/setup.sh TRIED_SOFTWARE). The index may narrow, never widen,
# what Flare Drive allows; the trial firmware still installs only releases Flare Drive itself has reviewed.
FIRMWARE = ["202111011321", "202306092158"]
# Flare Drive's version code for new releases: 20 is the first that lets the signed index approve a release for the
# trial firmware (19 still needed each release's SHA-256 built in, so it would list a new release and then refuse it).
MIN_APP = 20
# Outside git until the repository goes public. Each version's section holds its texts (docs/release-notes.md).
CHANGELOG = ROOT / "CHANGELOG.md"
HEADING = re.compile(r"^## \[([^\]]+)\]", re.M)
LINK = re.compile(r"^\[([^\]]+)\]: \S+$", re.M)
NOTE = re.compile(r"<!--\s*Flare Drive:(.*?)-->", re.S)


def key_path():
    return Path(os.environ.get("FLARE_DRIVE_SIGNING_KEY", Path.home() / ".flare-drive" / "release-signing-key"))


def load_key():
    path = key_path()
    if not path.is_file():
        sys.exit(f"FAIL: no signing key at {path}. Run keygen first, or set FLARE_DRIVE_SIGNING_KEY.")
    return Ed25519PrivateKey.from_private_bytes(base64.b64decode(path.read_text().strip()))


def public_b64(key):
    raw = key.public_key().public_bytes_raw()
    return base64.b64encode(raw).decode()


def run(*args, cwd=ROOT):
    print("+", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def output(*args):
    return subprocess.run([str(a) for a in args], cwd=ROOT, check=True, capture_output=True, text=True).stdout.strip()


def wrangler_object(action, key, *options):
    npx = shutil.which("npx")
    if not npx:
        sys.exit("FAIL: npx not found. Install Node.js, then sign in once with: npx wrangler@4.146.0 login")
    run(npx, "--yes", WRANGLER, "r2", "object", action, f"{BUCKET}/{FEED}/{key}", "--remote", *options)


def wrangler_put(key, file, content_type, cache_control):
    wrangler_object("put", key, "--file", file, "--content-type", content_type, "--cache-control", cache_control)


def fetch(name):
    """The live file, or None when it does not exist yet. Bypasses Cloudflare's cache."""
    url = f"{FEED_URL}/{name}?t={datetime.now().timestamp()}"
    request = urllib.request.Request(url, headers={"User-Agent": "FlareDrivePublish"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise


def live_index(key):
    """The published index after checking its signature, or None if none is published."""
    index, signature = fetch("index.json"), fetch("index.json.sig")
    if index is None:
        return None
    if signature is None:
        sys.exit("FAIL: index.json is published without index.json.sig. Fix the bucket by hand before publishing.")
    try:
        key.public_key().verify(base64.b64decode(signature), index)
    except InvalidSignature:
        sys.exit("FAIL: the published index is not signed by this key. Nothing was changed.")
    index = json.loads(index)
    # Lists signed before 2026-11-12 record whether each release ran in a car (triedInCar). Nothing reads it and
    # CHANGELOG.md records it, so the next signed list leaves it out.
    for release in index["releases"]:
        release.pop("triedInCar", None)
    return index


def publish_index(key, index):
    data = (json.dumps(index, indent=2) + "\n").encode()
    signature = base64.b64encode(key.sign(data)).decode() + "\n"
    key.public_key().verify(base64.b64decode(signature), data)
    with tempfile.TemporaryDirectory() as folder:
        index_file, sig_file = Path(folder) / "index.json", Path(folder) / "index.json.sig"
        index_file.write_bytes(data)
        sig_file.write_text(signature)
        # The signature first: an app fetching between the two uploads rejects the pair and keeps its last index.
        wrangler_put("index.json.sig", sig_file, "text/plain", "public, max-age=60")
        wrangler_put("index.json", index_file, "application/json", "public, max-age=60")


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def read_changelog():
    if not CHANGELOG.is_file():
        sys.exit(f"FAIL: no {CHANGELOG.name} here. It is kept outside git: copy it in from wherever it is kept.")
    return CHANGELOG.read_text(encoding="utf-8")


def write_changelog(text):
    CHANGELOG.write_text(text, encoding="utf-8", newline="\n")


def section_span(text, version):
    """Where the section headed ## [version] starts and ends in text, heading included, or None."""
    for heading in HEADING.finditer(text):
        if heading.group(1) == version:
            ends = [m.start() for m in (HEADING.search(text, heading.end()), LINK.search(text, heading.end())) if m]
            return heading.start(), min(ends, default=len(text))
    return None


def find_section(version):
    """The version's CHANGELOG.md section, heading included, or None when there is none (or no CHANGELOG.md)."""
    if not CHANGELOG.is_file():
        return None
    text = read_changelog()
    span = section_span(text, version)
    return text[span[0] : span[1]] if span else None


def section(version):
    found = find_section(version)
    if found is None:
        sys.exit(f"FAIL: {CHANGELOG.name} has no section for {version}. Write it following docs/release-notes.md.")
    return found


def flare_drive_note(version, text):
    """The Flare Drive note written in the section as <!-- Flare Drive: ... -->, as one line."""
    match = NOTE.search(text)
    note = " ".join(match.group(1).split()) if match else ""
    if not note:
        sys.exit(f"FAIL: {CHANGELOG.name}'s {version} section has no Flare Drive note: <!-- Flare Drive: ... -->.")
    return note


def without_verification(text):
    """text without its verified or unverified wording: "This version is unverified." anywhere, and a closing sentence
    that starts with Verified or Unverified. Only CHANGELOG.md records whether a release has run in a car."""
    text = re.sub(r"(^|(?<=[.!?]))\s*This version is (un)?verified\b[^.]*\.", "", text)
    return re.sub(r"(^|(?<=[.!?]))\s*(Unverified|Verified)\b[^.]*\.$", "", text).strip()


def release_body(text):
    """The GitHub Release's text, made from a CHANGELOG.md section: its heading and Flare Drive note left out, an
    "Action needed:" paragraph as an IMPORTANT callout, ### headings raised to ##, verified or unverified wording left
    out, and each paragraph or bullet on one line, since GitHub shows hard line breaks."""
    blocks = []  # paragraphs, bullets and headings, unwrapped; "" for a blank line
    for line in NOTE.sub("", text).splitlines()[1:]:
        line = line.strip()
        if not line or line.startswith(("- ", "#")) or not blocks or not blocks[-1] or blocks[-1].startswith("#"):
            blocks.append(line)
        else:
            blocks[-1] += " " + line
    lines = []
    for block in blocks:
        if block.startswith("### "):
            block = block[1:]
        elif block.startswith("Action needed:"):
            action = without_verification(block[len("Action needed:") :])
            block = f"> [!IMPORTANT]\n> {action[:1].upper()}{action[1:]}"
        elif block.startswith("- "):
            block = "- " + without_verification(block[2:])
            if block == "- ":
                continue
        elif block:
            block = without_verification(block)
            if not block:
                continue
        if block or (lines and lines[-1]):
            lines.append(block)
    return "\n".join(lines).strip() + "\n"


def with_links(text, index, unreleased_from=None):
    """text with a link for every published version that has none, to the commit its archive was built from, and with
    [Unreleased] comparing from v<unreleased_from> when given. The links stay at the end: [Unreleased], then the
    versions newest first."""
    lines = text.rstrip("\n").split("\n")
    links = {}
    while lines and (not lines[-1].strip() or LINK.match(lines[-1])):
        line = lines.pop()
        if line.strip():
            links[LINK.match(line).group(1)] = line
    for release in index["releases"]:
        if release["version"] not in links:
            links[release["version"]] = (
                f"[{release['version']}]: https://github.com/{REPO}/tree/{release['sourceCommit']}"
            )
    if unreleased_from:
        links["Unreleased"] = f"[Unreleased]: https://github.com/{REPO}/compare/v{unreleased_from}...HEAD"
    versions = sorted((label for label in links if release_version.is_valid(label)), key=version_key, reverse=True)
    others = [label for label in links if label != "Unreleased" and label not in versions]
    order = (["Unreleased"] if "Unreleased" in links else []) + versions + others
    return "\n".join(lines).rstrip("\n") + "\n\n" + "\n".join(links[label] for label in order) + "\n"


def prepare(args):
    """The release commit: VERSION raised and committed as `Release VERSION: SUMMARY`, then CHANGELOG.md's [Unreleased]
    part turned into the version's section, with any published version still missing its link given one."""
    version = args.version
    if not release_version.is_valid(version):
        sys.exit(f"FAIL: {version} is not {release_version.FORMAT}.")
    subject = f"Release {version}: {args.summary}"
    if len(subject) > 72:
        sys.exit(f"FAIL: the subject is {len(subject)} characters, more than 72: {subject}")
    if output("git", "status", "--porcelain"):
        sys.exit("FAIL: the working tree has changes. The release commit holds VERSION only.")
    current = (ROOT / "VERSION").read_text().strip()
    if version_key(version) <= version_key(current):
        sys.exit(f"FAIL: {version} is not above the current version, {current}.")
    index = live_index(load_key())
    if index is None:
        sys.exit("FAIL: no index is published yet. Run init first.")
    if any(r["version"] == version for r in index["releases"]):
        sys.exit(f"FAIL: {version} is already published.")
    text = read_changelog()
    if section_span(text, version):
        sys.exit(f"FAIL: {CHANGELOG.name} already has a section for {version}.")
    unreleased = section_span(text, "Unreleased")
    if unreleased is None:
        sys.exit(f"FAIL: {CHANGELOG.name} has no [Unreleased] section.")
    if not text[unreleased[0] : unreleased[1]].split("\n", 1)[1].strip():
        sys.exit(f"FAIL: nothing under [Unreleased] in {CHANGELOG.name}. Write the release's section there first.")

    (ROOT / "VERSION").write_text(version + "\n", newline="\n")
    committed = git("commit", "--quiet", "-m", subject, "--", "VERSION", check=False)
    if committed.returncode:
        git("checkout", "--", "VERSION", check=False)
        sys.exit(f"FAIL: could not commit VERSION: {(committed.stderr or committed.stdout).strip()}")
    print(f"Committed {output('git', 'rev-parse', '--short', 'HEAD')}: {subject}")

    heading_end = text.index("\n", unreleased[0]) + 1
    date = datetime.now().strftime("%Y-%m-%d")
    text = text[:heading_end] + f"\n## [{version}] - {date} · Unverified\n" + text[heading_end:]
    write_changelog(with_links(text, index, unreleased_from=version))
    print(f"{CHANGELOG.name}: [Unreleased] is now {version}'s section, unverified.")
    if not NOTE.search(section(version)):
        print(f"WARN: {version}'s section has no Flare Drive note (<!-- Flare Drive: ... -->) yet. publish needs one.")
    print("Check both texts with preview, push the commit, wait for CI, then publish.")


def preview(args):
    """A version's Flare Drive note and GitHub Release text, as publish would use them. Nothing is published."""
    version = args.version or (ROOT / "VERSION").read_text().strip()
    text = section(version)
    print(f"Flare Drive note for {version}:\n\n{flare_drive_note(version, text)}\n")
    print(f"GitHub Release text for {version}:\n\n{release_body(text)}", end="")


def keygen(_):
    path = key_path()
    if path.exists():
        sys.exit(f"FAIL: {path} already exists. A new key would stop installed Flare Drive from trusting releases.")
    path.parent.mkdir(parents=True, exist_ok=True)
    key = Ed25519PrivateKey.generate()
    path.write_text(base64.b64encode(key.private_bytes_raw()).decode() + "\n")
    if os.name != "nt":
        path.chmod(0o600)
    print(f"Signing key written to {path}. Back it up somewhere safe; it never goes into git.")
    print(f"Public key, for flare.releasePublicKeys in ~/.gradle/gradle.properties:\n{public_b64(key)}")


def init(_):
    key = load_key()
    if live_index(key) is not None:
        sys.exit("FAIL: an index is already published. Use verify to see it.")
    publish_index(key, {"schema": 1, "published": now(), "releases": []})
    verify(None)


def verify(_):
    key = load_key()
    index = live_index(key)
    if index is None:
        sys.exit("No index is published yet. Run init.")
    print(f"{FEED_URL}/index.json: signature OK, published {index['published']}, public key {public_b64(key)}")
    for release in index["releases"]:
        label = release.get("stability") or "no label"
        sha, size = release["sha256"][:12], release["size"]
        print(f"  {release['version']}  {sha}…  {size} bytes  {label}  {release['published']}")
    if not index["releases"]:
        print("  (no releases yet)")


def require_ready():
    if output("git", "status", "--porcelain"):
        sys.exit("FAIL: the working tree has changes. Publish only what is committed.")
    head = output("git", "rev-parse", "HEAD")
    if not output("git", "branch", "-r", "--contains", head):
        sys.exit("FAIL: HEAD is not pushed, so CI has not tested it. Push it and wait for CI.")
    query = "[.check_runs[] | {name, status, conclusion}]"
    runs = json.loads(output("gh", "api", f"repos/{REPO}/commits/{head}/check-runs", "--jq", query))
    if not runs:
        sys.exit(f"FAIL: CI has not run on {head[:7]}.")
    failed = [r["name"] for r in runs if r["status"] != "completed" or r["conclusion"] != "success"]
    if failed:
        sys.exit(f"FAIL: CI has not passed on {head[:7]}: {', '.join(failed)}.")
    return head


def publish(args):
    key = load_key()
    index = live_index(key)
    if index is None:
        sys.exit("FAIL: no index is published yet. Run init first.")
    head = require_ready()
    version = (ROOT / "VERSION").read_text().strip()
    if any(r["version"] == version for r in index["releases"]):
        sys.exit(f"FAIL: {version} is already published. Raise VERSION for a new release; a version is never replaced.")
    if not release_version.is_valid(version):
        sys.exit(f"FAIL: VERSION says {version}, which is not {release_version.FORMAT}.")
    if args.stable and release_version.is_prerelease(version):
        sys.exit(f"FAIL: {version} is a beta or test build: it is always experimental, never --stable.")
    # Both texts before the build, which takes minutes: a missing section or note stops it at once.
    text = section(version)
    note, body = flare_drive_note(version, text), release_body(text)
    print(f"Flare Drive note: {note}", flush=True)
    # The build and every check CI can't run (tools/verify_arm.py, as the pre-push hook runs them), then the
    # archive. Each step stops the publish on failure. Packaging runs on Linux too: zipped elsewhere, the iDrive app's
    # zips hold the same files in different bytes. A clean build: objects left from an earlier build would otherwise be
    # shipped as this commit's.
    verify_arm.run_all(ROOT, (*verify_arm.CHECKS, "tools/package_release.py"), clean=True)
    archive = ROOT / "artifacts" / f"aa-entrynav-{version}-armhf.tar"
    upload(key, index, archive, version, head, args.stable, note, body)


def notes(args):
    """Brings one published release's Flare Drive note and GitHub Release text back in line with its CHANGELOG.md
    section. The list is signed again only when the note changed; the archives stay as they are."""
    key = load_key()
    index = live_index(key)
    if index is None:
        sys.exit("FAIL: no index is published yet.")
    entries = [r for r in index["releases"] if r["version"] == args.version]
    if not entries:
        sys.exit(f"FAIL: {args.version} is not published.")
    text = section(args.version)
    note, body = flare_drive_note(args.version, text), release_body(text)
    if entries[0].get("notes") == note:
        print("The Flare Drive note already matches CHANGELOG.md.")
    else:
        entries[0]["notes"] = note
        publish_index(key, {**index, "published": now()})
    tag = f"v{args.version}"
    edited = subprocess.run(
        ["gh", "release", "edit", tag, "-R", REPO, "--notes", body], capture_output=True, text=True, check=False
    )
    if edited.returncode:
        print(f"WARN: the GitHub Release {tag} was not updated: {edited.stderr.strip()}")
    else:
        print(f"GitHub Release {tag} updated.")
    verify(None)


def stability(args):
    """Sets one release's label, stable or experimental, in the live index and signs it again."""
    key = load_key()
    index = live_index(key)
    if index is None:
        sys.exit("FAIL: no index is published yet.")
    entries = [r for r in index["releases"] if r["version"] == args.version]
    if not entries:
        sys.exit(f"FAIL: {args.version} is not published.")
    if args.label == "stable" and release_version.is_prerelease(args.version):
        sys.exit(
            f"FAIL: {args.version} is a beta or test build: it stays experimental. Publish X.Y.Z as stable instead."
        )
    entries[0]["stability"] = args.label
    publish_index(key, {**index, "published": now()})
    verify(None)


def retire(args):
    """Takes releases off the live index and signs it again. With --delete-archives, deletes their archives from the
    bucket afterwards, so no list ever names an archive that is gone."""
    key = load_key()
    index = live_index(key)
    if index is None:
        sys.exit("FAIL: no index is published yet.")
    missing = sorted(set(args.versions) - {r["version"] for r in index["releases"]})
    if missing:
        sys.exit(f"FAIL: not published: {', '.join(missing)}. Nothing was changed.")
    gone = [r for r in index["releases"] if r["version"] in args.versions]
    kept = [r for r in index["releases"] if r["version"] not in args.versions]
    publish_index(key, {**index, "published": now(), "releases": kept})
    print(f"Retired {', '.join(r['version'] for r in gone)}.")
    if args.delete_archives:
        for entry in gone:
            wrangler_object("delete", entry["file"])
    verify(None)


def upload(key, index, archive, version, commit, stable, note, body):
    """Uploads the archive, reads it back, then signs and uploads the index with its entry added."""
    data = archive.read_bytes()
    with tarfile.open(archive) as tar:
        recorded = tar.extractfile(f"aa-entrynav-{version}/opt/aa/VERSION").read().decode().strip()
    if recorded != version:
        sys.exit(f"FAIL: the archive says {recorded}, not {version}.")
    name = f"aa-entrynav-{version}-armhf.tar"
    entry = {
        "version": version,
        "file": f"releases/{version}/{name}",
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "sourceCommit": commit,
        "published": now(),
        "firmware": FIRMWARE,
        "stability": "stable" if stable else "experimental",
        "minApp": MIN_APP,
        "notes": note,
    }
    wrangler_put(entry["file"], archive, "application/x-tar", "public, max-age=31536000, immutable")
    if fetch(entry["file"]) != data:
        sys.exit("FAIL: the uploaded archive doesn't read back the same. The index was not changed.")
    publish_index(key, {"schema": 1, "published": now(), "releases": index["releases"] + [entry]})
    print(f"Published {version} ({entry['sha256'][:12]}…) from {commit[:7]}.")
    record_published(entry, index)
    if tag_release(entry):
        github_release(entry, body, latest=newest_stable(index["releases"], version))
    verify(None)


def record_published(entry, index):
    """Dates the version's CHANGELOG.md heading with the day it was published and links the version to the commit its
    archive was built from."""
    text = read_changelog()
    day = entry["published"][:10]
    heading = re.compile(rf"^(## \[{re.escape(entry['version'])}\] - )\d{{4}}-\d{{2}}-\d{{2}}", re.M)
    text = heading.sub(rf"\g<1>{day}", text, count=1)
    write_changelog(with_links(text, {"releases": index["releases"] + [entry]}))
    print(f"{CHANGELOG.name}: {entry['version']} dated {day} and linked to {entry['sourceCommit'][:7]}.")


def git(*args, check=True):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=check)


def version_key(version):
    """Release order: a stable build above its beta and test builds (tools/release_version.py)."""
    return release_version.key(version)


def newest_stable(releases, version):
    """version is a stable build above every stable build in releases: the one GitHub marks Latest."""
    if release_version.is_prerelease(version):
        return False
    return all(
        version_key(r["version"]) < version_key(version)
        for r in releases
        if r["version"] != version and not release_version.is_prerelease(r["version"])
    )


def tag_release(entry):
    """Tags v<version> on GitHub at the commit the published archive was built from, once the index lists it. An
    existing tag is never moved: one on another commit is reported. A failure is reported, never fatal: the release is
    already published, and `tags` makes up for it. True once the tag is on GitHub at that commit."""
    version, commit = entry["version"], entry["sourceCommit"]
    tag = f"v{version}"
    existing = git("rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}", check=False).stdout.strip()
    if existing and existing != commit:
        print(f"WARN: {tag} already points at {existing[:7]}, not {commit[:7]}, which {version} was built from.")
        print("  Left as it is, without a GitHub Release.")
        return False
    if not existing:
        message = (
            f"AA-EntryNav {version}\n\nArchive SHA-256: {entry['sha256']}\n"
            f"Published to Flare Drive's release list {entry['published']}.\n"
        )
        made = git("tag", "-a", tag, commit, "-m", message, check=False)
        if made.returncode:
            print(f"WARN: could not tag {tag}: {made.stderr.strip()}")
            return False
    pushed = git("push", "origin", f"refs/tags/{tag}", check=False)
    if pushed.returncode:
        print(f"WARN: {tag} is tagged here but not on GitHub: {pushed.stderr.strip()}")
        print(f"  Push it with: git push origin {tag}")
        return False
    print(f"Tagged {tag} at {commit[:7]} on GitHub.")
    return True


def github_release(entry, body, latest):
    """A GitHub Release for the tag, with body as its text, the archive Flare Drive installs and its SHA-256 file. The
    archive is downloaded from the bucket and must match the index's SHA-256, never taken from artifacts/, which a later
    build may have replaced. Only [latest] is marked Latest; a beta or test build is marked a pre-release. An existing
    Release is left as it is (`notes` updates its text); a failure is reported."""
    version, tag = entry["version"], f"v{entry['version']}"
    if subprocess.run(["gh", "release", "view", tag, "-R", REPO], capture_output=True).returncode == 0:
        print(f"{tag} already has a GitHub Release.")
        return
    data = fetch(entry["file"])
    if data is None or hashlib.sha256(data).hexdigest() != entry["sha256"]:
        print(f"WARN: the published {version} archive doesn't match the index's SHA-256: no GitHub Release for {tag}.")
        return
    name = Path(entry["file"]).name
    with tempfile.TemporaryDirectory() as folder:
        archive, sums = Path(folder) / name, Path(folder) / f"{name}.sha256"
        archive.write_bytes(data)
        sums.write_text(f"{entry['sha256']}  {name}\n", newline="\n")
        made = subprocess.run(
            ["gh", "release", "create", tag, str(archive), str(sums), "-R", REPO, "--verify-tag"]
            + ["--title", f"AA-EntryNav {version}", "--notes", body, f"--latest={'true' if latest else 'false'}"]
            + (["--prerelease"] if release_version.is_prerelease(version) else []),
            capture_output=True,
            text=True,
        )
    if made.returncode:
        print(f"WARN: no GitHub Release for {tag}: {made.stderr.strip()}")
        return
    print(f"GitHub Release {tag}{' (Latest)' if latest else ''}: {made.stdout.strip()}")


def tags(args):
    """Tags every published release that has no tag yet, from the live index, and gives each tag a GitHub Release,
    oldest first, with the newest stable build marked Latest. Its text comes from CHANGELOG.md, or is its Flare Drive
    note when CHANGELOG.md has no section for it."""
    index = live_index(load_key())
    if index is None:
        sys.exit("FAIL: no index is published yet.")
    git("fetch", "--tags", "origin", check=False)
    releases = sorted(index["releases"], key=lambda r: version_key(r["version"]))
    for entry in releases:
        if tag_release(entry):
            text = find_section(entry["version"])
            body = release_body(text) if text else (f"{entry['notes']}\n" if entry.get("notes") else "")
            github_release(entry, body, latest=newest_stable(releases, entry["version"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("keygen").set_defaults(func=keygen)
    commands.add_parser("init").set_defaults(func=init)
    commands.add_parser("verify").set_defaults(func=verify)
    commands.add_parser("tags").set_defaults(func=tags)
    pr = commands.add_parser("prepare")
    pr.add_argument("version", help="the new version, X.Y.Z")
    pr.add_argument("summary", help="what's in it, for the commit subject `Release X.Y.Z: SUMMARY`")
    pr.set_defaults(func=prepare)
    pv = commands.add_parser("preview")
    pv.add_argument("version", nargs="?", help="a version with a CHANGELOG.md section (default: VERSION's)")
    pv.set_defaults(func=preview)
    p = commands.add_parser("publish")
    p.add_argument("--stable", action="store_true", help="label it stable in Flare Drive (default: experimental)")
    p.set_defaults(func=publish)
    n = commands.add_parser("notes")
    n.add_argument("version", help="a published version")
    n.set_defaults(func=notes)
    s = commands.add_parser("stability")
    s.add_argument("version", help="a published version")
    s.add_argument("label", choices=["stable", "experimental"], help="shown as a pill in Flare Drive")
    s.set_defaults(func=stability)
    r = commands.add_parser("retire")
    r.add_argument("versions", nargs="+", help="published versions to take off the list")
    r.add_argument("--delete-archives", action="store_true", help="then delete their archives from the bucket")
    r.set_defaults(func=retire)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
