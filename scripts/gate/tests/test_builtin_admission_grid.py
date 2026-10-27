"""Self-test for scripts/builtin_admission_grid.py (build-free, no real bezelc).

The campaign-2 DONE instrument is a LANDED ORACLE: it must be exercised by
pregate, or a change to its classifier would silently change what "green"
means.  A fake `bezelc` (a shell script) stands in for the compiler and for the
generated program, so the generator, the classifier and the cell table are all
exercised end to end in about a second.  The real grid is run by hand (and at
the gate); see docs/proposals/papers/campaigns/builtin-call-admission.md.

What is asserted:
  * every `reject` cell NAMES the diagnostic it requires (D5) — a reject cell
    with no needle would accept "any error", which is how an unrelated
    diagnostic once made a cell green;
  * the classifier's five classes: OK / ACCEPTED / REJECTED / ABORT / INTERNAL;
  * a `reject` cell is OK only when the diagnostic contains the needle, and is
    REJECTED (not OK) when it does not;
  * a `noabort` cell is OK only when the program exits 0 (a runtime FAULT is a
    wrong answer, not a pass);
  * a `fault` cell is OK only when the fault text appears;
  * generation is deterministic, and the TSV's program path is CELL-RELATIVE,
    so two runs into different --out directories produce the same bytes.
"""
import os
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
import builtin_admission_grid as g  # noqa: E402

FAKE = r'''#!/bin/bash
# fake bezelc: -o EXE [--sanitize=address] SRC
out=""; src=""
while [ $0 ] && [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2;;
    --sanitize=address) shift;;
    *) src="$1"; shift;;
  esac
done
case "$FAKE_MODE" in
  reject)      echo "$src:1:1: error: no matching overload of \`int.Parse\`" >&2; exit 1;;
  reject_wrong) echo "$src:1:1: error: something else entirely" >&2; exit 1;;
  abort)       echo "bezelc: codegen error: IR verification failed (malformed IR emitted)" >&2; exit 1;;
  signal)      kill -SEGV $$ ;;
  internal)    echo "$src:1:1: warning: internal: lowerLocalDecl's initializer expression failed to lower" >&2; exit 0;;
  fault)       body='echo "Unhandled exception: System.IndexOutOfRangeException" >&2; exit 70';;
  wrongvalue)  body='echo 999';;
  rightvalue)  body='echo 42';;
  *)           body='echo 42';;
esac
printf '#!/bin/bash\n%s\n' "$body" > "$out"; chmod +x "$out"
'''


def _fake_compiler(d):
    p = pathlib.Path(d) / 'fake_bezelc'
    p.write_text(FAKE)
    p.chmod(0o755)
    return str(p)


def _cell(name, expect, body='Console.WriteLine(1);'):
    """A cell tuple in the grid's shape: (name, extra, pre, body, expect, imports, campaign)."""
    return (name, '', '', body, expect, '', '2')


class BuiltinAdmissionGridTest(unittest.TestCase):
    def test_every_reject_cell_names_its_diagnostic(self):
        """D5: a reject cell with no needle accepts ANY error — that is how an
        unrelated diagnostic (a missing import) once made a cell green."""
        for name, _extra, _pre, _body, expect, _imports, _camp in g.CELLS:
            if expect[0] == 'reject':
                self.assertTrue(expect[1], f'{name}: a reject cell must name its needle')
                self.assertTrue(isinstance(expect[1], (str, list)))
                needles = [expect[1]] if isinstance(expect[1], str) else expect[1]
                for n in needles:
                    self.assertTrue(n and n.strip(), f'{name}: empty needle')

    def test_cell_names_are_unique_and_generation_is_deterministic(self):
        names = [c[0] for c in g.CELLS]
        self.assertEqual(len(names), len(set(names)))
        with tempfile.TemporaryDirectory() as d:
            a = g.prog('', '', 'Console.WriteLine(1);')
            b = g.prog('', '', 'Console.WriteLine(1);')
            self.assertEqual(a, b)
            self.assertIn('class C', a)

    def test_campaign_tags_are_known(self):
        for name, _e, _p, _b, _e2, _i, camp in g.CELLS:
            self.assertIn(camp, ('2', '5'), f'{name}: unknown campaign tag {camp!r}')

    def _classify(self, mode, expect, body='Console.WriteLine(1);'):
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d)
            os.environ['FAKE_MODE'] = mode
            try:
                row = g.classify_one(bezelc, os.path.join(d, 'programs'), os.path.join(d, 'cells'),
                                     _cell('c_' + mode, expect, body), False, 30, 30)
            finally:
                os.environ.pop('FAKE_MODE', None)
            return row

    def test_reject_cell_is_ok_only_when_the_needle_is_present(self):
        row = self._classify('reject', ('reject', 'no matching overload of `int.Parse`'))
        self.assertEqual(row[4], 'OK')
        row = self._classify('reject_wrong', ('reject', 'no matching overload of `int.Parse`'))
        self.assertEqual(row[4], 'REJECTED')

    def test_compiler_abort_is_its_own_class(self):
        row = self._classify('abort', ('reject', 'no matching overload of `int.Parse`'))
        self.assertEqual(row[4], 'ABORT')
        row = self._classify('signal', ('reject', 'no matching overload of `int.Parse`'))
        self.assertEqual(row[4], 'ABORT')

    def test_internal_degrade_is_its_own_class(self):
        row = self._classify('internal', ('reject', 'no matching overload of `int.Parse`'))
        self.assertEqual(row[4], 'INTERNAL')

    def test_value_cell_checks_the_exact_output(self):
        row = self._classify('rightvalue', ('value', '42\n'))
        self.assertEqual(row[4], 'OK')
        row = self._classify('wrongvalue', ('value', '42\n'))
        self.assertEqual(row[4], 'ACCEPTED')

    def test_noabort_cell_rejects_a_runtime_fault(self):
        """D4: `noabort` must not be vacuous — a program that faults at RUN time
        is a wrong answer, not a pass."""
        row = self._classify('rightvalue', ('noabort', None))
        self.assertEqual(row[4], 'OK')
        row = self._classify('fault', ('noabort', None))
        self.assertEqual(row[4], 'ACCEPTED')

    def test_fault_cell_needs_the_fault_text(self):
        row = self._classify('fault', ('fault', 'IndexOutOfRangeException'))
        self.assertEqual(row[4], 'OK')
        row = self._classify('rightvalue', ('fault', 'IndexOutOfRangeException'))
        self.assertEqual(row[4], 'ACCEPTED')

    def test_program_path_in_the_row_is_cell_relative(self):
        """D12: an ABSOLUTE path would make two runs into different --out
        directories differ byte-for-byte."""
        row = self._classify('rightvalue', ('value', '42\n'))
        self.assertFalse(os.path.isabs(row[6]), row[6])
        self.assertTrue(row[6].startswith('programs/'), row[6])


if __name__ == '__main__':
    unittest.main()
