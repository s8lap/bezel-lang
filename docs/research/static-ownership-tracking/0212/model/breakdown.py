#!/usr/bin/env python3
"""breakdown.py — for one bound, the reasons 0212 rejects a program the v0.355 baseline accepts, by what the program
did at the baseline. Usage: python3 -I breakdown.py [--kdtor] [--qdtor] [--r40-teardown] [--vars N] [--len N] [--if] [--block]"""
import argparse, sys, os
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bzcore_0212 as M
ap = argparse.ArgumentParser()
ap.add_argument('--kdtor', action='store_true'); ap.add_argument('--qdtor', action='store_true'); ap.add_argument('--r40-teardown', dest='r40', action='store_true')
ap.add_argument('--vars', type=int, default=3); ap.add_argument('--len', type=int, default=4)
ap.add_argument('--if', dest='with_if', action='store_true'); ap.add_argument('--block', dest='with_block', action='store_true')
ap.add_argument('--features', default='p0212'); ap.add_argument('--explicit', action='store_true')
a = ap.parse_args()
M.RULES.update(M.BASE55)
if a.kdtor: M.RULES.update(k_dtor_reads=True)
if a.qdtor: M.RULES.update(q_dtor_reads=True)
if a.r40: M.RULES.update(teardown_live=False)
M.WITH_BLOCK[0] = a.with_block
vs = ['a', 'b', 'c', 'd'][:a.vars]; feats = M.FEATURE_SETS[a.features]
flips = dict(bbc=True, linked_fmove_carries=True, erasure_rule=True, mint_stored_only=a.explicit)
progs = list(M.enum_programs(vs, a.len, feats, a.with_if, False, False, 2))
base = {}
for p in progs:
    row, _ = M.evaluate(p)
    base[M.key(p)] = row
why = Counter(); wit = {}
with M.mutated(flips):
    for p in progs:
        b = base[M.key(p)]
        if b.verdict != 'accept': continue
        v, w, ctx = M.check(p)
        if v == 'accept': continue
        k = (v.split(':', 1)[1], M.base_class(b))
        why[k] += 1
        if k not in wit or len(M.oneline(p)) < len(wit[k]): wit[k] = M.oneline(p)
print(f'# 0212 rejections of baseline-accepted programs — vars={a.vars} len={a.len} features={a.features}' + (' --if' if a.with_if else '') + (' --block' if a.with_block else '') + (' --kdtor' if a.kdtor else '') + (' --qdtor' if a.qdtor else '') + (' --r40-teardown' if a.r40 else '') + (' [explicit mint]' if a.explicit else ''))
print(f'programs {len(progs)}; baseline accepted {sum(1 for r in base.values() if r.verdict == "accept")}; rejected by 0212: {sum(why.values())}')
print('| rejecting rule | what the program did at the baseline | programs | shortest witness |')
print('|---|---|---|---|')
for (rule, cls), n in sorted(why.items(), key=lambda kv: (kv[0][0], -kv[1])):
    print(f'| {rule} | {cls} | {n} | `{wit[(rule, cls)]}` |')
