import sys; sys.path.insert(0,'.')
import bzcore_trust as B
from collections import Counter
class A: pass
a=A(); a.len=5; a.with_if=False; a.with_try=False; a.with_while=False; a.body=2
vs=['a','b','c']; feats=B.FEATURE_SETS['trust']
for name,flips in [('baseline',{}),('derived',dict(derived_retention=True)),('sharp',dict(linked_fmove_carries=True)),('both',dict(derived_retention=True,linked_fmove_carries=True))]:
    with B.mutated(flips):
        rows,_=B.sweep(a,vs,feats)
    res=[r for r in B.accepted(rows) if r.bad]
    c=Counter(tuple(r.bad) for r in res)
    print(f"== {name}: residual/unsound accepted programs = {len(res)} {dict(c)}")
    for r in B.minimal(res,8): print('   ',B.oneline(r.prog),'runs:',','.join(r.outs))
    # which of the residuals are puse-based vs kuse-based
    print('    via puse:',sum(any(s[0]=='puse' for s in B.iter_stmts(r.prog)) for r in res),'via kuse:',sum(any(s[0]=='kuse' for s in B.iter_stmts(r.prog)) for r in res))
