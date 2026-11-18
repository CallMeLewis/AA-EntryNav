# Copyright (c) 2026 CallMeLewis
"""Say whether commits change anything the tests could notice, or documentation only.

  python tools/code_changes.py OLD NEW    exit 0 when OLD..NEW changes more than documentation, 3 when only that
                                          (any other exit is an error, such as a commit this clone lacks)

Documentation is every Markdown file and everything under docs/, except NOTICE.md, which tools/verify_core.py reads.
CI skips a push that changes nothing else (the `paths` of .github/workflows/ci.yml, which must say the same), and so
does the pre-push hook's ARM check. publish_release.py uses tested_commit() to find the commit CI did test.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def is_documentation(path):
    if path == "NOTICE.md":
        return False
    return path.endswith(".md") or path.startswith("docs/")


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def changes_code(old, new):
    """True when old..new changes a file that is not documentation."""
    names = git("diff", "--name-only", old, new).splitlines()
    return any(not is_documentation(name) for name in names)


def tested_commit(head):
    """The newest commit at or before head that CI runs on: head itself unless it only changes documentation."""
    for commit in git("rev-list", "--first-parent", head).splitlines():
        names = git("diff-tree", "--no-commit-id", "--name-only", "-r", "-m", "--root", commit).splitlines()
        if any(not is_documentation(name) for name in names):
            return commit
    return head


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sys.exit(0 if changes_code(sys.argv[1], sys.argv[2]) else 3)


if __name__ == "__main__":
    main()
