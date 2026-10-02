#!/usr/bin/env bash
# untracked_guard.sh — shared untracked-file check for pregate.sh and
# gate_and_push.sh (#5265).
#
# CMake globs src/ and tests/ into the build, so an untracked file there is
# compiled into the gate binary: the gate then tests something other than the
# commit it reports (false FAIL or false PASS). Untracked files under
# src/ tests/ scripts/ CMakeLists.txt cmake/ are therefore a FAIL; untracked
# files anywhere else (repo-root junk) are only a WARN. .gitignore'd files
# never count.
#
# Usage: scripts/untracked_guard.sh [REPO_DIR]
# Output: "FAIL <path>" / "WARN <path>" lines on stdout, plus a remedy line.
# Exit:   1 if any FAIL path exists, else 0 (a WARN never fails).
set -uo pipefail
cd "${1:-.}" || exit 2

GUARDED=(src tests scripts CMakeLists.txt cmake)
bad="$(git ls-files --others --exclude-standard -- "${GUARDED[@]}")"
all="$(git ls-files --others --exclude-standard)"
warn="$(grep -vxF -f <(printf '%s\n' "$bad") <<<"$all" | sed '/^$/d')"

if [ -n "$warn" ]; then
  echo "$warn" | head -10 | sed 's/^/WARN /'
  n="$(echo "$warn" | wc -l)"
  [ "$n" -gt 10 ] && echo "WARN ... and $((n - 10)) more untracked file(s) outside the build inputs"
fi
if [ -n "$bad" ]; then
  echo "$bad" | sed 's/^/FAIL /'
  echo "  untracked build-input files are globbed into the gate binary. Quarantine them" \
       "(mv into /var/tmp/bezel-scratch/<lane>/, do NOT delete) or commit them, then rebuild."
  exit 1
fi
exit 0
