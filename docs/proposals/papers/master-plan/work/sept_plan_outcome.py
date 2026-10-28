#!/usr/bin/env python3
"""How did the 2026-09 defect-campaign families fare? For each family section of
docs/proposals/papers/defect-campaign-2026-09.md, list the issue numbers it names and their state today."""
import json, re, sys, os, collections
here=os.path.dirname(os.path.abspath(__file__))
repo=os.path.abspath(os.path.join(here,'../../../../..'))
doc=open(os.path.join(repo,'docs/proposals/papers/defect-campaign-2026-09.md')).read()
state={}
for name in ('issues-open.jsonl','issues-closed.jsonl'):
    for l in open(os.path.join(here,'data',name)):
        i=json.loads(l); state[i['number']]=(i['state'], i.get('state_reason'), i['closed_at'], i['title'])
secs=re.split(r'\n### ', doc)
print("family | issues named | closed | open | open list")
for s in secs[1:]:
    head=s.split('\n',1)[0][:70]
    if 'Open members' not in s and 'members' not in s.lower(): continue
    body=s.split('\n',1)[1]
    # take the 'Open members' paragraph(s) only
    m=re.search(r'\*\*Open members[^\n]*\n(.*?)(?:\n\*\*|\n### |\Z)', body, re.S)
    txt=m.group(0) if m else body
    nums=sorted({int(n) for n in re.findall(r'#(\d{3,4})', txt)})
    cl=[n for n in nums if state.get(n,('?',))[0]=='closed']
    op=[n for n in nums if state.get(n,('?',))[0]=='open']
    print(f"{head} | {len(nums)} | {len(cl)} | {len(op)} | {' '.join('#'+str(n) for n in op)}")
anchors=[2718,4146,4212,4140,4125,2924,3493,1877,3379,4453,4454,3460,4452,3733,3682,3732,3892,3895,3917,4383]
print("\nanchors / decisions:")
for a in anchors:
    st=state.get(a); print(a, st[0] if st else '?', (st[2] or '')[:10] if st else '', (st[3][:90] if st else ''))
