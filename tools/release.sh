#!/usr/bin/env bash
# Publishes a release: tools/release.sh 0.1.1
#   1. needs tools/release-notes/v0.1.1.md (the release's text; tools/release-notes/v0.1.0.md shows
#      the layout) and sets tools/VERSION to 0.1.1,
#   2. exports and pushes the source (tools/export_github.sh), then tags that commit v0.1.1 and
#      pushes the tag,
#   3. GitHub Actions builds the package and publishes the release with it (build-windows.yml,
#      about 20 minutes); the launchers of earlier versions then offer the update.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
version=${1:?usage: tools/release.sh <version, e.g. 0.1.1>}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Not a version: $version" >&2; exit 1; }
tag=v$version
dest=${GITHUB_EXPORT:-../uncharted-github}
[[ -f tools/release-notes/$tag.md ]] || { echo "Write tools/release-notes/$tag.md first" >&2; exit 1; }
git -C "$dest" fetch -q --tags origin
if git -C "$dest" rev-parse -q --verify "refs/tags/$tag" > /dev/null; then
    echo "$tag exists already" >&2
    exit 1
fi
printf '%s\n' "$version" > tools/VERSION
tools/export_github.sh "$dest" "Release $version"
git -C "$dest" push -q origin main
git -C "$dest" tag -a "$tag" -m "Release $version"
git -C "$dest" push -q origin "$tag"
echo "Pushed $tag: GitHub Actions builds the package and publishes the release (about 20 minutes)."
