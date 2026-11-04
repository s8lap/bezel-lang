#!/usr/bin/env bash
# test_token.sh — hold ONE machine-wide build token for the lifetime of a test
# process. Spawned by tests/test_main.cpp (holdTestToken) from every
# bezel_tests_<area> binary, sharded or not, before its first test runs.
#
# WHY. MEASURED 2026-11-04 ~19:30: `build_jobserver.py status` read
# `available=20 held=0` (the box "idle") while ~20 whole-area bezel_tests_*
# processes ran across 17 lane worktrees (load 13). The pool rationed only
# compiles and links, so test runs were neither counted nor capped. Lanes start
# tests through dev_heavy.sh, through agent_check.sh, and by running
# build/tests/bezel_tests_<area> directly, so (as build_token.sh's header
# records for compiles) only a hook in the binary itself cannot be bypassed.
#
# Protocol (the caller sets up the descriptors):
#   fd 0  the read end of a pipe whose write end ONLY the test process holds.
#         EOF on it means the test has exited, however it exited (SIGKILL too).
#   fd 3  the write end of a "ready" pipe. One byte is written to it once the
#         token is in hand; EOF without a byte means no token was taken.
#
# The token itself is taken by scripts/build_token.sh, UNCHANGED, so every one
# of its cases applies as it does to a compile: BEZEL_JOBSERVER=off runs
# straight through, a job already accounted for by a joined make passes
# through, a launcher-aware daemon hands out a FIFO token (this process then
# reads as `bzl-tok-held`, which the daemon's audit already counts, so the
# running daemon needs no restart and never re-mints a test-held token), and
# with no daemon one of the machine-wide flock slots is taken instead.
#
# The holder is a CHILD of the test, not a wrapper around it: the test keeps
# its own pid, so `kill <pid>` / timeout(1) still reach the test itself.
set -u
# Already held for this process tree (test_main.cpp sets it once its token is
# in hand): a test binary started by a test must not queue for a second token
# while its parent holds the first. test_main.cpp checks this too, before it
# spawns anything; this copy is what the script self-tests can reach.
if [ -n "${BEZEL_TEST_TOKEN_HELD:-}" ]; then
  printf T >&3
  exit 0
fi
exec bash "$(dirname -- "${BASH_SOURCE[0]}")/build_token.sh" \
  sh -c 'printf T >&3; exec 3>&-; exec cat >/dev/null'
