#!/usr/bin/env python3
"""takeout.py — for one bound, the CLEAN rejections of 0212 revised (programs the v0.355 baseline accepts, that never
fault and hold no dangling stored borrow), split by whether the taken-out rule is what rejects them: a program counts
for the rule when it is accepted again with `fmove_carries_holder` switched off.
Usage: python3 -I takeout.py [--kdtor] [--qdtor] [--vars N] [--len N]"""
import argparse, sys, os
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bzcore_0212 as M
ap = argparse.ArgumentParser()
ap.add_argument('--kdtor', action='store_true'); ap.add_argument('--qdtor', action='store_true')
ap.add_argument('--vars', type=int, default=3); ap.add_argument('--len', type=int, default=4)
ap.add_argument('--features', default='p0212r')
a = ap.parse_args()
M.RULES.update(M.BASE55)
if a.kdtor: M.RULES.update(k_dtor_reads=True)
if a.qdtor: M.RULES.update(q_dtor_reads=True)
vs = ['a', 'b', 'c', 'd'][:a.vars]; feats = M.FEATURE_SETS[a.features]
progs = list(M.enum_programs(vs, a.len, feats, False, False, False, 2))
base = {M.key(p): M.evaluate(p)[0] for p in progs}
rej = []
with M.mutated(dict(M.REV2)):
    for p in progs:
        b = base[M.key(p)]
        if b.verdict == 'accept' and M.check(p)[0] != 'accept': rej.append((p, b))
clean = [(p, b) for p, b in rej if M.base_class(b) == 'never(clean)']
n = Counter()
with M.mutated(dict(M.REV2, fmove_carries_holder=False)):
    for p, b in clean:
        kinds = {s[0] for s in M.iter_stmts(p)}
        by_rule = M.check(p)[0] == 'accept'
        n['taken-out rule' if by_rule else 'another rule'] += 1
        if by_rule: n['  of which: no `link` in the program' if 'link' not in kinds else '  of which: the program links a child'] += 1
print(f'# clean rejections of 0212 revised — vars={a.vars} len={a.len} features={a.features}' + (' --kdtor' if a.kdtor else '') + (' --qdtor' if a.qdtor else ''))
print(f'baseline accepted {sum(1 for r in base.values() if r.verdict == "accept")}; rejected by 0212 revised {len(rej)}; of those clean {len(clean)}')
for k in ('taken-out rule', '  of which: no `link` in the program', '  of which: the program links a child', 'another rule'): print(f'{k}: {n[k]}')
