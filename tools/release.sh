#!/usr/bin/env bash
# Cut a firmware release: bump the version, prove it builds, commit, tag.
#
#   ./tools/release.sh 2.0.3.15            # the normal case
#   ./tools/release.sh 2.0.3.15 --skip-build
#
# The rule it enforces is the project's: every version number gets BOTH a git tag and a
# `## <version>` section in CHANGELOG.md. Writing the entry first is what makes that
# happen - a number nothing describes is not a release, it is a rebuild, and the
# difference only shows up when someone asks months later what changed between two devices.
#
# Refuses, in order, unless:
#   1. the version is four dotted numbers (hcp::ota parses four fields, imgtool wants
#      "x.y.z+w" - see the translation below);
#   2. it is strictly greater than the current one, because a device refuses to install
#      an equal or lower version over the network at all;
#   3. CHANGELOG.md has "## <version>";
#   4. the tag does not already exist - a tag is not moved once it names a release;
#   5. every board builds warning-free, so a tag cannot point at a tree that does not
#      compile.
#
# It commits exactly three files (prj.conf, Kconfig, CHANGELOG.md) and leaves everything
# else alone, so a release does not sweep up unrelated work in progress.
#
# No git remote yet; when there is one: git push && git push origin v<version>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # .../hcp
cd "$HERE"

VERSION="${1:-}"
case "$VERSION" in
  ""|-h|--help)
    sed -n '2,36p' "$0"; exit 2 ;;
esac
shift
SKIP_BUILD=0
while [ $# -gt 0 ]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

die() { echo "release: $*" >&2; exit 1; }

# 1. shape
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
  die "${VERSION} is not major.minor.patch.build (four dotted numbers, e.g. 2.0.3.15)"

# MCUboot's imgtool takes the same number with a '+' before the build field: its regex
# (scripts/imgtool/version.py) rejects a dotted fourth one. Kept here and in build.sh, so
# the two cannot drift.
IMGTOOL_VERSION="${VERSION%.*}+${VERSION##*.}"

# 2. greater than what is in the tree now
CURRENT="$(sed -n 's/^CONFIG_APP_VERSION="\(.*\)"/\1/p' prj.conf | head -1)"
[ -n "$CURRENT" ] || die "cannot read CONFIG_APP_VERSION from prj.conf"

version_gt() {  # left > right, field by field
  local l r i
  for i in 1 2 3 4; do
    l="$(echo "$1" | cut -d. -f$i)"
    r="$(echo "$2" | cut -d. -f$i | tr -d '+')"
    [ -z "$l" ] && l=0
    [ -z "$r" ] && r=0
    if [ "$l" -gt "$r" ]; then return 0; fi
    if [ "$l" -lt "$r" ]; then return 1; fi
  done
  return 1
}
version_gt "$VERSION" "$CURRENT" ||
  die "${VERSION} is not above the current ${CURRENT}; a device will refuse it over OTA (equal or lower is a USB re-flash, not a release)"

# 3. the changelog gate
grep -qE "^## ${VERSION}( |$)" CHANGELOG.md ||
  die "CHANGELOG.md has no '## ${VERSION}' section. Write what changed and what you measured, then re-run."

# 4. no duplicate tag
if git rev-parse -q --verify "refs/tags/v${VERSION}" >/dev/null; then
  die "tag v${VERSION} already points at $(git rev-parse --short "v${VERSION}"); a tag is not moved once it names a release"
fi

echo ">>> ${CURRENT} -> ${VERSION}  (image header: ${IMGTOOL_VERSION})"

# 5. bump both symbols in both places that carry them
python3 - "$VERSION" "$IMGTOOL_VERSION" <<'PY'
import re, sys
version, imgtool = sys.argv[1], sys.argv[2]

# prj.conf: the two symbols, always together.
text = open("prj.conf").read()
text, n = re.subn(r'(?m)^CONFIG_APP_VERSION="[^"]*"$',
                  f'CONFIG_APP_VERSION="{version}"', text)
text, m = re.subn(r'(?m)^CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="[^"]*"$',
                  f'CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="{imgtool}"', text)
assert n == 1 and m == 1, f"prj.conf version symbols matched {n}/{m} times, expected 1/1"
open("prj.conf", "w").write(text)

# Kconfig: the default of the same knob, so a build without --version is this release.
text = open("Kconfig").read()
start = text.index("config APP_VERSION")
end = text.index("\n\n", start)
block, count = re.subn(r'default "[^"]*"', f'default "{version}"',
                       text[start:end], count=1)
assert count == 1, "APP_VERSION default not found in Kconfig"
open("Kconfig", "w").write(text[:start] + block + text[end:])
print(f">>> bumped prj.conf and Kconfig")
PY

if [ "$SKIP_BUILD" = "0" ]; then
  # All boards, product configuration, new version as the default: a release tag on a
  # tree that does not build is worse than no tag, because it looks authoritative.
  echo ">>> building all boards (use --skip-build to bypass)"
  if ! ./build.sh all > build/release-build.log 2>&1; then
    tail -25 build/release-build.log
    die "build failed; nothing was committed or tagged. The version bump is in the working tree - fix or revert it."
  fi
  # Anything but the known MCUboot version-string warning from the signing tool.
  WARNINGS="$(grep -E 'error:|warning:' build/release-build.log | grep -vc MCUBOOT_IMGTOOL || true)"
  if [ "$WARNINGS" != "0" ]; then
    grep -E 'error:|warning:' build/release-build.log | grep -v MCUBOOT_IMGTOOL | head -5
    die "the build is not warning-free; nothing was committed or tagged"
  fi
  grep -E "FLASH:|sram0_0_seg" build/release-build.log | tail -6
fi

git add prj.conf Kconfig CHANGELOG.md
git commit -q -m "release: ${VERSION}

Both version symbols move together, as they must: the application reports
${VERSION} (hcp::ota compares up to four dotted fields) and the MCUboot image header
carries ${IMGTOOL_VERSION}, which is the only spelling imgtool's version regex
accepts. See CHANGELOG.md for what is in it and what was measured.

Built by ./tools/release.sh, which also tagged v${VERSION}."
git tag -a "v${VERSION}" -m "HCP firmware ${VERSION} - see CHANGELOG.md"

echo ">>> committed $(git rev-parse --short HEAD) and tagged v${VERSION}"
echo ">>> next, to a device:"
echo "    HCP_SN=hcp-M1231 ./tools/ota_release.sh ${VERSION} hcp_c3 --bench"
echo ">>> and to a remote, once one exists: git push && git push origin v${VERSION}"
