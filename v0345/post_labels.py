"""post_labels.py [--write]: apply the label changes and the one retitle of scratch/v0345/trackers.json — `remove` on
every `close` (`spec-question`, `spec-defect` or `needs-owner-decision`, whichever the 20 closed issues carry; #7275
loses `needs-owner-decision`: it is decided under the delegation and listed for the owner's review in the changelog
entry), and `remove`/`add`/`retitle` on the three decided issues that stay open as their own trackers (#7273:
`spec-question` -> `spec-tracking`, retitled as proposal 0200's tracker; #6772 and #7281: `spec-question` ->
`spec-tracking`). scratch/post_land.py posts the comments and closes but reads none of these fields; run this after it.
Dry run by default (prints what it would do); `--write` calls `gh api` (DELETE …/issues/N/labels/NAME,
POST …/issues/N/labels, PATCH …/issues/N with the new title). A label the issue already carries is not re-added
(POST is additive and idempotent)."""
import json, subprocess, sys, urllib.parse
W = '--write' in sys.argv


def api(path, method, payload=None):
    if not W:
        print('DRY', method, path, json.dumps(payload, ensure_ascii=False) if payload else '')
        return
    cmd = ['gh', 'api', path, '--method', method]
    r = subprocess.run(cmd + (['--input', '-'] if payload else []), input=json.dumps(payload) if payload else None,
                       capture_output=True, text=True)
    print(path, method, 'OK' if r.returncode == 0 else 'FAIL ' + r.stderr[:200])


for e in json.load(open('/home/user/bezel-lang/scratch/v0345/trackers.json')):
    if e.get('action') not in ('close', 'comment'):
        continue
    n = e['issue']
    for lab in e.get('remove', []):
        api(f"repos/s8lap/bezel-lang/issues/{n}/labels/{urllib.parse.quote(lab)}", 'DELETE')
    if e.get('add'):
        api(f"repos/s8lap/bezel-lang/issues/{n}/labels", 'POST', {'labels': e['add']})
    if e.get('retitle'):
        api(f"repos/s8lap/bezel-lang/issues/{n}", 'PATCH', {'title': e['retitle']})
