# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Build the release archive: artifacts/aa-entrynav-<version>-armhf.tar, ready to copy to the head unit.

Its files sit at the paths they take on the car (opt/aa/..., opt/hmi/data/ro/Plugins/AAEntryNav/...), with MD5SUMS
checkable by the car's BusyBox before and after the copy, and setup.sh at the top, which checks and installs them
(docs/INSTALLATION.md). Only the programs recorded by the last validation (make verify_binary verify_wireless) go in.
The files iDrive reads itself (manifest, description, text and image zips) are parsed here first: nothing on the car
could stop iDrive from tripping over a damaged one. The archive is reproducible: fixed owner, modes and dates.
"""

import io
import json
import subprocess
import sys
import tarfile
import zipfile
from hashlib import md5, sha256
from pathlib import Path
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[1]
PLUGIN = "opt/hmi/data/ro/Plugins/AAEntryNav"
UPSTREAM_NOTICE = """\
# Licence

AA-EntryNav started from entry-aad (https://github.com/Jiraiya43/entry-aad) by Jiraiya43, a port of
Supra-iDrive6-Android-Auto (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto) by Sq00ky, which began as NBT-AAD
by nanaisu. The code taken from them is under the PolyForm Noncommercial License 1.0.0:
https://polyformproject.org/licenses/noncommercial/1.0.0

> Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
>
> Required Notice: Copyright (c) 2026 Jiraiya43 - entry-aad

No warranty: installing this modifies the head unit and may affect its warranty.
"""
PLUGIN_FILES = (
    "libAAEntryNav.so",
    "AAEntryNav.manifest",
    "AAEntryNav.xml",
    "AAEntryNav_ALL_Texts.zip",
    "AAEntryNav_ALL_Images.zip",
)
PROGRAMS = ("aa-entrynav", "evo-trial")
EXECUTABLES = {f"opt/aa/{name}" for name in PROGRAMS} | {f"{PLUGIN}/libAAEntryNav.so"}
SETUP = ROOT / "tools/setup.sh"


def validated_programs() -> dict:
    """The ARM programs, exactly those recorded by the last validation."""
    report_path = ROOT / "artifacts/wireless-validation.json"
    if not report_path.is_file():
        raise SystemExit(f"FAIL: {report_path} missing: run make verify_binary verify_wireless first")
    report = json.loads(report_path.read_text())
    programs = {}
    for name in PROGRAMS:
        data = (ROOT / name).read_bytes()
        if report["binaries"][name]["sha256"] != sha256(data).hexdigest():
            raise SystemExit(f"FAIL: {name} differs from the validated one: rebuild, then make verify_wireless")
        programs[f"opt/aa/{name}"] = data
    return programs


def check_idrive_files(files: dict) -> None:
    """What HmiMain3D parses before any code of ours runs: well-formed XML, complete zips, the manifest's names."""
    for name in ("AAEntryNav.manifest", "AAEntryNav.xml"):
        try:
            root = ElementTree.fromstring(files[f"{PLUGIN}/{name}"])
        except ElementTree.ParseError as error:
            raise SystemExit(f"FAIL: hmiapp/{name} is not well-formed XML: {error}") from None
        if name == "AAEntryNav.manifest" and (root.tag != "plugin" or root.findtext("name") != "AAEntryNav"):
            raise SystemExit("FAIL: hmiapp/AAEntryNav.manifest does not describe the plugin AAEntryNav")
    for name in ("AAEntryNav_ALL_Texts.zip", "AAEntryNav_ALL_Images.zip"):
        try:
            with zipfile.ZipFile(io.BytesIO(files[f"{PLUGIN}/{name}"])) as archive:
                damaged = archive.testzip()
                empty = not archive.namelist()
        except zipfile.BadZipFile as error:
            raise SystemExit(f"FAIL: hmiapp/{name} is not a zip: {error}") from None
        if damaged or empty:
            raise SystemExit(f"FAIL: hmiapp/{name} is damaged or empty ({damaged or 'no file'})")


def setup_script() -> bytes:
    """tools/setup.sh, as the car's BusyBox reads it: ASCII, Unix line ends."""
    data = SETUP.read_bytes()
    if b"\r" in data or not data.isascii() or not data.startswith(b"#!/bin/sh\n"):
        raise SystemExit("FAIL: tools/setup.sh must be ASCII with Unix line ends, starting with #!/bin/sh")
    return data


def release_files(version: str) -> dict:
    subprocess.run(
        [sys.executable, "-I", str(ROOT / "hmiapp/make_db.py"), str(ROOT / "hmiapp/res"), str(ROOT / "hmiapp")],
        check=True,
    )
    files = {f"{PLUGIN}/{name}": (ROOT / "hmiapp" / name).read_bytes() for name in PLUGIN_FILES}
    if not files[f"{PLUGIN}/libAAEntryNav.so"].startswith(b"\x7fELF"):
        raise SystemExit("FAIL: hmiapp/libAAEntryNav.so is not an ARM build")
    check_idrive_files(files)
    files.update(validated_programs())
    files["opt/aa/setup.sh"] = setup_script()
    files["opt/aa/VERSION"] = f"{version}\n".encode()
    files["setup.sh"] = files["opt/aa/setup.sh"]
    files["MD5SUMS"] = "".join(f"{md5(data).hexdigest()}  {name}\n" for name, data in sorted(files.items())).encode()
    # Flare Drive's ReleasePackage expects LICENSE.md. It carries the licence and notices of the code taken from
    # earlier projects, which go with every copy of a build while any of it is left.
    files["LICENSE.md"] = UPSTREAM_NOTICE.encode()
    # Flare Drive expects INSTALLATION.md too. The guide is kept outside git until the repository goes public, so it
    # comes from this folder, not from the commit.
    guide = ROOT / "docs/INSTALLATION.md"
    if not guide.is_file():
        raise SystemExit(f"FAIL: {guide} missing: Flare Drive needs it in the archive. Copy it from the local docs.")
    files["INSTALLATION.md"] = guide.read_bytes()
    return files


def main() -> None:
    version = (ROOT / "VERSION").read_text().strip()
    top = f"aa-entrynav-{version}"
    # Every file carries the commit's time, so the same commit always packs the same bytes.
    committed = subprocess.run(["git", "log", "-1", "--format=%ct"], cwd=ROOT, capture_output=True, text=True)
    if committed.returncode or not committed.stdout.strip().isdigit():
        raise SystemExit(f"FAIL: git can't read the commit's time here: {committed.stderr.strip() or 'no commit'}")
    epoch = int(committed.stdout)
    archive = ROOT / f"artifacts/{top}-armhf.tar"
    archive.parent.mkdir(exist_ok=True)
    payload = io.BytesIO()
    files = release_files(version)
    folders = sorted({str(Path(name).parent).replace("\\", "/") for name in files} - {"."})
    folders = sorted({"/".join(folder.split("/")[: i + 1]) for folder in folders for i in range(folder.count("/") + 1)})
    with tarfile.open(fileobj=payload, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for name in [""] + folders:  # explicit folders: no tar has to guess their mode
            item = tarfile.TarInfo(f"{top}/{name}".rstrip("/"))
            item.type = tarfile.DIRTYPE
            item.mode = 0o755
            item.mtime = epoch
            item.uname = item.gname = "root"
            tar.addfile(item)
        for name, data in sorted(files.items()):
            item = tarfile.TarInfo(f"{top}/{name}")
            item.size = len(data)
            item.mode = 0o755 if name in EXECUTABLES else 0o644
            item.mtime = epoch
            item.uname = item.gname = "root"
            tar.addfile(item, io.BytesIO(data))
    archive.write_bytes(payload.getvalue())
    digest = sha256(payload.getvalue()).hexdigest()
    (archive.parent / f"{archive.name}.sha256").write_text(f"{digest}  {archive.name}\n")
    print(f"{archive}\nsha256 {digest}")


if __name__ == "__main__":
    main()
