#!/usr/bin/env bash
#
# Reliable test runner for the Bezel compiler.
#
# Why this exists: the test executable (bezel_tests) links against the
# bezel_core library. If `cmake --build` is interrupted, fails silently, or a
# command sequence runs the binary before the build finishes, you can end up
# *running a stale bezel_tests* — one compiled from older source — which then
# reports the suite green when the working tree is actually broken. That false
# green is how a regression once reached main (PR #246: the ref/out write path
# was broken, but a stale binary reported "0 failed", so it merged).
#
# This script removes that whole class of error:
#   1. Force-removes the old test binary so a stale one can never be run.
#   2. Builds the test target and HARD-STOPS on any build failure (set -e),
#      so a failed build can never be mistaken for a passing test run.
#   3. Runs the freshly-linked binary and propagates its real exit code.
#
# Usage:
#   scripts/run-tests.sh                 # build (dir: ./build) + run all tests
#   scripts/run-tests.sh -j8             # override build parallelism
#   BUILD_DIR=out scripts/run-tests.sh   # use a different build directory
#
# Exit code is the test binary's own exit code (non-zero if any test failed),
# or the build's exit code if the build failed.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
# Default to -j16 (the build is compile-bound; this machine has many cores and
# each TU peaks well under the build cage's memory budget). Override by passing
# a -jN as $1.
JOBS_FLAG="${1:--j16}"

# The suite is several per-area binaries (bezel_tests_<group>); the
# CMake-generated manifest lists every one CMake built, which is why nothing
# here has to hardcode the set.
MANIFEST="${BUILD_DIR}/tests/test_binaries.txt"

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "error: build directory '${BUILD_DIR}' does not exist." >&2
  echo "       configure first, e.g.:" >&2
  echo "       cmake -S . -B ${BUILD_DIR} -DCMAKE_CXX_COMPILER=clang++" >&2
  exit 2
fi

# 1. A stale binary must never survive into the run.
if [[ -s "${MANIFEST}" ]]; then
  while IFS=$'\t' read -r _g bin _w; do
    [[ -n "${bin:-}" ]] && rm -f "${bin}"
  done < "${MANIFEST}"
fi

# 2. Build; set -e aborts the script (and this run) on any build error, so a
#    failed build is never silently followed by a "passing" test run.
#    `bezel_tests` is the aggregate target over every per-area binary.
echo "==> building bezel_tests (${BUILD_DIR}, ${JOBS_FLAG})"
cmake --build "${BUILD_DIR}" "${JOBS_FLAG}" --target bezel_tests

# 3. Every binary the build claims to have produced must exist and be freshly
#    linked. A missing manifest means we have no idea what the suite is — that
#    is an error, never "run what happens to be there".
if [[ ! -s "${MANIFEST}" ]]; then
  echo "error: build reported success but '${MANIFEST}' is missing/empty." >&2
  echo "       the test targets may not be wired into the build." >&2
  exit 3
fi
TEST_BINS=()
while IFS=$'\t' read -r _g bin _w; do
  [[ -n "${bin:-}" ]] || continue
  if [[ ! -x "${bin}" ]]; then
    echo "error: build reported success but '${bin}' is missing." >&2
    exit 3
  fi
  TEST_BINS+=("${bin}")
done < "${MANIFEST}"

# 4. Run each binary serially, propagating the first failure. (`set -e` is on,
#    so an explicit rc capture is needed to run them all before reporting.)
rc=0
for bin in "${TEST_BINS[@]}"; do
  echo "==> running ${bin}"
  "${bin}" || rc=$?
done
exit "${rc}"
