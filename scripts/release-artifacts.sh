#!/usr/bin/env bash
# Render release artifacts for a tag: release notes (the CHANGELOG section),
# the stable AUR PKGBUILD pinned to the verified commit, and its .SRCINFO.
#
#   scripts/release-artifacts.sh <tag> <commit> <outdir>
#
# Fails if the tag does not match VERSION or CHANGELOG.md has no section for
# it — a release must be described before it is published.
set -euo pipefail

tag="$1" commit="$2" out="$3"
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' <"$root/VERSION")"

if [[ "$tag" != "v$version" ]]; then
    echo "release: tag $tag does not match VERSION $version" >&2
    exit 1
fi
if [[ ! "$commit" =~ ^[0-9a-f]{40}$ ]]; then
    echo "release: commit must be a full 40-char SHA, got '$commit'" >&2
    exit 1
fi

mkdir -p "$out"
awk -v v="$version" '
    $0 ~ "^## \\[" v "\\]" { found = 1; next }
    found && /^## \[/ { exit }
    found { print }
' "$root/CHANGELOG.md" | sed -e '/./,$!d' >"$out/release-notes.md"
if [[ ! -s "$out/release-notes.md" ]]; then
    echo "release: CHANGELOG.md has no [$version] section" >&2
    exit 1
fi

sed -e "s/@VERSION@/$version/" -e "s/@COMMIT@/$commit/" \
    "$root/packaging/aur/PKGBUILD.in" >"$out/PKGBUILD"

# makepkg refuses to run as root; CI calls this as an unprivileged user.
if command -v makepkg >/dev/null 2>&1; then
    (cd "$out" && makepkg --printsrcinfo >.SRCINFO)
fi
echo "release: artifacts for $tag ($commit) in $out"
