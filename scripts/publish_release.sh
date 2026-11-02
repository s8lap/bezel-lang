#!/usr/bin/env bash
# publish_release.sh -- build the fully static musl bezelc and publish it as a
# GitHub pre-release.
#
# Usage: publish_release.sh <worktree> <tag> <title> <body-file> [branch]
#
# WHY. The release must be built from the PUSHED commit, so this runs after
# the gate has pushed origin/main, in the worktree that holds that commit, with
# a clean tree. The tarball name and the version string both carry the short
# sha, and the tag is r<run>-<short sha>, matching the R34/R35/R37 pre-releases
# (tags r34-c0a1928b, r35-e4562ec2, r37-db3bdf98).
#
# WHY THIS IS IN THE REPO. It ran from a scratch directory for R37
# (/var/tmp/bezel-scratch/r37-coord/publish_release.sh), where a scratch-only
# procedure is one reaping away from being lost, and it cost two runs to two
# `gh` CLI facts that are now encoded below.
#
# THE TWO `gh` FACTS, both measured on 2026-10-30 with gh 2.87.3:
#   1. THIS `gh` HAS NO `--body-file`. The option is `-F` / `--notes-file`.
#      `--body-file` is rejected, and the release is not created.
#   2. `--target` WANTS THE FULL 40-CHARACTER SHA. A short sha is rejected
#      with "Release.target_commitish is invalid" (422). The SHORT sha is for
#      the TAG and the TARBALL name; the tag already identifies the commit, so
#      --target is only belt-and-braces -- but it must be the full sha.
#
# The release is published to a PUBLIC repository. It is a coordinator
# decision, and it is the owner's call whether a release should be cut at all.
#
# Exit 0 = the release exists and its URL is printed.
set -euo pipefail

if [ $# -lt 4 ]; then
  echo "usage: publish_release.sh <worktree> <tag> <title> <body-file> [branch]" >&2
  exit 64
fi

WT="$1"; TAG="$2"; TITLE="$3"; BODY="$4"; BRANCH="${5:-}"
[ -d "$WT" ] || { echo "publish_release: no worktree $WT" >&2; exit 2; }
[ -f "$BODY" ] || { echo "publish_release: no body file $BODY" >&2; exit 2; }

cd "$WT"
git fetch -q origin
[ -z "$(git status --porcelain --untracked-files=no)" ] || {
  echo "publish_release: the tree is dirty; refusing" >&2; exit 2; }
if [ -n "$BRANCH" ]; then
  [ "$(git rev-parse --abbrev-ref HEAD)" = "$BRANCH" ] || {
    echo "publish_release: not on $BRANCH" >&2; exit 2; }
fi
# The release is built from the commit that origin/main points at.
HEAD_SHA=$(git rev-parse HEAD)
SHORT=$(git rev-parse --short=8 HEAD)
REMOTE=$(git rev-parse origin/main)
if [ "$HEAD_SHA" != "$REMOTE" ]; then
  echo "publish_release: HEAD ($SHORT) is not origin/main ($(git rev-parse --short=8 origin/main))" >&2
  exit 2
fi
echo "publish_release: building from $SHORT (origin/main)"

OUT="$WT/scratch/release-full"
scripts/release_static.sh --full --out "$OUT" -j 8

NAME="bezelc-0.0.0-$SHORT-linux-x86_64"
TARBALL="$OUT/$NAME.tar.gz"
[ -f "$TARBALL" ] || { echo "publish_release: no $TARBALL" >&2; exit 1; }
[ -f "$TARBALL.sha256" ] || { echo "publish_release: no sha256" >&2; exit 1; }

echo "publish_release: creating $TAG"
# --notes-file, NOT --body-file; --target takes the FULL sha.
gh release create "$TAG" "$TARBALL" "$TARBALL.sha256" \
  --title "$TITLE" --notes-file "$BODY" --prerelease --target "$HEAD_SHA"
gh release view "$TAG" --json url -q .url
