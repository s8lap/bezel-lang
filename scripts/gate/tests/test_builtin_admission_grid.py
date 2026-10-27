"""Self-test for scripts/builtin_admission_grid.py (build-free, no real bezelc).

The campaign-2 DONE instrument is a LANDED ORACLE: it must be exercised by
pregate, or a change to its classifier would silently change what "green"
means.  A fake `bezelc` (a shell script) stands in for the compiler and for the
generated program, so the generator, the classifier and the cell table are all
exercised end to end in about a second.  The real grid is run BY HAND at the
gate (like its model `scripts/regression_matrix.py`); see
docs/proposals/papers/campaigns/builtin-call-admission.md.

What is asserted:
  * every `reject` cell NAMES the diagnostic FAMILIES it requires (D5) — a
    reject cell that accepted one guessed wording turned a fix that landed on
    another wording of the same family RED, which is what kept the grid from
    being the campaign's DONE instrument;
  * every reject alternative is ANCHORED (it names the cell's own subject) or
    is one of the compiler's context-free family phrases — a bare phrase such
    as "no matching overload of" would match a diagnostic about a DIFFERENT
    call in the same program (the D3 failure mode);
  * a diagnostic about another call in the same program never satisfies a cell;
  * the classification is STABLE within a family: every alternative of every
    reject cell turns that cell OK;
  * a diagnostic in NO family leaves the cell REJECTED (a false wrong only
    when the diagnostic is genuinely absent);
  * the classifier's five classes: OK / ACCEPTED / REJECTED / ABORT / INTERNAL;
  * a `reject` cell is OK only when the diagnostic contains one of its needles;
  * a `noabort` cell is OK only when the program exits 0 (a runtime FAULT is a
    wrong answer, not a pass);
  * a `fault` cell is OK only when the fault text appears;
  * generation is deterministic, and the TSV's program path is CELL-RELATIVE,
    so two runs into different --out directories produce the same bytes.
"""
import os
import pathlib
import re
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
  reject_msg)  echo "$src:1:1: error: $FAKE_MSG" >&2; exit 1;;
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

