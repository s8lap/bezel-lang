"""Self-test for scripts/regression_matrix.py (build-free, no real bezelc).

A fake `bezelc` (a shell script) compiles each program into a fake executable
whose behaviour depends on which compiler "tag" built it, so the generator, the
classifier and the later-worse comparison are exercised end to end in about a
second. The real matrix is run by hand; see tests/README.md, "Regression matrix".
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
import regression_matrix as rm  # noqa: E402

FAKE = r'''#!/bin/bash
# fake bezelc: -c -o LIB src  => ok;  [--sanitize=address] [--import X] -o EXE src
out=""; src=""; lib=0
while [ $# -gt 0 ]; do
  case "$1" in -o) out="$2"; shift 2;; -c) lib=1; shift;; --import) shift 2;; --*) shift;; *) src="$1"; shift;; esac
done
if [ $lib = 1 ]; then : > "$out"; exit 0; fi
case "$FAKE_MODE" in
  cf) echo "$src:1:1: error: nope" >&2; exit 1;;
esac
case "$FAKE_MODE" in
  leak) body='echo 1; echo "ERROR: LeakSanitizer: detected memory leaks" >&2; echo "SUMMARY: AddressSanitizer: 16 byte(s) leaked in 2 allocation(s)." >&2; exit 1';;
  crash) body='echo "ERROR: AddressSanitizer: SEGV on unknown address" >&2; exit 1';;
  out) body='echo 2';;
  *) body='echo 1';;
esac
printf '#!/bin/bash\n%s\n' "$body" > "$out"; chmod +x "$out"
'''


class RegressionMatrixTest(unittest.TestCase):
    def test_generation_is_deterministic_and_unique(self):
        with tempfile.TemporaryDirectory() as d:
            a = rm.generate(f'{d}/a', 'Set')
            b = rm.generate(f'{d}/b', 'Set')
            self.assertEqual([c[0] for c in a], [c[0] for c in b])
            self.assertEqual(len({c[0] for c in a}), len(a))
            for c in a:
                self.assertEqual(pathlib.Path(f'{d}/a/{c[0]}.bzl').read_text(),
                                 pathlib.Path(f'{d}/b/{c[0]}.bzl').read_text())
            self.assertEqual(pathlib.Path(f'{d}/a/LibX.bzl').read_text(),
                             pathlib.Path(f'{d}/b/LibX.bzl').read_text())

    def test_filter_and_scope(self):
        names = [c[0] for c in rm.cells('SortedSet', 'app')]
        self.assertTrue(names and all(n.startswith('app_') for n in names))
        self.assertTrue(any('SS_c_' in n for n in names))       # SortedSet<Cmp>
        self.assertFalse(any('_St_i_' in n for n in names))      # plain Set<int> is not SortedSet
        ops = {c[3] for c in rm.cells('genfield')}
        self.assertEqual(ops, {'genfield'})
        self.assertTrue(any(c[2] == rm.GENERIC_KEY for c in rm.cells('gb_s')))

    def test_required_element_kinds_present(self):
        tys = {v[0] for v in rm.T.values()}
        for want in ('Set<P>', 'Set<E>', 'Set<int?>', 'Set<Item?>', 'Set<IS>', 'SortedSet<Cmp>',
                     'Set<V>', 'Set<decimal>', 'SortedSet<P2>'):
            self.assertIn(want, tys)
        gen = ' '.join(rm.GENERIC_OPS.values())
        self.assertIn('GB<int>', gen)            # generic class, owned field
        self.assertIn('SinkS<int>', gen)         # generic owned parameter

    def test_classify(self):
        self.assertEqual(rm.classify(False, 0, '', 'boom'), (rm.CF, 'boom'))
        self.assertEqual(rm.classify(True, 0, '', ''), (rm.CLEAN, ''))
        self.assertEqual(rm.classify(True, 134, '', '')[0], rm.CRASH)
        self.assertEqual(rm.classify(True, 1, 'ERROR: AddressSanitizer: SEGV on', '')[0], rm.CRASH)
        self.assertEqual(rm.classify(True, 1, 'free(): invalid pointer', '')[0], rm.CRASH)
        leak = 'ERROR: LeakSanitizer: x\nSUMMARY: AddressSanitizer: 68 byte(s) leaked in 3 allocation(s).'
        self.assertEqual(rm.classify(True, 1, leak, ''), (rm.LEAK, '68B/3'))

    def test_worse_rules(self):
        clean, leak, crash = (rm.CLEAN, '', '1'), (rm.LEAK, '16B/2', '1'), (rm.CRASH, 'SEGV', '')
        self.assertTrue(rm.worse(clean, leak))
        self.assertTrue(rm.worse(leak, crash))
        self.assertTrue(rm.worse(clean, (rm.CLEAN, '', '2')))                 # stdout drift
        self.assertTrue(rm.worse(leak, (rm.LEAK, '32B/2', '1')))              # bigger leak
        self.assertEqual(rm.worse(leak, (rm.LEAK, '8B/1', '1')), '')
        self.assertEqual(rm.worse(crash, (rm.CF, 'x', '')), '')               # reference never ran
        self.assertEqual(rm.worse(leak, clean), '')

    def _run(self, d, modes, *extra):
        fake = pathlib.Path(d) / 'fakebezelc'
        fake.write_text(FAKE)
        fake.chmod(0o755)
        args = [sys.executable, str(ROOT / 'scripts/regression_matrix.py'), '--out', f'{d}/out',
                '--filter', 'Mutex', '--scope', 'loc', '--jobs', '4', '--iterations', '2', *extra]
        env = dict(os.environ)
        # each tag gets its own wrapper so FAKE_MODE differs per compiler
        for tag, mode in modes:
            w = pathlib.Path(d) / f'bz_{tag}'
            w.write_text(f'#!/bin/bash\nFAKE_MODE={mode} exec {fake} "$@"\n')
            w.chmod(0o755)
            args += ['--bezelc', f'{tag}={w}']
        return subprocess.run(args, capture_output=True, text=True, env=env)

    def test_end_to_end_worse_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as d:
            r = self._run(d, [('ref', 'ok'), ('new', 'leak')])
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn('new vs ref:', r.stdout)
            self.assertRegex(r.stdout, r'loc_MX_using\s+ref=CLEAN\s+->\s+new: LEAK 16B/2')
            tsv = pathlib.Path(d, 'out', 'new.tsv').read_text()
            self.assertIn('loc_MX_using\tLEAK\t16B/2\t1 1', tsv)

    def test_end_to_end_not_worse_exits_zero(self):
        with tempfile.TemporaryDirectory() as d:
            r = self._run(d, [('ref', 'crash'), ('new', 'ok')])       # improvement is not a failure
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = self._run(d, [('ref', 'ok'), ('new', 'cf')])           # CF is worse than CLEAN
            self.assertEqual(r.returncode, 1)
            r = self._run(d, [('ref', 'ok'), ('new', 'out')])          # stdout drift
            self.assertEqual(r.returncode, 1)
            self.assertIn('stdout', r.stdout)


if __name__ == '__main__':
    unittest.main()
