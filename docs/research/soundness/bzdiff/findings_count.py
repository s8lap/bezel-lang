#!/usr/bin/env python3
"""findings_count.py RESULTS.jsonl… — attribute every disagreement row to a FINDINGS.md entry by
shape, and print the totals used in FINDINGS.md (per sweep and overall)."""
import json, sys
from collections import Counter, defaultdict


def stmts(key):
    return [l.strip().split()[0] for l in key.split(' / ') if l.strip() and l.strip()[0] not in '}']


def classify(r):
    """Returns the finding id a disagreeing row belongs to (under the r35-gap model), or None."""
    k = r['key']; ks = stmts(k)
    m = r.get('model35', r['model']); c = r['compiler']
    l1 = r.get('l1_35', r['l1']); l2 = r.get('l2')
    if c['reject'] and c['reject'].startswith('PARSE'):
        return 'F5 replacing move unparseable (#5923)'
    if 'tbrw' in ks and (l1 == 'disagree' or l2 == 'disagree'):
        return 'F1 temporary field borrow: escape admitted, temporary never disposed'
    if l1 == 'disagree':
        mk, ck = m['kind'], c['kind']
        if mk == 'reject' and ck != 'reject':
            f = m['reject']
            if f == 'state-match': return 'F3 borrow into owned-state local (#4846)'
            if f == 'consume-of-borrow': return 'F4 move of a borrow accepted'
            if f == 'read-of-moved' and ck == 'warn': return 'F6 moved-from state lost after Dispose()+consume'
            if f == 'dispose-of-borrow' and ck == 'warn': return 'F6 moved-from state lost after Dispose()+consume'   # sibling: a borrow of the disposed binding may call Dispose()
            if f == 'read-of-moved' and ck == 'accept': return 'F10 field store through a moved-from binding (SIGSEGV)'
            if f == 'using-pin' and ck == 'warn': return 'F7 using var moved out after Dispose()'
            if f == 'using-pin' and ck == 'accept' and 'throw' in ks: return 'F15 throw of a using var (spec question: A5 vs K3.8)'
            if f == 'freeze': return 'F8 borrow-pin does not freeze its source'
            if f == 'using-unmarked': return 'F2 unmarked using var over an owned binding (#5582)'
            return 'other: model reject ' + f + ' / compiler ' + ck
        if mk != 'reject' and ck == 'reject':
            if c['reject'] == 'state-match': return 'F13 conditional consume rejected at scope end'
            if c['reject'] == 'freeze' and 'if' in ks: return 'F18 freeze liveness counts a use on the other arm (over-rejection)'
            if any('maybe-consumed join' in x for x in c['msgs']): return 'F16 reassignment of a maybe-consumed local rejected (#5810)'
            return 'other: model ' + mk + ' / compiler reject ' + c['reject']
        if mk == 'warn' and ck == 'accept':
            if 'fst' in ks: return 'F14 missing §28.2 warning at a field store through a disposed binding'
            return 'other: model warn / compiler accept'
        if mk == 'accept' and ck == 'warn':
            return 'other: model accept / compiler warn ' + ','.join(c['warns'])
        return 'other: ' + mk + '/' + ck
    if l2 == 'disagree':
        p = next(p for p in r['paths'] if p['verdict'] != 'agree')
        if 'decl' in ks and 'asg' in ks and p['verdict'] == 'trace differs':
            return 'F11 drop-on-overwrite skipped for an adopted local (leak)'
        if 'try' in ks and p['verdict'] == 'trace differs' and p['compiler']['outcome'] == 'uncaught':
            return 'F13 conditional consume rejected at scope end'   # sibling: on the exception edge the maybe-consumed local is leaked instead
        if 'if' in ks and p['verdict'] == 'trace differs' and p['compiler']['outcome'] == 'uncaught':
            return 'F17 exception edge skips a local that a LATER binding transfer consumes (leak)'
        return 'other L2: ' + p['verdict']
    return None


def main(paths):
    rows = [json.loads(l) for p in paths for l in open(p)]
    tot = Counter(); per = defaultdict(Counter); fam = Counter(); fam_sweep = defaultdict(Counter)
    unchecked_crash = 0; race = 0; term = 0
    for r in rows:
        if 'error' in r: continue
        s = r.get('sweep', '?'); c = per[s]
        c['programs'] += 1; tot['programs'] += 1
        for cfg, key in (('spec', 'l1'), ('r35', 'l1_35')):
            v = r.get(key, r['l1'])
            c[f'{cfg}-{v}'] += 1; tot[f'{cfg}-{v}'] += 1
        if 'l2' in r:
            c['l2-' + r['l2']] += 1; tot['l2-' + r['l2']] += 1
            if any(p['model'].get('term') for p in r['paths']): term += 1
            if any(p['compiler']['outcome'] == 'RACE' for p in r['paths']): race += 1
        if r.get('unchecked') and any(p['outcome'].startswith('crash') for p in r['unchecked']['paths']):
            unchecked_crash += 1
        f = classify(r)
        if f:
            fam[f] += 1; fam_sweep[f][s] += 1
    print('## totals'); print(dict(tot))
    print(f'unchecked-build crashes among both-accepted programs: {unchecked_crash}; RACE outcomes: {race}; programs with an uncaught-exception termination disposal: {term}')
    print('\n## per sweep')
    for s, c in per.items():
        n = c['programs']
        print(f"| {s} | {n} | {c['spec-agree']} ({100*c['spec-agree']/n:.1f}%) | {c['spec-reject-family-differs']} | {c['spec-disagree']} | "
              f"{c['r35-agree']} ({100*c['r35-agree']/n:.1f}%) | {c['r35-reject-family-differs']} | {c['r35-disagree']} | "
              f"{c['l2-agree']}/{c['l2-agree'] + c['l2-disagree']} |")
    print('\n## rows per finding (under the r35-gap model)')
    for f, n in fam.most_common():
        print(f'{n:6d}  {f}   {dict(fam_sweep[f])}')


if __name__ == '__main__':
    main(sys.argv[1:])
