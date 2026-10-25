#!/usr/bin/env python3
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0
"""Licence and header gate for the HCP tree.

Checks, in the order the failures are reported:

  1. every source file carries `SPDX-License-Identifier: Apache-2.0`;
  2. every source file carries a `Copyright (c)` line naming the author;
  3. no file carries a second, different licence id (a tree with two licences is a
     question for a lawyer, and this tree had 21 MIT files until the header sweep after
     2.0.3.21);
  4. no tracked file is empty - a tool once truncated 22 files by writing before
     reading them, and every one of them still parsed as "fine" until the build
     reached a module.yml that had become zero bytes;
  5. no U+3000 IDEOGRAPHIC SPACE in Kconfig or conf files. Zephyr's Kconfig help
     blocks are whitespace sensitive and a full-width space from an IME is invisible
     in a diff; it took a build to find the first one.

Exclusions are listed explicitly below, because "no licence line" is correct for a
couple of them (the licence text itself, YAML metadata that upstream tooling parses)
and wrong for everything else.

    ./tools/check_licence.py [--verbose]
"""

import argparse
import os
import re
import subprocess
import sys

# The whole line, comment lead-in included, capturing only the licence id.
SPDX_RE = re.compile(r"^\s*(?:[/*#]+|//)\s*SPDX-License-Identifier:\s*([A-Za-z0-9.+-]+)\s*$")

COPY_PREFIX = "Copyright (c)"

# Files that are source in the sense that matters: the extension or the basename puts
# them in a compiler's, cmake's or kconfig's input.
SOURCE_EXT = {
    ".c", ".h", ".cpp", ".hpp", ".py", ".sh", ".cmake",
    ".conf", ".cfg", ".dts", ".dtsi", ".overlay", ".in",
}
SOURCE_NAMES = {"CMakeLists.txt", "Makefile", "Kconfig"}
SOURCE_NAME_PREFIXES = ("Kconfig.",)
SOURCE_NAME_SUFFIXES = ("_defconfig",)

# Deliberately not required to carry a header, with the reason each time.
SKIP_BENEFIT = {
    ".md": "documentation; the project licence is in LICENSE",
    ".yml": "board/module metadata parsed by tooling that tolerates no comment",
    ".yaml": "testcase.yaml / devicetree binding data",
    ".png": "binary", ".bin": "binary", ".der": "binary",
    ".pem": "private key material, and never committed anyway",
    ".key": "private key material", ".txt": "notes and fetched build caches",
}
SKIP_BASENAME = {
    "LICENSE": "it IS the licence text",
    ".gitignore": "git config", ".gitmodules": "git config",
}


def tracked_files():
    out = subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True)
    return [p for p in out.stdout.split("\n") if p]


def is_source(path):
    base = os.path.basename(path)
    ext = os.path.splitext(base)[1]
    if base in SKIP_BASENAME or base.startswith("LICENSE"):
        return False
    if ext in SKIP_BENEFIT:
        return False
    if ext in SOURCE_EXT:
        return True
    if base in SOURCE_NAMES:
        return True
    return base.startswith(SOURCE_NAME_PREFIXES) or base.endswith(SOURCE_NAME_SUFFIXES)


def check(path, verbose):
    """Problems found in one file, as strings."""
    problems = []
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except UnicodeDecodeError:
        return []          # binary that glob still listed; not our business here
    if not text.strip():
        problems.append("file is empty")
        return problems

    lines = text.split("\n")
    # A *declaration* line only - one that says nothing but the tag. Matching a bare
    # substring would flag this script, which quotes the tag in its own rules.
    ids = [SPDX_RE.match(l).group(1) for l in lines if SPDX_RE.match(l)]
    if not ids:
        problems.append("no SPDX line")
    elif "Apache-2.0" not in ids:
        problems.append(f"licence is not Apache-2.0: {ids}")
    if len(ids) > 1:
        problems.append(f"{len(ids)} SPDX lines")
    if not any(COPY_PREFIX in l for l in lines):
        problems.append("no Copyright line")

    if os.path.basename(path).startswith("Kconfig") or path.endswith((".conf",)):
        for i, line in enumerate(lines, 1):
            if "\u3000" in line:
                problems.append(f"U+3000 IDEOGRAPHIC SPACE at line {i}")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--verbose", action="store_true", help="list every checked file, not just failures")
    args = ap.parse_args()

    files = [p for p in tracked_files() if is_source(p)]
    bad = 0
    for p in files:
        problems = check(p, args.verbose)
        if problems:
            bad += 1
            print(f"FAIL {p}: {'; '.join(problems)}")
        elif args.verbose:
            print(f"ok   {p}")

    skipped = [p for p in tracked_files() if not is_source(p)]
    print(f"\nlicence: {len(files) - bad}/{len(files)} source files clean, "
          f"{len(skipped)} excluded (docs, metadata, licence text, binaries)")
    for p in skipped:
        reason = SKIP_BENEFIT.get(os.path.splitext(p)[1]) or SKIP_BASENAME.get(os.path.basename(p)) or "not source"
        if args.verbose:
            print(f"  skip {p}: {reason}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