# A call-shaped name in a generated program: `Console.WriteLine(`, `sb.Append(`.
CALL_RE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\s*\(')


def _fake_compiler(d):
    p = pathlib.Path(d) / 'fake_bezelc'
    p.write_text(FAKE)
    p.chmod(0o755)
    return str(p)


def _cell(name, expect, body='Console.WriteLine(1);', subject=''):
    """A cell tuple in the grid's shape:
    (name, extra, pre, body, expect, imports, campaign, subject, lib, member)."""
    return (name, '', '', body, expect, '', '2', subject, '', '')


def _needles(expect):
    want = expect[1]
    return [want] if isinstance(want, str) else list(want)


def _anchored(alt, subject):
    """True when the alternative names the cell's own subject (or its head or
    its member), so a diagnostic about ANOTHER call cannot contain it."""
    head, _, short = subject.rpartition('.')
    return bool(subject and subject in alt) or \
           bool(short and short in alt) or \
           bool(head and head in alt)


def _context_free(alt):
    """True when the alternative carries one of the compiler's family phrases
    that name no call."""
    return any(p in alt for p in g.CONTEXT_FREE)


def _decoys(program, subject):
    """Diagnostics about the OTHER calls in the same program — a reject cell
    must not be satisfied by any of them."""
    head, _, short = subject.rpartition('.')
    skip = {subject, head, short}
    out = []
    for n in CALL_RE.findall(program):
        if n in skip:
            continue
        out += [
            f"no matching overload of `{n}`",
            f"no applicable overload of `{n}`",
            f"ambiguous call to `{n}`",
            f"the argument to `{n}` has a type that is not implicitly "
            f"convertible to the target type",
            f"'{n}' has no static method or property 'X' (§3.4)",
            f"type '{n}' has no static method or property 'X' (§3.4)",
            f"codegen cannot dispatch member 'X' on receiver of type '{n}'",
            f"the call to `{n}` supplies 1 argument(s) but stdlib §9.6 declares 2",
            f"explicit type argument(s), but `{n}` declares 0 type parameters",
            f"`{n}` is a reference to a generic method and names no type argument",
            f"the initializer for `x` has a type that is not implicitly "
            f"convertible to the target type",
        ]
    return out


class BuiltinAdmissionGridTest(unittest.TestCase):
    # ------------------------------------------------------------------ shape
    def test_every_reject_cell_lists_its_diagnostic_families(self):
        """D5: a reject cell that accepts ONE guessed wording turns a fix that
        lands on another wording of the same family RED — which is what kept
        the grid from being the campaign's DONE instrument."""
        reject = [c for c in g.CELLS if c[4][0] == 'reject']
        self.assertGreaterEqual(len(reject), 60)
        for name, _e, _p, _b, expect, _i, _camp, subject, _lib, _m in reject:
            needles = _needles(expect)
            self.assertGreaterEqual(len(needles), 2, f'{name}: one guessed wording')
            for n in needles:
                self.assertTrue(n and n.strip(), f'{name}: empty needle')

    def test_every_reject_cell_names_its_subject(self):
        """The subject is what makes a diagnostic about ANOTHER call unable to
        satisfy the cell."""
        for name, _e, _p, _b, expect, _i, _camp, subject, _lib, _m in g.CELLS:
            if expect[0] == 'reject':
                self.assertTrue(subject, f'{name}: a reject cell must name its subject')

    def test_every_reject_alternative_is_anchored_or_a_context_free_phrase(self):
        """A bare phrase ("no matching overload of", "type argument") matches a
        diagnostic about a DIFFERENT call — the D3 failure mode."""
        for name, _e, _p, _b, expect, _i, _camp, subject, _lib, _m in g.CELLS:
            if expect[0] != 'reject':
                continue
            for n in _needles(expect):
                self.assertTrue(_anchored(n, subject) or _context_free(n),
                                f'{name}: the alternative {n!r} names no call and is not '
                                f'one of the compiler\'s context-free family phrases')

    def test_a_diagnostic_about_another_call_never_satisfies_a_cell(self):
        """The false-OK mirror of D3: the cell's other calls cannot produce the
        diagnostic the cell wants."""
        for name, extra, pre, body, expect, imports, _camp, subject, _lib, _m in g.CELLS:
            if expect[0] != 'reject':
                continue
            program = g.prog(extra, pre, body, imports)
            decoys = _decoys(program, subject)
            self.assertTrue(decoys, f'{name}: no decoy was built for this program')
            for n in _needles(expect):
                if not _anchored(n, subject):
                    continue  # a context-free phrase: covered by the record's argument
                for d in decoys:
                    self.assertNotIn(n, d,
                                     f'{name}: the alternative {n!r} is satisfied by the '
                                     f'unrelated diagnostic {d!r}')

    # --------------------------------------------------------------- behaviour
    def _classify(self, mode, expect, body='Console.WriteLine(1);', subject=''):
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d)
            os.environ['FAKE_MODE'] = mode
            try:
                row = g.classify_one(bezelc, os.path.join(d, 'programs'), os.path.join(d, 'cells'),
                                     _cell('c_' + mode, expect, body, subject), False, 30, 30)
            finally:
                os.environ.pop('FAKE_MODE', None)
            return row

    def _classify_msg(self, msg, expect, body='Console.WriteLine(1);', subject=''):
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d)
            os.environ['FAKE_MODE'] = 'reject_msg'
            os.environ['FAKE_MSG'] = msg
            try:
                row = g.classify_one(bezelc, os.path.join(d, 'programs'), os.path.join(d, 'cells'),
                                     _cell('c_msg', expect, body, subject), False, 30, 30)
            finally:
                os.environ.pop('FAKE_MODE', None)
                os.environ.pop('FAKE_MSG', None)
            return row

    def test_the_classification_is_stable_within_a_family(self):
        """N1: every alternative of every reject cell turns that cell OK, so a
        fix that words the SAME diagnostic differently still turns the grid
        green."""
        checked = 0
        for name, extra, pre, body, expect, imports, _camp, subject, _lib, _m in g.CELLS:
            if expect[0] != 'reject':
                continue
            for n in _needles(expect):
                row = self._classify_msg(n, expect, body, subject)
                self.assertEqual(row[6], 'OK',
                                 f'{name}: the diagnostic {n!r} did not satisfy the cell '
                                 f'({row[7]})')
                checked += 1
        self.assertGreaterEqual(checked, 200)

    def test_a_diagnostic_in_no_family_leaves_the_cell_rejected(self):
        """N1: the cell is a FALSE WRONG only when the diagnostic is genuinely
        absent."""
        for name, extra, pre, body, expect, imports, _camp, subject, _lib, _m in g.CELLS:
            if expect[0] != 'reject':
                continue
            row = self._classify_msg('the compiler could not bind this call',
                                      expect, body, subject)
            self.assertEqual(row[6], 'REJECTED', f'{name}: an unrelated diagnostic was accepted')

    def test_reject_cell_is_ok_only_when_the_needle_is_present(self):
        row = self._classify('reject', ('reject', ['no matching overload of `int.Parse`']),
                             subject='int.Parse')
        self.assertEqual(row[6], 'OK')
        row = self._classify('reject_wrong', ('reject', ['no matching overload of `int.Parse`']),
                             subject='int.Parse')
        self.assertEqual(row[6], 'REJECTED')

    def test_compiler_abort_is_its_own_class(self):
        row = self._classify('abort', ('reject', ['no matching overload of `int.Parse`']),
                             subject='int.Parse')
        self.assertEqual(row[6], 'ABORT')
        row = self._classify('signal', ('reject', ['no matching overload of `int.Parse`']),
                             subject='int.Parse')
        self.assertEqual(row[6], 'ABORT')

    def test_internal_degrade_is_its_own_class(self):
        row = self._classify('internal', ('reject', ['no matching overload of `int.Parse`']),
                             subject='int.Parse')
        self.assertEqual(row[6], 'INTERNAL')

    def test_value_cell_checks_the_exact_output(self):
        row = self._classify('rightvalue', ('value', '42\n'))
        self.assertEqual(row[6], 'OK')
        row = self._classify('wrongvalue', ('value', '42\n'))
        self.assertEqual(row[6], 'ACCEPTED')

    def test_noabort_cell_rejects_a_runtime_fault(self):
        """D4: `noabort` must not be vacuous — a program that faults at RUN time
        is a wrong answer, not a pass."""
        row = self._classify('rightvalue', ('noabort', None))
        self.assertEqual(row[6], 'OK')
        row = self._classify('fault', ('noabort', None))
        self.assertEqual(row[6], 'ACCEPTED')

    def test_fault_cell_needs_the_fault_text(self):
        row = self._classify('fault', ('fault', 'IndexOutOfRangeException'))
        self.assertEqual(row[6], 'OK')
        row = self._classify('rightvalue', ('fault', 'IndexOutOfRangeException'))
        self.assertEqual(row[6], 'ACCEPTED')

    def test_cell_names_are_unique_and_generation_is_deterministic(self):
        names = [c[0] for c in g.CELLS]
        self.assertEqual(len(names), len(set(names)))
        with tempfile.TemporaryDirectory() as d:
            a = g.prog('', '', 'Console.WriteLine(1);')
            b = g.prog('', '', 'Console.WriteLine(1);')
            self.assertEqual(a, b)
            self.assertIn('class C', a)

    def test_campaign_tags_are_known(self):
        for name, _e, _p, _b, _e2, _i, camp in [(c[0], c[1], c[2], c[3], c[4], c[5], c[6])
                                                for c in g.CELLS]:
            self.assertIn(camp, ('2', '5'), f'{name}: unknown campaign tag {camp!r}')

    def test_every_cell_names_a_tracker_member_or_is_a_control(self):
        """The map IS the record's cell→member table: every cell has an entry
        and every entry is a tracker number or '' (a control cell)."""
        for name, _e, _p, _b, _e2, _i, _camp, _s, _l, member in g.CELLS:
            self.assertIn(name, g.MEMBER, f'{name}: no MEMBER entry')
            if member:
                self.assertRegex(member, r'^#\d+$', f'{name}: bad member {member!r}')
        self.assertEqual(sorted(g.MEMBER), sorted(c[0] for c in g.CELLS))

    def test_the_cross_module_face_is_covered(self):
        """N5: a class named like a builtin, IMPORTED from another module — a
        distinct resolution path from the same-module one."""
        xmod = [c for c in g.CELLS if c[8]]
        self.assertGreaterEqual(len(xmod), 2)
        for c in xmod:
            self.assertIn('import Lib;', c[5], f'{c[0]}: a cross-module cell must import Lib')
            self.assertIn('module Lib;', c[8], f'{c[0]}: the library must declare module Lib')

    def test_program_path_in_the_row_is_cell_relative(self):
        """D12: an ABSOLUTE path would make two runs into different --out
        directories differ byte-for-byte."""
        row = self._classify('rightvalue', ('value', '42\n'))
        self.assertFalse(os.path.isabs(row[8]), row[8])
        self.assertTrue(row[8].startswith('programs/'), row[8])


if __name__ == '__main__':
    unittest.main()
