#!/usr/bin/env bash
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0
# Generate the release record for a version tag: what went in, and whose work it was.
#
#   ./tools/release_notes.sh v2.0.3.21              # everything since the previous tag
#   ./tools/release_notes.sh v2.0.3.20 v2.0.3.21    # an explicit range
#
# The body has three parts, in this order:
#
#   1. the CHANGELOG.md section for the version, copied verbatim - that text is written by
#      a human who measured something, and tools/release.sh refuses to cut a release
#      without it, so it is the part worth reading first;
#   2. the commits in the range, one per line with its short hash, for whoever wants to
#      jump from a release to the change that caused a behaviour;
#   3. new contributors - every author who appears in this range and in none of the
#      history before it. Names are taken from the commit author line, and a person who
#      contributed under two identities shows up twice, which is the honest answer until
#      someone maps them together.
#
# Written to stdout so both the CI job and a human before the tag can look at it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$(dirname "$HERE")"

TO="${2:-${1:-HEAD}}"
FROM="${1:-}"
if [ "$#" -le 1 ]; then
  # The most recent tag reachable before this one: describe walks the history, so it is
  # right even when tags were cut out of order.
  FROM=$(git describe --tags --abbrev=0 "$TO^" 2>/dev/null || true)
fi
[ -n "$FROM" ] || { echo "release_notes: no previous tag before $TO; using the root commit" >&2
                   FROM=$(git rev-list --max-parents=0 "$TO" | tail -1); }
FROM_REF="$FROM"
TO_REF="$TO"

version="${TO_REF#refs/tags/}"; version="${version#v}"

printf 'HCP firmware %s\n\n' "$version"

# 1. the hand-written section, up to the next heading
if section=$(awk -v v="$version" '
     $0 ~ "^## " v "([[:space:]]|$)" {inblock=1; next}
     inblock && /^## / {exit}
     inblock {print}
   ' CHANGELOG.md) && [ -n "$section" ]; then
  printf '%s\n\n' "---"
  printf '%s\n\n' "$section"
fi

# 2. the commits, oldest last, so the newest work reads first
printf '## Commits since %s\n\n' "$FROM_REF"
git log --no-merges --pretty=format:'- %s (%h)' "$FROM_REF..$TO_REF"
printf '\n\n'

# 3. authors new to the project in this range
new_authors=$(comm -13 \
  <(git log --no-merges --pretty=format:'%aN <%aE>' "$FROM_REF" | sort -u) \
  <(git log --no-merges --pretty=format:'%aN <%aE>' "$FROM_REF..$TO_REF" | sort -u))
if [ -n "$new_authors" ]; then
  printf '## New contributors\n\n'
  printf '%s\n' "$new_authors" | sed 's/^/- /'
  printf '\n'
fi

printf -- '---\n_Built by ./tools/release_notes.sh from %s..%s_\n' "$FROM_REF" "$TO_REF"
