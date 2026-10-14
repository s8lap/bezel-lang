#!/usr/bin/env python3
"""usage: python3 -I run_compile.py <bezelc> <progdir> <outtsv> <objdir>
Compiles every *.bzl in progdir with `<bezelc> -c prog -o objdir/name.o` (60 s timeout, 8 workers).
Writes TSV: name, rc, first-diagnostic-line (abs paths normalised). rc 124 = timeout; negative = signal."""
import os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
bz, pd, out, od = sys.argv[1:5]
os.makedirs(od, exist_ok=True)
names = sorted(f for f in os.listdir(pd) if f.endswith('.bzl'))
def one(n):
    o = os.path.join(od, n[:-4] + '.o')
    try:
        p = subprocess.run([bz, '-c', os.path.join(pd, n), '-o', o], capture_output=True, timeout=60, cwd=od)
        rc = p.returncode
        txt = (p.stderr + p.stdout).decode('utf-8', 'replace')
    except subprocess.TimeoutExpired:
        rc, txt = 124, 'TIMEOUT'
    try: os.unlink(o)
    except OSError: pass
    first = ''
    for l in txt.splitlines():
        if l.strip():
            first = l; break
    first = first.replace(pd.rstrip('/') + '/', '').replace(od, '<obj>')
    first = re.sub(r'\t', ' ', first)[:400]
    return n[:-4], rc, first
with ThreadPoolExecutor(8) as ex, open(out, 'w') as fh:
    for r in ex.map(one, names):
        fh.write('%s\t%d\t%s\n' % r)
