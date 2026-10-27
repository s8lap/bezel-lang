#!/usr/bin/env python3
"""Search the pulled open-issue snapshot (issues/open_issues.json) by regex over title+body."""
import json, re, sys
rows = json.load(open(__file__.rsplit('/', 1)[0] + '/issues/open_issues.json'))
pat = re.compile(sys.argv[1], re.I)
body = '--body' in sys.argv
for r in rows:
    text = r['title'] + ('\n' + r['body'] if body else '')
    if pat.search(text):
        print(f"#{r['number']} [{','.join(r['labels'])}] {r['title'][:150]}")
