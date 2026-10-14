#!/usr/bin/env python3
"""usage: python3 -I diff_results.py <resA.tsv> <resB.tsv> <index.tsv> [<repo> <shaA> <shaB>]  -> differing rows as TSV on stdout

class: a=accept->reject b=reject->accept c=diag text changed d=crash/timeout change
last column test_file_changed: yes if the row's test file is in `git diff --name-only shaA shaB -- tests`,
no if not, unknown if git cannot resolve the range (or the optional repo/sha arguments are absent)."""
import subprocess, sys
def load(p):
    d = {}
    for l in open(p):
        n, rc, first = (l.rstrip('\n').split('\t') + ['', '', ''])[:3]
        d[n] = (int(rc), first)
    return d
A, B = load(sys.argv[1]), load(sys.argv[2])
idx = {}
for l in list(open(sys.argv[3]))[1:]:
    c = l.rstrip('\n').split('\t'); idx[c[0]] = c
changed = None  # None = unknown
if len(sys.argv) > 6:
    try:
        out = subprocess.run(['git', '-C', sys.argv[4], 'diff', '--name-only', sys.argv[5], sys.argv[6], '--', 'tests'],
                             capture_output=True, text=True, check=True).stdout
        changed = set(out.split())
    except (subprocess.CalledProcessError, OSError):
        changed = None
print('class\tname\tfile\tline\ttest\tkind\trcA\trcB\tdiagA\tdiagB\ttest_file_changed')
for n in sorted(A):
    if n not in B: continue
    (ra, da), (rb, db) = A[n], B[n]
    if (ra, da) == (rb, db): continue
    crash = lambda r: r not in (0, 1)
    if crash(ra) != crash(rb) or (crash(ra) and ra != rb): cl = 'd'
    elif ra == 0 and rb != 0: cl = 'a'
    elif ra != 0 and rb == 0: cl = 'b'
    else: cl = 'c'
    i = idx.get(n, [n, '', '', '', '', ''])
    if changed is None: tfc = 'unknown'
    else: tfc = 'yes' if i[1] in changed else 'no'
    print('\t'.join([cl, n, i[1], i[2], i[3], i[4], str(ra), str(rb), da, db, tfc]))
