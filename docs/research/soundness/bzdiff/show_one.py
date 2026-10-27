#!/usr/bin/env python3
"""show_one.py "<key>" — print, compile and run one model program and summarise both sides."""
import json, subprocess, sys, os
here = os.path.dirname(os.path.abspath(__file__))
out = subprocess.run([sys.executable, '-I', os.path.join(here, 'bzdiff.py'), 'one', sys.argv[1], '--out', os.path.join(here, 'runs', 'one')],
                     capture_output=True, text=True).stdout
src, js = out.split('===JSON===\n')
r = json.loads(js)
if '--src' in sys.argv: print(src)
print('model   :', r['model']['raw'], r['model']['rawwarns'], 'bad=' + ','.join(r['model']['bad']))
print('compiler:', r['compiler']['kind'], r['compiler']['reject'] or '', r['compiler']['warns'])
for m in r['compiler']['msgs'][:3]: print('   >', m[:230])
print('L1:', r['l1'], ' L2:', r.get('l2', '-'))
for p in r.get('paths', []):
    print(' path', repr(p['choices']), p['verdict'])
    print('    M', p['model']['outcome'], ' '.join(f'{k[0].upper()}{i}' for k, i in p['model']['trace']), ('term=' + str(p['model']['term']) if p['model']['term'] else ''))
    print('    C', p['compiler']['outcome'], ' '.join(f'{k[0].upper()}{i}' for k, i in p['compiler']['trace']))
if 'unchecked' in r: print(' unchecked:', r['unchecked'])
if 'compiler_only' in r: print(' compiler-only run:', json.dumps(r['compiler_only']))
