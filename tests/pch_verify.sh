#!/usr/bin/env bash
# Pre-build content check of the test PCH in relocatable-ccache mode (#5085).
# Wired in tests/CMakeLists.txt as the always-run target bezel_tests_pch_verify,
# which bezel_tests_pch depends on, so it runs before the PCH rule is evaluated.
#
# usage: pch_verify.sh <pch>
#
# make rebuilds the PCH only when an input's MTIME is newer than the .pch. A
# package update (rpm/dnf) installs headers with the package's build mtime,
# which can be OLDER than the PCH, and clang does not revalidate system headers
# from a PCH (user headers by size only, under -fno-pch-timestamp). So after
# such an update the tree would keep compiling against a stale PCH, silently.
#
# tests/pch_sum_launcher.sh records <pch>.inputs: the sha256 of every file the
# PCH was built from. This script re-checks those hashes (~0.03 s for the ~580
# inputs) and, if any input's CONTENT changed or vanished, or the record is
# missing, deletes the .pch, .sum and .inputs, so make rebuilds the PCH and,
# through their dependency on it, every consumer. It never deletes anything
# when the PCH does not exist.
set -u
if [ "$#" -ne 1 ]; then
    echo "pch_verify.sh: usage: <pch>" >&2
    exit 2
fi
pch=$1
[ -e "$pch" ] || exit 0
if [ -f "$pch.inputs" ] && sha256sum --quiet --status -c -- "$pch.inputs" 2>/dev/null; then
    exit 0
fi
if [ -f "$pch.inputs" ]; then
    why="an input changed since it was built (e.g. a package update that kept old mtimes)"
else
    why="it has no $pch.inputs record, so it cannot be checked"
fi
echo "pch_verify.sh: $pch is stale or unverifiable: $why; deleting it so it is rebuilt (#5085)" >&2
rm -f -- "$pch" "$pch.sum" "$pch.inputs" || exit 1
exit 0
