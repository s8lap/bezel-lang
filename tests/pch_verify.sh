#!/usr/bin/env bash
# Pre-build content check of the test PCH in relocatable-ccache mode (#5085).
# Wired in tests/CMakeLists.txt as an always-run custom command (target
# bezel_tests_pch_verify) that bezel_tests_pch depends on, so it runs before
# the PCH rule is evaluated.
#
# usage: pch_verify.sh <pch> [<stamp>]
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
# missing, deletes the .pch, .sum and .inputs. It never deletes anything when
# the PCH does not exist.
#
# <stamp> (#5088): deleting the .pch is not enough under Ninja, which computes
# its dirty set before the build starts and never re-stats the .pch, so the
# build would finish rc=0 on the stale objects. The stamp is the custom
# command's OUTPUT and an OBJECT_DEPENDS of the PCH compile. It is touched ONLY
# when the PCH is deleted; Ninja's restat then keeps the PCH edge (and every
# consumer) dirty, or cleans it when the stamp did not move. Under make the
# deleted .pch alone forces the rebuild. The same build therefore recovers
# under both generators. A missing stamp is created at configure time with mtime 1
# (tests/CMakeLists.txt #5092); if cmake has not been reconfigured this script
# creates it as a fallback with the same ancient mtime, so creating it never
# makes a valid PCH out of date.
set -u
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "pch_verify.sh: usage: <pch> [<stamp>]" >&2
    exit 2
fi
pch=$1; stamp=${2:-}
if [ -n "$stamp" ] && [ ! -e "$stamp" ]; then
    touch -d @1 -- "$stamp" || exit 1
fi
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
if [ -n "$stamp" ]; then touch -- "$stamp" || exit 1; fi
exit 0
