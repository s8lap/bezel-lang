#!/usr/bin/env bash
# install_hooks.sh — install the worktree-ownership hooks for the whole repo.
#
# WHY: see scripts/wt_owner.sh. Hooks live in ONE place per repo (the common
# git dir's hooks/, or core.hooksPath), shared by every worktree, while the
# hook logic is tracked in scripts/hooks/ and differs between branches. So the
# installed hooks are thin wrappers that exec the CURRENT worktree's
# scripts/hooks/<name> and silently do nothing when a worktree's branch
# predates it. Behaviour therefore follows the checked-out branch, and a
# branch without scripts/hooks/ (or the gate) is unaffected.
#
# Installing is the coordinator's decision, not a lane's: it changes commit
# behaviour in every worktree of the repo at once.
#
# Usage:  scripts/install_hooks.sh              # install / update (idempotent)
#         scripts/install_hooks.sh --uninstall  # remove only OUR wrappers
# Refuses (exit 1) to overwrite an existing hook it did not write.
set -euo pipefail

HOOKS=(pre-commit post-checkout)
MARK="bezel-hook-wrapper (installed by scripts/install_hooks.sh)"

hp="$(git config --get core.hooksPath || true)"
if [ -n "$hp" ]; then
  case "$hp" in /*) dir="$hp" ;; *) dir="$(git rev-parse --show-toplevel)/$hp" ;; esac
else
  dir="$(git rev-parse --path-format=absolute --git-common-dir)/hooks"
fi
mkdir -p "$dir"

wrapper() { # $1 = hook name
  cat <<EOF
#!/bin/sh
# $MARK — do not edit.
# Runs the current worktree's tracked scripts/hooks/$1, if it has one.
top="\$(git rev-parse --show-toplevel 2>/dev/null)" || exit 0
h="\$top/scripts/hooks/$1"
[ -x "\$h" ] || exit 0
exec "\$h" "\$@"
EOF
}

if [ "${1:-}" = --uninstall ]; then
  for h in "${HOOKS[@]}"; do
    if [ -f "$dir/$h" ] && grep -qF "$MARK" "$dir/$h"; then rm -f "$dir/$h"; echo "install_hooks: removed $dir/$h"; fi
  done
  exit 0
fi

# Check every target first, so a refusal never leaves a half-installed set.
for h in "${HOOKS[@]}"; do
  if [ -e "$dir/$h" ] && ! grep -qF "$MARK" "$dir/$h" 2>/dev/null; then
    echo "install_hooks: REFUSING — $dir/$h exists and was not installed by this script." >&2
    echo "  Merge it by hand (or move it aside) and re-run." >&2
    exit 1
  fi
done
for h in "${HOOKS[@]}"; do
  if [ -f "$dir/$h" ] && [ "$(cat "$dir/$h")" = "$(wrapper "$h")" ]; then
    echo "install_hooks: $dir/$h already current"
  else
    wrapper "$h" > "$dir/$h"; chmod +x "$dir/$h"
    echo "install_hooks: installed $dir/$h"
  fi
done
