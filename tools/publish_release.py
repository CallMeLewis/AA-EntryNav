# Copyright (c) 2026 CallMeLewis
"""Publish a release for Flare Drive to download: https://updates.darkzone.dev/aa-entrynav/

  uv run --no-project --with cryptography python tools/publish_release.py keygen   create the signing key, once
  uv run --no-project --with cryptography python tools/publish_release.py init     publish an empty signed index, once
  uv run --no-project --with cryptography python tools/publish_release.py verify   check the live index and list it
  uv run --no-project --with cryptography python tools/publish_release.py publish [OPTIONS]
  uv run --no-project --with cryptography python tools/publish_release.py notes VERSION TEXT   rewrite its notes
  uv run --no-project --with cryptography python tools/publish_release.py stability VERSION stable|experimental
  uv run --no-project --with cryptography python tools/publish_release.py tried VERSION   it has run in a car
  uv run --no-project --with cryptography python tools/publish_release.py retire VERSION... [--delete-archives]
  uv run --no-project --with cryptography python tools/publish_release.py tags     tag releases, with GitHub Releases

OPTIONS: --notes TEXT, --tried-in-car, --stable. Each release carries the owner's label, stable or experimental
(experimental unless --stable), shown as a pill in Flare Drive. `stability` changes it later, and `tried` records
that a release published without --tried-in-car has since run in a car: the list is signed again, the archive stays as
it is.

`retire` takes releases off the list, which is signed again; Flare Drive stops offering them at its next check, and a
head unit that has one installed keeps it. With --delete-archives their archives are then deleted from the bucket.

`publish` builds what CI tested: it refuses a dirty tree, a HEAD whose CI has not passed, and a version already
published. It then builds the ARM programs, runs the QEMU tests, validates and packages (README.md, Building), in
Docker when not on Linux, uploads the archive, and only then the signed index, so Flare Drive never sees an entry whose
archive is missing.

Once the index lists a release, `publish` tags it v<version> on GitHub, at the commit its archive was built
from (the index's sourceCommit), push the tag, and give it a GitHub Release with the published archive and its SHA-256
(with `gh`, signed in to GitHub). An existing tag is never moved. `tags` does the same for every published release still
without a tag or a Release.

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
    return json.loads(index)


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
        tried = "tried in a car" if release.get("triedInCar") else "not yet tried in a car"
        label = release.get("stability") or "no label"
        sha, size = release["sha256"][:12], release["size"]
        print(f"  {release['version']}  {sha}…  {size} bytes  {label}, {tried}  {release['published']}")
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
    # The build and every check CI can't run (tools/verify_arm.py, as the pre-push hook runs them), then the
    # archive. Each step stops the publish on failure. Packaging runs on Linux too: zipped elsewhere, the iDrive app's
    # zips hold the same files in different bytes.
    verify_arm.run_all(ROOT, (*verify_arm.CHECKS, "tools/package_release.py"))
    upload(key, index, ROOT / "artifacts" / f"aa-entrynav-{version}-armhf.tar", version, head, args)


def notes(args):
    """Replaces one release's notes in the live index and signs it again. The archives stay as they are."""
    key = load_key()
    index = live_index(key)
    if index is None:
        sys.exit("FAIL: no index is published yet.")
    entries = [r for r in index["releases"] if r["version"] == args.version]
    if not entries:
        sys.exit(f"FAIL: {args.version} is not published.")
    entries[0]["notes"] = args.text
    publish_index(key, {**index, "published": now()})
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
    entries[0]["stability"] = args.label
    publish_index(key, {**index, "published": now()})
    verify(None)


def tried(args):
    """Records in the live index that one release has run in a car, and signs it again."""
    key = load_key()
    index = live_index(key)
    if index is None:
        sys.exit("FAIL: no index is published yet.")
    entries = [r for r in index["releases"] if r["version"] == args.version]
    if not entries:
        sys.exit(f"FAIL: {args.version} is not published.")
    if entries[0].get("triedInCar"):
        sys.exit(f"{args.version} is already recorded as tried in a car. Nothing was changed.")
    entries[0]["triedInCar"] = True
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


