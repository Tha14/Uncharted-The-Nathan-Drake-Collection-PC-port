#!/usr/bin/env bash
# tools/release.sh X.Y.Z - publishes version X.Y.Z of the port from this repository.
#
# Before: write tools/release-notes/vX.Y.Z.md (its first lines are the same as the previous
# release's, then what is new). The script then
#   - puts X.Y.Z in tools/VERSION and commits it with the notes as "Release X.Y.Z",
#   - brings in what is on GitHub (rebase), pushes main and the tag vX.Y.Z.
# The tag starts .github/workflows/build-windows.yml, which builds the package and publishes the
# release; launchers of earlier versions then offer the update.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

version=${1:-}
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "usage: tools/release.sh X.Y.Z" >&2
    exit 2
fi
tag=v$version
notes=tools/release-notes/$tag.md
if [[ ! -s $notes ]]; then
    echo "missing $notes: write the release notes first" >&2
    exit 1
fi
git fetch -q --tags origin
if git rev-parse -q --verify "refs/tags/$tag" > /dev/null; then
    echo "$tag is taken already" >&2
    exit 1
fi
if [[ $(git rev-parse --abbrev-ref HEAD) != main ]]; then
    echo "releases are made from main" >&2
    exit 1
fi

echo "$version" > tools/VERSION
git add tools/VERSION "$notes"
git commit -q -m "Release $version"
git pull -q --rebase --autostash origin main
git push -q origin main
git tag "$tag"
git push -q origin "$tag"
echo "$tag pushed: the build publishes the release when it is done."
