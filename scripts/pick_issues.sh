#!/usr/bin/env bash
# pick_issues.sh — THE rule for which issues a commit counts toward.
#
# A commit's issues are the #NNN (three or more digits) in its SUBJECT line
# only, in order, de-duplicated: `fix #4807/#4808` -> both, `fix #5023+#3904`
# -> both. A subject with none counts as 0 (banking, test-only, infra commits).
#
# WHY the subject only: bodies are full of cross-references ("regressed by
# #4501", "see #3243"). Counting them made pick_ledger.sh report 200 distinct
# issues over 466a28e2..origin/main where the real figure was ~71, and that made
# run_status.sh's "banked N/target" useless as a gate trigger.
#
# Single source for pick_stage.sh (the note's `issues:` field), pick_ledger.sh
# and, through it, run_status.sh.
#
# Usage:  <commit message> | scripts/pick_issues.sh     # prints "#A #B" or ""
#         scripts/pick_issues.sh <rev>                  # same, for a commit
set -uo pipefail
if [ $# -gt 0 ]; then git log -1 --format=%s "$1"; else head -n 1; fi \
  | grep -oE '#[0-9]{3,}' | awk '!seen[$0]++' | paste -sd' '
exit 0
