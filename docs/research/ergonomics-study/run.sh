#!/usr/bin/env bash
# Re-run the ergonomics study: run.sh BEZELC [--drafts] [--only PATTERN] [--keep] [--timeout S]
# See README.md, "Re-running the study". The work is done by run.py (python3 stdlib only).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ $# -lt 1 ]]; then
  echo "usage: $0 BEZELC [--drafts] [--only PATTERN] [--keep] [--timeout SECONDS] [--workdir DIR]" >&2
  exit 2
fi
exec python3 -I "$here/run.py" "$@"
