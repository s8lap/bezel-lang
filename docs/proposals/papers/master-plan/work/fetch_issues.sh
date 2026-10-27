#!/usr/bin/env bash
# Fetch every issue (open and closed) of s8lap/bezel-lang via the REST API, page by page.
# GraphQL is blocked in this environment and `gh api --paginate` is unreliable, so each page is
# an explicit request. Output: one JSON array per page under data/pages/<state>-<page>.json,
# then merged into data/issues-<state>.jsonl (one issue per line, PRs removed).
# Usage: fetch_issues.sh [open|closed|all]   (default: all)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/data"
mkdir -p "$out/pages"
states="${1:-all}"
if [ "$states" = all ]; then states="open closed"; fi
for state in $states; do
  page=1
  while :; do
    f="$out/pages/$state-$page.json"
    if [ ! -s "$f" ]; then
      gh api "repos/s8lap/bezel-lang/issues?state=$state&per_page=100&page=$page&sort=created&direction=asc" > "$f.tmp"
      mv "$f.tmp" "$f"
    fi
    n=$(python3 -I -c 'import json,sys; print(len(json.load(open(sys.argv[1]))))' "$f")
    echo "$state page $page: $n" >&2
    [ "$n" -lt 100 ] && break
    page=$((page+1))
  done
  python3 -I - "$out" "$state" <<'PY'
import json, glob, sys, os
out, state = sys.argv[1], sys.argv[2]
seen = set()
with open(os.path.join(out, f"issues-{state}.jsonl"), "w") as w:
    for f in sorted(glob.glob(os.path.join(out, "pages", f"{state}-*.json")), key=lambda p: int(p.rsplit("-",1)[1].split(".")[0])):
        for it in json.load(open(f)):
            if "pull_request" in it or it["number"] in seen:
                continue
            seen.add(it["number"])
            w.write(json.dumps({
                "number": it["number"], "title": it["title"], "state": it["state"],
                "labels": [l["name"] for l in it.get("labels", [])],
                "created_at": it["created_at"], "updated_at": it["updated_at"],
                "closed_at": it.get("closed_at"), "comments": it.get("comments", 0),
                "state_reason": it.get("state_reason"),
                "body_len": len(it.get("body") or ""), "body": it.get("body") or "",
                "milestone": (it.get("milestone") or {}).get("title"),
            }) + "\n")
print(state, len(seen), file=sys.stderr)
PY
done
