#!/usr/bin/env bash
# pick_issues.sh — THE rules for which issues a commit names, and whether it
# is a fix. Single source for pick_stage.sh, pick_ledger.sh and (through the
# ledger) run_status.sh's "fixes banked" figure.
#
# ISSUES. A commit's issues are the #NNN (three or more digits) in its SUBJECT
# line only, in order, de-duplicated: `fix #4807/#4808` -> both,
# `fix #5023+#3904` -> both. A subject with none names 0 issues.
# WHY the subject only: bodies are full of cross-references ("regressed by
# #4501", "see #3243"); counting them reported 200 distinct issues over
# 466a28e2..origin/main where the real figure was ~71.
#
# KIND. Whether a commit is a fix is RECORDED by pick_stage.sh --kind
# (fix|test|bank|infra) in the refs/notes/bezel-picks note; only kind=fix
# commits count toward "fixes banked". The subject-only rule alone was not
# enough: banking, test-only and infra commits routinely name the issue they
# relate to ("Bank … from #5032", "test #5035 …"). For a commit with no note
# (picked before notes existed, or by hand), --infer-kind falls back to a
# subject heuristic, and the ledger marks the result as inferred:
#   fix   if the subject starts with fix / fix-forward / narrow / close
#         (case-insensitive, optionally after a `sema: ` / `codegen test: `
#         style area prefix), OR ends with `(#NNN)` and does not start with
#         bank / test / lit / regression pin;
#   other otherwise.
#
# Usage:  <commit message> | scripts/pick_issues.sh     # prints "#A #B" or ""
#         scripts/pick_issues.sh <rev>                  # same, for a commit
#         scripts/pick_issues.sh --infer-kind [<rev>]   # prints fix | other
set -uo pipefail

subject() { if [ -n "${1:-}" ]; then git log -1 --format=%s "$1"; else head -n 1; fi; }

if [ "${1:-}" = --infer-kind ]; then
  s="$(subject "${2:-}")"
  area='^([A-Za-z0-9_./-]+( [A-Za-z0-9_./-]+)?: )?'
  if printf '%s\n' "$s" | grep -qiE "${area}(fix|fix-forward|narrow|close)([^A-Za-z]|$)"; then
    echo fix
  elif printf '%s\n' "$s" | grep -qE '\(#[0-9]{3,}\)[[:space:]]*$' \
       && ! printf '%s\n' "$s" | grep -qiE '^(bank|test|lit|regression pin)'; then
    echo fix
  else
    echo other
  fi
  exit 0
fi

subject "${1:-}" | grep -oE '#[0-9]{3,}' | awk '!seen[$0]++' | paste -sd' '
exit 0
