#!/usr/bin/env python3
"""Keep sce_sys/param.json's contentVersion in step with the release tags.

The PS5 homebrew catalog (docs/versioning.md in darkxex/ps5-homebrew-catalog)
decides whether an installed copy is outdated by comparing contentVersion,
NN.NNN.NNN, as three numbers. Each release must raise it, never lower or reuse
one, and the release tag is the content version itself.

  content-version.py current            print the committed contentVersion
  content-version.py bump <part>        raise it (major, minor or patch), write
                                        param.json and print the new value;
                                        "current" releases the committed value
                                        as it is
  content-version.py check <version>    fail unless <version> is well formed and
                                        higher than every version tag

Version tags are read from `git tag`, so run it in a clone with its tags.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

PARAM = Path(__file__).resolve().parents[1] / "sce_sys" / "param.json"
FORMAT = re.compile(r"^(\d{2})\.(\d{3})\.(\d{3})$")
WIDTHS = (2, 3, 3)


def parse(version):
    m = FORMAT.match(version)
    if not m:
        sys.exit(f"content-version: {version!r} is not NN.NNN.NNN")
    return tuple(int(g) for g in m.groups())


def show(parts):
    return ".".join(str(n).zfill(w) for n, w in zip(parts, WIDTHS))


def version_tags():
    tags = subprocess.run(["git", "tag", "--list"], check=True,
                          capture_output=True, text=True).stdout.split()
    return [t for t in tags if FORMAT.match(t)]


def check(version, ignore=()):
    new = parse(version)
    for tag in version_tags():
        if tag in ignore:
            continue
        if parse(tag) >= new:
            sys.exit(f"content-version: {version} is not higher than the "
                     f"existing tag {tag}; consoles would not see it as an update")


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd = argv[1]
    param = json.loads(PARAM.read_text())
    current = param["contentVersion"]

    if cmd == "current":
        parse(current)
        print(current)
    elif cmd == "check" and len(argv) == 3:
        # The tag being checked already exists when a tag push runs this.
        check(argv[2], ignore=(argv[2],))
        print(argv[2])
    elif cmd == "bump" and len(argv) == 3:
        major, minor, patch = parse(current)
        part = argv[2]
        parts = {"major": (major + 1, 0, 0), "minor": (major, minor + 1, 0),
                 "patch": (major, minor, patch + 1),
                 "current": (major, minor, patch)}.get(part)
        if parts is None:
            sys.exit(f"content-version: unknown part {part!r}")
        if any(n >= 10 ** w for n, w in zip(parts, WIDTHS)):
            sys.exit(f"content-version: raising {part} of {current} overflows NN.NNN.NNN")
        new = show(parts)
        if new in version_tags():
            sys.exit(f"content-version: tag {new} already exists; bump instead")
        check(new)
        if new != current:
            param["contentVersion"] = new
            PARAM.write_text(json.dumps(param, indent=2) + "\n")
        print(new)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