def upload(key, index, archive, version, commit, args):
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
        "triedInCar": args.tried_in_car,
        "stability": "stable" if args.stable else "experimental",
        "minApp": MIN_APP,
        "notes": args.notes,
    }
    wrangler_put(entry["file"], archive, "application/x-tar", "public, max-age=31536000, immutable")
    if fetch(entry["file"]) != data:
        sys.exit("FAIL: the uploaded archive doesn't read back the same. The index was not changed.")
    publish_index(key, {"schema": 1, "published": now(), "releases": index["releases"] + [entry]})
    print(f"Published {version} ({entry['sha256'][:12]}…) from {commit[:7]}.")
    if tag_release(entry):
        github_release(entry, latest=all(version_key(r) < version_key(entry) for r in index["releases"]))
    verify(None)


def git(*args, check=True):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=check)


def version_key(entry):
    return tuple(int(part) for part in entry["version"].split("."))


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


def github_release(entry, latest):
    """A GitHub Release for the tag, with the archive Flare Drive installs and its SHA-256 file. The archive is
    downloaded from the bucket and must match the index's SHA-256, never taken from artifacts/, which a later build may
    have replaced. Only [latest] is marked Latest. An existing Release is left as it is; a failure is reported."""
    version, tag = entry["version"], f"v{entry['version']}"
    if subprocess.run(["gh", "release", "view", tag, "-R", REPO], capture_output=True).returncode == 0:
        print(f"{tag} already has a GitHub Release.")
        return
    data = fetch(entry["file"])
    if data is None or hashlib.sha256(data).hexdigest() != entry["sha256"]:
        print(f"WARN: the published {version} archive doesn't match the index's SHA-256: no GitHub Release for {tag}.")
        return
    name = Path(entry["file"]).name
    body = f"{entry['notes']}\n" if entry.get("notes") else ""
    with tempfile.TemporaryDirectory() as folder:
        archive, sums = Path(folder) / name, Path(folder) / f"{name}.sha256"
        archive.write_bytes(data)
        sums.write_text(f"{entry['sha256']}  {name}\n", newline="\n")
        made = subprocess.run(
            ["gh", "release", "create", tag, str(archive), str(sums), "-R", REPO, "--verify-tag"]
            + ["--title", f"AA-EntryNav {version}", "--notes", body, f"--latest={'true' if latest else 'false'}"],
            capture_output=True,
            text=True,
        )
    if made.returncode:
        print(f"WARN: no GitHub Release for {tag}: {made.stderr.strip()}")
        return
    print(f"GitHub Release {tag}{' (Latest)' if latest else ''}: {made.stdout.strip()}")


def tags(args):
    """Tags every published release that has no tag yet, from the live index, and gives each tag a GitHub Release,
    oldest first, with the newest marked Latest."""
    index = live_index(load_key())
    if index is None:
        sys.exit("FAIL: no index is published yet.")
    git("fetch", "--tags", "origin", check=False)
    releases = sorted(index["releases"], key=version_key)
    for entry in releases:
        if tag_release(entry):
            github_release(entry, latest=entry is releases[-1])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("keygen").set_defaults(func=keygen)
    commands.add_parser("init").set_defaults(func=init)
    commands.add_parser("verify").set_defaults(func=verify)
    commands.add_parser("tags").set_defaults(func=tags)
    p = commands.add_parser("publish")
    p.add_argument("--notes", default="", help="one short paragraph, shown in Flare Drive")
    p.add_argument("--tried-in-car", action="store_true", help="only once this exact build has run in a car")
    p.add_argument("--stable", action="store_true", help="label it stable in Flare Drive (default: experimental)")
    p.set_defaults(func=publish)
    n = commands.add_parser("notes")
    n.add_argument("version", help="a published version")
    n.add_argument("text", help="one short paragraph, shown in Flare Drive")
    n.set_defaults(func=notes)
    s = commands.add_parser("stability")
    s.add_argument("version", help="a published version")
    s.add_argument("label", choices=["stable", "experimental"], help="shown as a pill in Flare Drive")
    s.set_defaults(func=stability)
    t = commands.add_parser("tried")
    t.add_argument("version", help="a published version that has since run in a car")
    t.set_defaults(func=tried)
    r = commands.add_parser("retire")
    r.add_argument("versions", nargs="+", help="published versions to take off the list")
    r.add_argument("--delete-archives", action="store_true", help="then delete their archives from the bucket")
    r.set_defaults(func=retire)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
