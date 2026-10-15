#!/bin/sh
# Cuts a firmware release: sets FIRMWARE_VERSION in include/config.h, commits it, tags the commit
# vX.Y.Z and pushes both. The tag starts .forgejo/workflows/release.yml, which builds and
# publishes the release the website's browser flasher installs.
#
#   tools/release.sh 1.2.3            # a release
#   tools/release.sh 1.2.3-beta.1     # a prerelease (published, but not offered by the flasher)
set -eu

VERSION="${1:-}"
VERSION="${VERSION#v}"
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' || {
    echo "usage: $0 MAJOR.MINOR.PATCH[-suffix]" >&2
    exit 1
}
TAG="v$VERSION"
cd "$(dirname "$0")/.."

[ "$(git rev-parse --abbrev-ref HEAD)" = "main" ] || { echo "Release from main." >&2; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "Commit or stash your changes first." >&2; exit 1; }
git fetch -q origin main --tags
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || { echo "main isn't in step with origin/main - pull/push first." >&2; exit 1; }
! git rev-parse -q --verify "refs/tags/$TAG" >/dev/null || { echo "$TAG already exists." >&2; exit 1; }

# The version line in config.h:  #define FIRMWARE_VERSION "1.2.3"
sed -i.bak -E "s/^(#define FIRMWARE_VERSION )\"[^\"]*\"/\\1\"$VERSION\"/" include/config.h && rm -f include/config.h.bak
grep -q "^#define FIRMWARE_VERSION \"$VERSION\"$" include/config.h || { echo "Couldn't set FIRMWARE_VERSION in include/config.h" >&2; exit 1; }

git commit -q -m "chore(release): $TAG" include/config.h
git tag -a "$TAG" -m "LapDog timer firmware $VERSION"
git push -q origin main "$TAG"
echo "Released $TAG - the release workflow is building it now."
