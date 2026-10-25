#!/usr/bin/env python3
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0
"""Release-consistency gate: the version the tree claims has to hang together.

Four places carry the firmware version and three of them can disagree silently:

  CONFIG_APP_VERSION                  what the device reports on <sn>/online and compares
                                      against version.txt - parsed as up to four dotted
                                      fields, so it has to be exactly `x.y.z.w`
  CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION the same number as imgtool will accept - its regex
                                      (scripts/imgtool/version.py) wants `x.y.z+w`, and a
                                      dotted fourth field fails the build
  Kconfig's `default "..."`           what a build without --version stamps, so it has to
                                      agree with prj.conf or two builds of one tree
                                      produce two versions
  CHANGELOG.md `## <version>`          a number nothing describes is a rebuild, not a
                                      release
  git tag v<version>                   the rule this project set for itself: every version
                                      gets a tag, so a device on the network can be mapped
                                      back to the code that is running on it

tools/release.sh enforces these while making a release. This enforces them afterwards,
which is where the interesting mistakes are: a hand-edited prj.conf, a release commit
reverted without its tag, a CHANGELOG section rewritten onto another version.

    ./tools/check_release.py
"""

import re
import subprocess
import sys

FOUR_FIELDS = re.compile(r"^\d+\.\d+\.\d+\.\d+$")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def main():
    problems = []

    prj = read("prj.conf")
    app = re.search(r'^CONFIG_APP_VERSION="([^"]*)"', prj, re.M)
    img = re.search(r'^CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="([^"]*)"', prj, re.M)
    if not app:
        problems.append('prj.conf: CONFIG_APP_VERSION missing')
        app = None
    else:
        app = app.group(1)
        if not FOUR_FIELDS.match(app):
            problems.append(f'CONFIG_APP_VERSION="{app}" is not major.minor.patch.build')
    if not img:
        problems.append('prj.conf: CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION missing')
        img = None
    else:
        img = img.group(1)

    if app and img is not None:
        want = app.rsplit(".", 1)[0] + "+" + app.rsplit(".", 1)[1] if app else None
        if img != want:
            problems.append(f'imgtool version "{img}" should be "{want}" for {app} '
                            "(the same number, spelled the way imgtool's regex accepts)")

    kconfig = read("Kconfig")
    block = kconfig[kconfig.index("config APP_VERSION"):] if "config APP_VERSION" in kconfig else ""
    kd = re.search(r'default "([^"]*)"', block)
    if not kd:
        problems.append("Kconfig: no `default` for config APP_VERSION")
    elif app and kd.group(1) != app:
        problems.append(f'Kconfig default "{kd.group(1)}" != CONFIG_APP_VERSION "{app}"')

    if app:
        if not re.search(r"^## " + re.escape(app) + r"\b", read("CHANGELOG.md"), re.M):
            problems.append(f"CHANGELOG.md has no `## {app}` section")
        tag = subprocess.run(["git", "rev-parse", "-q", "--verify", f"refs/tags/v{app}"],
                             capture_output=True, text=True)
        if tag.returncode != 0:
            problems.append(f"tag v{app} does not exist - devices reporting {app} on the "
                            "network cannot be mapped back to a commit")

    if problems:
        for p in problems:
            print("FAIL", p)
        return 1
    print(f"release: version {app} consistent across prj.conf, Kconfig, CHANGELOG.md and "
          f"tag v{app}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
