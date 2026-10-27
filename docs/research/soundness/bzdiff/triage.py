#!/usr/bin/env python3
"""triage.py RESULTS.jsonl… — group the disagreements of one or more sweeps by signature.

Signature of a level-1 disagreement: (model kind, model family, compiler kind, compiler family).
Signature of a level-2 disagreement: the first disagreeing path's reason plus the model outcome.
For each group: count, the minimal witnesses (shortest program text), and the sweeps it came from.
Also prints the per-sweep summary (programs, level-1 agreement, level-2 agreement).
"""
import json, sys
from collections import Counter, defaultdict


def plen(key):
    return (key.count(' / ') + 1, len(key), key)


def main(paths):
    paths = [p for p in paths if not p.startswith('--')]
    rows = []
    for p in paths:
        for line in open(p):
            rows.append(json.loads(line))
    per = defaultdict(Counter)
    for r in rows:
        c = per[r.get('sweep', '?')]
        if 'error' in r: c['harness-error'] += 1; continue
        c['programs'] += 1
        c['l1-' + r['l1']] += 1
        c['l135-' + r.get('l1_35', r['l1'])] += 1
        if 'l2' in r: c['l2-' + r['l2']] += 1
        if r.get('l2') == 'disagree' and r.get('l1_35') == 'agree': c['l2-disagree-under-r35-model'] += 1
        if r['model']['kind'] != 'reject' and any(p['model'].get('term') for p in r.get('paths', [])):
            c['uncaught-termination-disposal-paths'] += 1
    print('## per sweep')
    for s, c in per.items():
        n = c['programs']; a1 = c['l1-agree']; l2n = c['l2-agree'] + c['l2-disagree']
        a35 = c['l135-agree']
        print(f"{s}: {n} programs; level 1 agree {a1} ({100*a1/max(n,1):.1f}%), reject-family-differs {c['l1-reject-family-differs']}, "
              f"disagree {c['l1-disagree']}; under the r35-gap model: agree {a35} ({100*a35/max(n,1):.1f}%), family-differs {c['l135-reject-family-differs']}, disagree {c['l135-disagree']}; "
              f"level 2: {c['l2-agree']}/{l2n} agree; uncaught-termination paths {c['uncaught-termination-disposal-paths']}; harness errors {c['harness-error']}")
    groups = defaultdict(list)
    use35 = '--r35' in sys.argv        # group by the r35-gap model's verdicts instead
    for r in rows:
        if 'error' in r:
            groups[('harness-error', r['error'][:80])].append(r); continue
        l1 = r.get('l1_35', r['l1']) if use35 else r['l1']
        if l1 != 'agree':
            m, c = (r.get('model35', r['model']) if use35 else r['model']), r['compiler']
            sig = ('L1', m['kind'], m['reject'] or ','.join(m['warns']), c['kind'], c['reject'] or ','.join(c['warns']))
            groups[sig].append(r)
        elif r.get('l2') == 'disagree':
            p = next(p for p in r['paths'] if p['verdict'] != 'agree')
            groups[('L2', p['model']['outcome'], p['verdict'])].append(r)
        if r.get('unchecked') and any(p['outcome'].startswith('crash') or (p['outcome'] not in ('ok', 'FAULT', 'uncaught', 'RACE'))
                                      for p in r['unchecked']['paths']):
            groups[('UNCHECKED', tuple(sorted({p['outcome'] for p in r['unchecked']['paths']})))].append(r)
    print('\n## disagreement groups (count, signature, minimal witnesses)')
    for sig, rs in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        keys = sorted({r['key'] for r in rs}, key=plen)
        sweeps = Counter(r.get('sweep') for r in rs)
        print(f'\n### {len(rs)}  {sig}   sweeps={dict(sweeps)}')
        for k in keys[:4]:
            print('    ' + k)
        r = next(r for r in rs if r['key'] == keys[0])
        if sig[0] == 'L1':
            for m in r['compiler'].get('msgs', [])[:2]: print('      > ' + m[:200])
            if 'compiler_only' in r: print('      compiler-only run:', json.dumps(r['compiler_only'])[:300])
        elif sig[0] == 'L2':
            p = next(p for p in r['paths'] if p['verdict'] != 'agree')
            print('      path', repr(p['choices']), 'M', p['model']['outcome'], p['model']['trace'])
            print('      ', ' ' * len(repr(p['choices'])), 'C', p['compiler']['outcome'], p['compiler']['trace'])


if __name__ == '__main__':
    main(sys.argv[1:])
