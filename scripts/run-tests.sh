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
JOBS_FLAG="${1:--j4}"

TEST_BIN="${BUILD_DIR}/tests/bezel_tests"

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "error: build directory '${BUILD_DIR}' does not exist." >&2
  echo "       configure first, e.g.:" >&2
  echo "       cmake -S . -B ${BUILD_DIR} -DCMAKE_CXX_COMPILER=clang++" >&2
  exit 2
fi

# 1. A stale binary must never survive into the run.
rm -f "${TEST_BIN}"

# 2. Build; set -e aborts the script (and this run) on any build error, so a
#    failed build is never silently followed by a "passing" test run.
echo "==> building bezel_tests (${BUILD_DIR}, ${JOBS_FLAG})"
cmake --build "${BUILD_DIR}" "${JOBS_FLAG}" --target bezel_tests

# 3. The binary must exist and be freshly linked after a successful build.
if [[ ! -x "${TEST_BIN}" ]]; then
  echo "error: build reported success but '${TEST_BIN}' is missing." >&2
  echo "       the test target may not be wired into the build." >&2
  exit 3
fi

echo "==> running ${TEST_BIN}"
exec "${TEST_BIN}"
