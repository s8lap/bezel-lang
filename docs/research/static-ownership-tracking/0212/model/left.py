#!/usr/bin/env python3
"""left.py — what 0212 leaves: for one bound, the accepted programs in which some run reads a dead or foreign object
through a stored borrow, classified by the read that faults and by the kill that made the object dead.
Usage: python3 -I left.py [--kdtor] [--qdtor] [--vars N] [--len N]"""
import argparse, sys, os
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bzcore_0212 as M
ap = argparse.ArgumentParser()
ap.add_argument('--kdtor', action='store_true'); ap.add_argument('--qdtor', action='store_true'); ap.add_argument('--hidden-link', dest='hidden', action='store_true')
ap.add_argument('--draft1', action='store_true', help='0212 as first drafted (f72926c68) instead of the revised rules')
ap.add_argument('--vars', type=int, default=3); ap.add_argument('--len', type=int, default=4)
a = ap.parse_args()
M.RULES.update(M.BASE55)
if a.kdtor: M.RULES.update(k_dtor_reads=True)
if a.qdtor: M.RULES.update(q_dtor_reads=True)
if a.hidden: M.RULES.update(hidden_link=True)
vs = ['a', 'b', 'c', 'd'][:a.vars]; feats = M.FEATURE_SETS['p0212r']
left = Counter(); wit = {}; n = 0
with M.mutated(dict(bbc=True, linked_fmove_carries=True, erasure_rule=True) if a.draft1 else dict(M.REV2)):
    for p in M.enum_programs(vs, a.len, feats):
        v, w, ctx = M.check(p)
        if v != 'accept': continue
        n += 1
        runs = M.run_all(p, ctx)
        bad = [r for r in runs if r.outcome in ('FAULT', 'RACE') and isinstance(r.info, tuple) and r.info[0] in M.HOLDER_READS]
        if not bad: continue
        kinds = {s[0] for s in M.iter_stmts(p)}
        read = sorted({r.info[0] for r in bad}); out = sorted({r.outcome for r in bad})
        if 'fst' in kinds and 'fbrw' in kinds: cause = 'slot kill (fst after fbrw): K5.6'
        elif 'newk2' in kinds and 'disp' in kinds: cause = 'announced kill of one source of two (a two-argument mint, then disp): K4.2'
        elif 'kset' in kinds and 'disp' in kinds: cause = 'announced kill of one source of two (a later store added the second, then disp): K4.2'
        elif 'newko' in kinds and not a.draft1 and False: cause = 'OTHER'
        elif a.draft1 and 'newko' in kinds: cause = 'FIRST DRAFT HOLE: an `out owned` recipient carries nothing (critic c02)'
        elif a.draft1 and 'wrap' in kinds: cause = 'FIRST DRAFT HOLE: an owning choice over a holder carries nothing (critic c05)'
        elif a.draft1 and 'fmove' in kinds: cause = 'FIRST DRAFT HOLE: a child taken out by a replacing move does not carry its owner (critic c03)'
        else: cause = 'OTHER'
        k = (cause, '/'.join(out), '+'.join(read))
        left[k] += 1
        if k not in wit or len(M.oneline(p)) < len(wit[k]): wit[k] = M.oneline(p)
print(f'# what 0212 ' + ('as first drafted' if a.draft1 else 'revised') + f' leaves — vars={a.vars} len={a.len} features=p0212r' + (' --kdtor' if a.kdtor else '') + (' --qdtor' if a.qdtor else '') + (' --hidden-link' if a.hidden else ''))
print(f'accepted: {n}; with a stale read through a stored borrow on some run: {sum(left.values())}')
print('| cause | outcome | the read that meets the flag | programs | shortest witness |')
print('|---|---|---|---|---|')
for k, c in sorted(left.items(), key=lambda kv: -kv[1]):
    print(f'| {k[0]} | {k[1]} | {k[2]} | {c} | `{wit[k]}` |')
