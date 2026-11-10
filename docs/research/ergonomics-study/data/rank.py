# Re-derive the README rankings from data/frictions.tsv.
import collections,sys
p=sys.argv[1]
rows=[l.rstrip('\n').split('\t') for l in open(p)]
H={h:i for i,h in enumerate(rows[0])}
for col in ('report_theme','bug_cluster'):
    S=collections.defaultdict(dict); R=collections.Counter()
    for r in rows[1:]:
        k=r[H[col]]
        if not k: continue
        t=r[H['task']]; S[k][t]=max(S[k].get(t,0),int(r[H['weight']])); R[k]+=1
    res=sorted(((sum(v.values()),max(v.values()),len(v),R[k],k) for k,v in S.items()),reverse=True)
    print(f'== {col}: score max_weight tasks rows')
    for x in res: print(*x)
