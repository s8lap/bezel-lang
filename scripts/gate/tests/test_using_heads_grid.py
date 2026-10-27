"""Self-test for scripts/using_heads_grid.py (build-free, no real bezelc).

The campaign-1 (`using-heads`) DONE instrument must be EXERCISED, or a change
to its classifier would silently change what "green" means. `scripts/gate/
tests/` is discovered by `scripts/pregate.sh` (it runs
`python3 -m unittest discover -s scripts/gate/tests`), so this file is the
grid's presence at a gate. The real grid is run by hand (and at the gate);
see docs/proposals/papers/campaigns/using-heads.md.

What is asserted — each item is a defect the reviewer demonstrated:

  * every REJ cell's needle NAMES THE RULE it decides (a § marker), and no
    needle is a generic token ("using var" is the §28.3 must-use REMEDY, so a
    cell that named it was green on §28.3's rule while claiming §28.10's — and
    it went RED when a lane implemented §28.10 properly);
  * a needle is matched against the diagnostic that REJECTED the program, not
    against a warning that happens to cite the same section;
  * the §28.9 vacuity lint is recognised by the rule it names, not by its
    wording;
  * every KNOWN-RED row names a fixing commit that exists and whose subject
    names the issue, and a KNOWN-RED cell BLOCKS the DONE verdict (the
    reviewer ran a filter that printed "5 known-red" and exited 0 with five
    cells red and leaking);
  * a quarantined cell (the `task` row, pending #6194) is held out of the
    DONE count and reported on its own line, so the verdict does not flip
    run-to-run;
  * the spelling-agreement check (§28.12's "takes §28.10's admission
    UNCHANGED") detects a disagreement — in the RUN family as well as the
    REJ family — and does NOT judge the head whose spec question is the
    OWNER's;
  * the classifier pins every verdict it can return (CLEAN / LEAK / CRASH /
    CF), because a cell that leaks must be classified LEAK;
  * a TIMEOUT cell (a program that hangs the compiler) BLOCKS the DONE
    verdict, and the sanitizer counter is its OWN counter — a timeout is not
    a sanitizer skip;
  * the EXIT STATUS matches the printed verdict (a grid that prints "DONE:
    NO" and exits 0 reports a red campaign as a pass), and a stale
    executable in the `--out` directory cannot be counted as a cell's
    sanitizer check;
  * the §28.9 vacuity rule (36 of the 153 cells) is driven through the
    grid's OWN VAC/NOVAC branch, and a rejection under the rule a cell names
    makes that cell GREEN;
  * the report TSV has one row per cell with five fields, and two runs into
    different `--out` directories produce byte-identical bytes.
"""
import contextlib
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
import using_heads_grid as g  # noqa: E402

# The tokens that must NEVER be a cell's discriminator: each of them is a
# substring of a NEIGHBOURING rule's diagnostic.
GENERIC_TOKENS = {
    'using var', 'Dispose', 'DisposeAsync', 'is vacuous',
    'reflection is not yet implemented',
}

FAKE = r'''#!/bin/bash
# fake bezelc: --sanitize=address -o EXE SRC
out=""; src=""
while [ $0 ] && [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2;;
    --sanitize=address) shift;;
    *) src="$1"; shift;;
  esac
done
case "$FAKE_MODE" in
  reject)      echo "$src:80:5: owned value in \`src\` would be dropped here without disposal (§28.3 — consider \`using var\`, explicit \`.Dispose()\`, \`move\`, or \`return\`)" >&2; exit 1;;
  reject_2810) echo "$src:80:5: an unmarked \`using var y := x;\` adopts a named owned binding (§28.10)" >&2; exit 1;;
  warn_only)   echo "$src:80:5: warning: \`using var\` \`x\` pins a source that already outlives this scope (§28.10 / §21.1)" >&2
               echo "$src:80:5: owned value in \`src\` would be dropped here without disposal (§28.3 — consider \`using var\`, explicit \`.Dispose()\`, \`move\`, or \`return\`)" >&2; exit 1;;
  vacuous)     echo "$src:80:5: warning: \`using var\` \`x\` is vacuous — the RHS is a value-typed/runtime-managed value with no disposal obligation (§28.9); use \`const x := …;\` for a fixed alias without the disposal hint (§28.10 / §21.1)" >&2
               printf '#!/bin/bash\necho END\n' > "$out"; chmod +x "$out"; exit 0;;
  *)           echo "$src:80:5: warning: nothing" >&2;;
esac
printf '#!/bin/bash\necho 42\n' > "$out"; chmod +x "$out"
'''


def _fake_compiler(d, mode='default'):
    p = pathlib.Path(d) / 'fake_bezelc'
    p.write_text(FAKE)
    p.chmod(0o755)
    os.environ['FAKE_MODE'] = mode
    return str(p)


def _git(*args):
    return subprocess.run(['git', '-C', str(ROOT)] + list(args),
                          capture_output=True, text=True)


def _run_main(argv):
    """Drive the grid's `main()` and return (exit status, what it printed).

    The exit status is the grid's DONE verdict as a shell sees it, and it is
    the only thing a caller (the campaign's runner, a CI job) can see: a grid
    that prints "DONE: NO" and exits 0 is a grid that reports a red campaign
    as a pass.
    """
    old = sys.argv
    sys.argv = ['using_heads_grid.py'] + list(argv)
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            rc = g.main()
    finally:
        sys.argv = old
    return rc, buf.getvalue()


class UsingHeadsGridTest(unittest.TestCase):
    # ---------------------------------------------------------------- needles
    def test_every_rej_cell_names_the_rule_it_decides(self):
        """H1: the needle must be the RULE's marker, so a diagnostic that
        decides a NEIGHBOURING rule cannot make the cell green — and a correct
        fix (which cites the right section) makes it green."""
        seen = 0
        for (head, form), expect in g.EXPECT.items():
            if expect is None or expect[0] != 'REJ':
                continue
            seen += 1
            needle = expect[1]
            needles = list(needle) if isinstance(needle, (list, tuple)) \
                else [needle]
            self.assertTrue(needles, f'{head}/{form}: no needle')
            for n in needles:
                self.assertIn('§', n,
                              f'{head}/{form}: the needle {n!r} must name the '
                              'rule it decides')
        self.assertGreater(seen, 20)

    def test_no_rej_needle_is_a_generic_token(self):
        """'using var' is the §28.3 must-use remedy; 'Dispose' and
        'DisposeAsync' are member names; 'is vacuous' is the lint's wording."""
        for (head, form), expect in g.EXPECT.items():
            if expect is None or expect[0] != 'REJ':
                continue
            needle = expect[1]
            needles = list(needle) if isinstance(needle, (list, tuple)) \
                else [needle]
            for n in needles:
                self.assertNotIn(n, GENERIC_TOKENS,
                                 f'{head}/{form}: {n!r} is a generic token')

    def test_needle_hit_accepts_a_list(self):
        self.assertTrue(g.needle_hit('see §28.10', ['§28.10', '§28.12']))
        self.assertTrue(g.needle_hit('see §28.12', ['§28.10', '§28.12']))
        self.assertFalse(g.needle_hit('see §28.3', ['§28.10', '§28.12']))
        self.assertTrue(g.needle_hit('see §28.10', '§28.10'))
        self.assertFalse(g.needle_hit('see §28.3', '§28.10'))

    def test_a_warning_may_not_satisfy_a_reject_needle(self):
        """The §28.10 borrow-pin WARNING cites §28.10. A cell that needed
        §28.10's rejection must not be made green by that warning."""
        log = ('prog.bzl:80:5: warning: `using var` `x` pins a source that '
               'already outlives this scope (§28.10 / §21.1)')
        rejecting = '\n'.join(ln for ln in log.splitlines()
                              if ln.strip() and 'warning:' not in ln)
        self.assertFalse(g.needle_hit(rejecting, '§28.10'))

    def test_vacuity_lint_is_recognised_by_its_rule(self):
        vacuous = ('prog.bzl:80:5: warning: `using var` `x` is vacuous — the '
                   'RHS is a value-typed/runtime-managed value with no '
                   'disposal obligation (§28.9); use `const x := …;` for a '
                   'fixed alias without the disposal hint (§28.10 / §21.1)')
        self.assertTrue(g.VACUOUS_RE.search(vacuous))
        must_use = ('prog.bzl:80:5: owned value in `src` would be dropped here '
                    'without disposal (§28.3 — consider `using var`, explicit '
                    '`.Dispose()`, `move`, or `return`)')
        self.assertFalse(g.VACUOUS_RE.search(must_use))

    # ----------------------------------------------------------- classifier
    def test_the_classifier_pins_every_verdict(self):
        """The classifier is the heart of the leak oracle: a program that
        leaks must be classified LEAK, one that crashes CRASH, a clean one
        CLEAN, and one that would not build CF. (The reviewer's mutation
        check showed that a `classify` that never reports LEAK left every
        test GREEN.)"""
        self.assertEqual(g.classify(False, 1, 'error: something'),
                         (g.CF, 'compile failed'))
        self.assertEqual(g.classify(True, 0, ''), (g.CLEAN, ''))
        leak = ('==42==ERROR: LeakSanitizer: 2100 byte(s) leaked in 120 '
                'allocation(s).')
        self.assertEqual(g.classify(True, 0, leak), (g.LEAK, '2100B/120'))
        crash = '==42==ERROR: AddressSanitizer: SEGV on unknown address 0x0'
        self.assertEqual(g.classify(True, 0, crash), (g.CRASH, 'SEGV'))
        self.assertEqual(g.classify(True, 3, ''), (g.CRASH, 'rc=3'))
        self.assertEqual(g.classify(True, 0, 'free(): invalid pointer'),
                         (g.CRASH, 'glibc'))

    # ------------------------------------------------------- the REJ branch
    def test_a_warning_citing_the_section_does_not_make_a_rej_cell_green(self):
        """The grid's own REJ branch must match the needle against the lines
        that REJECTED the program, not against a warning that happens to
        cite the same section. The fake compiler emits a §28.10 WARNING
        beside an unrelated §28.3 ERROR, so the cell (which claims §28.10's
        rejection) must be WRONG — this is the admitting twin the
        fix-forward claims to have closed."""
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d, 'warn_only')
            os.makedirs(os.path.join(d, 'run'))
            os.makedirs(os.path.join(d, 'programs'))
            try:
                out = g.run_cell(bezelc, os.path.join(d, 'run'),
                                 os.path.join(d, 'programs'), 'class',
                                 'copy_local_source', 30, 30, {})
            finally:
                os.environ.pop('FAKE_MODE', None)
        _head, _form, verdict, observed, _detail = out
        self.assertEqual(
            verdict, 'WRONG',
            'a §28.10 WARNING beside an unrelated §28.3 ERROR must not make '
            f'the cell green (got {verdict!r}: {observed!r})')

    # ------------------------------------------------------------ known-red
    def test_known_red_rows_name_a_fixing_commit_that_exists(self):
        """M1: the demotion must be earned — the row names the issue AND the
        commit that fixes the cell, and the commit is real."""
        for (head, form), (issue, needle, commit) in g.KNOWN_RED.items():
            self.assertTrue(issue.startswith('#'), f'{head}/{form}')
            self.assertTrue(needle, f'{head}/{form}: no needle')
            self.assertTrue(commit, f'{head}/{form}: no fixing commit')
            r = _git('cat-file', '-e', commit + '^{commit}')
            self.assertEqual(r.returncode, 0,
                             f'{head}/{form}: {commit} is not a commit')
            subject = _git('log', '-1', '--format=%s', commit).stdout
            self.assertIn(issue, subject,
                          f'{head}/{form}: {commit} does not name {issue}')

    def test_a_known_red_cell_blocks_the_done_verdict(self):
        """M1: the reviewer ran a filter that printed '5 known-red' and EXITED
        0 with five cells red and leaking. A KNOWN-RED cell must block."""
        results = [('value_choice', 'copy_local_source', 'WRONG',
                    'LEAK: 2100B/120', '')]
        rows, counts, jc, quarantined, done, _qv = g.judge(results)
        self.assertEqual(jc.get('KNOWN-RED', 0), 1)
        self.assertFalse(done, 'a KNOWN-RED cell must block the DONE verdict')
        self.assertEqual(counts.get('KNOWN-RED', 0), 1)

    def test_a_known_red_row_needs_its_needle_in_the_observed_text(self):
        """A red that does not carry the needle is a DIFFERENT defect."""
        results = [('value_choice', 'copy_local_source', 'WRONG',
                    'CRASH: heap-use-after-free', '')]
        _rows, _counts, jc, _q, done, _qv = g.judge(results)
        self.assertEqual(jc.get('WRONG', 0), 1)
        self.assertEqual(jc.get('KNOWN-RED', 0), 0)
        self.assertFalse(done)

    # ------------------------------------------------------------ quarantine
    def test_the_task_row_is_quarantined_out_of_the_done_count(self):
        """H3: the Task row flips run-to-run on the SAME build (#6194's hang),
        so it must not be part of a verdict that must not flip."""
        results = [('task', 'using_var', 'WRONG', 'run timeout (hang)', ''),
                   ('class', 'using_var', 'OK', 'clean, output []', '')]
        rows, _counts, jc, quarantined, done, _qv = g.judge(results)
        self.assertEqual(len(quarantined), 1)
        self.assertEqual(jc.get('WRONG', 0), 0)
        self.assertTrue(done, 'a quarantined cell must not block the verdict')
        self.assertEqual([r[2] for r in rows if r[0] == 'task'],
                         ['QUARANTINED'])

    def test_every_quarantined_cell_names_the_issue_that_lifts_it(self):
        for (head, form), issue in g.QUARANTINE.items():
            self.assertTrue(issue.startswith('#'), f'{head}/{form}')

    # ------------------------------------------------------------ agreement
    def test_the_agreement_check_sees_a_disagreement(self):
        """H2: §28.12's "takes §28.10's admission UNCHANGED" — the two
        spellings must agree, and the grid must SEE it."""
        rows = [('class', 'copy_local_source', 'WRONG',
                 'compiled, expected rejection', ''),
                ('class', 'copy_await_source', 'WRONG',
                 'rejected, but no ... in the diagnostic', '')]
        out = g.check_agreement(rows)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0][2], 'MISMATCH')

    def test_agreement_is_silent_when_the_spellings_agree(self):
        rows = [('class', 'copy_local_source', 'WRONG',
                 'compiled, expected rejection', ''),
                ('class', 'copy_await_source', 'WRONG',
                 'compiled, expected rejection', '')]
        self.assertEqual(g.check_agreement(rows), [])

    def test_the_open_question_head_is_not_judged_by_the_agreement_check(self):
        """The `object_class` await-spelling cells turn on §28.12's
        "unchanged" — the OWNER's question. The grid adds no verdict."""
        rows = [('object_class', 'copy_local_source', 'OK', 'clean, output []',
                 ''),
                ('object_class', 'copy_await_source', 'WRONG',
                 'rejected, expected a run', '')]
        self.assertEqual(g.check_agreement(rows), [])

    def test_a_mismatch_blocks_the_done_verdict(self):
        results = [('class', 'copy_local_source', 'WRONG',
                    'compiled, expected rejection', ''),
                   ('class', 'copy_await_source', 'WRONG',
                    'rejected, but no ... in the diagnostic', '')]
        _rows, _counts, jc, _q, done, _qv = g.judge(results)
        self.assertEqual(jc.get('MISMATCH', 0), 1)
        self.assertFalse(done)

    def test_a_mismatch_blocks_the_done_verdict_on_its_own(self):
        """The MISMATCH must be the ONLY blocker here: the plain cell is OK,
        so a `blocking` set without MISMATCH would let this cell family
        through — and a §28.12 "takes §28.10's admission UNCHANGED" violation
        would then print "DONE: YES" once the other red cells are fixed.
        (The fixture of the test above has BOTH cells WRONG, so its WRONG
        blocks independently of the MISMATCH: the reviewer's mutation check
        found that a MISMATCH removed from the blocking set left every test
        GREEN.)"""
        results = [('class', 'copy_local_source', 'OK',
                    'rejected (§28.10)', ''),
                   ('class', 'copy_await_source', 'WRONG',
                    'compiled, expected rejection', '')]
        out = g.check_agreement(results)
        self.assertEqual(len(out), 1, f'expected one MISMATCH, got {out!r}')
        self.assertEqual(out[0][2], 'MISMATCH')
        _rows, _counts, jc, _q, done, _qv = g.judge(results)
        self.assertEqual(jc.get('MISMATCH', 0), 1)
        self.assertEqual(jc.get('WRONG', 0), 0,
                         'this fixture must leave the MISMATCH as the only '
                         'blocker')
        self.assertFalse(done,
                         'a MISMATCH must block the DONE verdict on its own')

    def test_a_mismatch_in_the_run_family_blocks_the_done_verdict(self):
        """The agreement check is load-bearing in the RUN family too: the two
        spellings of a RUN cell must AGREE on the admission (§28.12), and a
        disagreement blocks the DONE verdict. The fixture here is a RUN cell
        — a REJ-family fixture does not exercise this path (the reviewer's
        mutation check found the RUN-family comparison unpinned).

        The plain cell is OK and the await cell is the one that was REJECTED:
        the MISMATCH is then the only blocker, so a `blocking` set without
        MISMATCH is caught here."""
        rows = [('class', 'using_var', 'OK', 'clean, output []', ''),
                ('class', 'await_using_var', 'WRONG',
                 'rejected, expected a run', '')]
        out = g.check_agreement(rows)
        self.assertEqual(len(out), 1, f'expected one MISMATCH, got {out!r}')
        self.assertEqual(out[0][2], 'MISMATCH')
        _rows, _counts, jc, _q, done, _qv = g.judge(rows)
        self.assertEqual(jc.get('MISMATCH', 0), 1)
        self.assertEqual(jc.get('WRONG', 0), 0)
        self.assertFalse(done, 'a MISMATCH must block the DONE verdict')

    # ------------------------------------------------- the timeout / sanitizer
    def test_a_timeout_cell_blocks_the_done_verdict(self):
        """A cell whose program hangs the compiler is UNMEASURED. A DONE
        instrument must not print YES with an unmeasured cell — the
        reviewer's measurement: `judge([('class','using_var','TIMEOUT',
        'compile timeout','')])` returned `done=True`."""
        results = [('class', 'using_var', 'TIMEOUT', 'compile timeout', '')]
        _rows, _counts, jc, _q, done, _qv = g.judge(results)
        self.assertEqual(jc.get('TIMEOUT', 0), 1)
        self.assertFalse(done, 'a TIMEOUT cell must block the DONE verdict')

    def test_the_sanitizer_counter_is_its_own_counter(self):
        """`NATIVE SANITIZER: n check(s) skipped` counts the cells whose
        ASan check was skipped because the ASan RUNTIME was unavailable — it
        is NOT the timeout count."""
        with tempfile.TemporaryDirectory() as d:
            odir = os.path.join(d, 'run')
            os.makedirs(odir)
            exe = os.path.join(odir, 'class__using_var.exe')
            with open(exe, 'wb') as f:
                f.write(b'not an asan binary')
            self.assertEqual(
                g.asan_skipped_count(odir, [('class', 'using_var')]), 1)
            with open(exe, 'wb') as f:
                f.write(b'\x00__asan_init\x00')
            self.assertEqual(
                g.asan_skipped_count(odir, [('class', 'using_var')]), 0)

    def test_the_summary_names_the_timeout_and_keeps_the_sanitizer_separate(self):
        """The cells line names the TIMEOUT cells; the sanitizer line carries
        the SANITIZER counter; and the verdict is NO. The DONE line's FULL
        parenthetical is pinned too: the record quotes that line verbatim, so
        a dropped "+ timeout" would make the record's quote drift from the
        code — the class of drift D3 exists to prevent."""
        lines = g.summary_lines(1, {'TIMEOUT': 1}, 0, [], {})
        text = '\n'.join(lines)
        self.assertIn('1 timeout', text)
        self.assertIn('NATIVE SANITIZER: 0 check(s) skipped', text)
        self.assertIn('DONE: NO', text)
        self.assertIn('wrong + known-red + mismatch + timeout', text)

    # ------------------------------------------- the exit status, the counter
    def test_the_exit_status_matches_the_printed_verdict(self):
        """The exit status is the ONLY thing a caller sees. A grid that prints
        "DONE: NO" and exits 0 reports a red campaign as a pass — the
        reviewer's measurement with the mutant: "DONE: NO — 5 cell(s) block
        it" and exit status 0."""
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d, 'default')
            try:
                # The fake's program prints 42, so the cell never reaches its
                # END marker: it is WRONG, and it BLOCKS.
                red_rc, red_out = _run_main(
                    ['--bezelc', bezelc, '--out', os.path.join(d, 'red'),
                     '--filter', 'string using_var', '--jobs', '2'])
                # The `vacuous` fake's program prints END and emits the §28.9
                # vacuity warning: the VAC cell is OK, and nothing blocks.
                # (The fake reads FAKE_MODE when it RUNS, so the mode is
                # switched here, not at the fake's creation.)
                os.environ['FAKE_MODE'] = 'vacuous'
                green_rc, green_out = _run_main(
                    ['--bezelc', bezelc, '--out', os.path.join(d, 'green'),
                     '--filter', 'value_choice using_var', '--jobs', '2'])
            finally:
                os.environ.pop('FAKE_MODE', None)
        self.assertIn('DONE: NO', red_out)
        self.assertEqual(red_rc, 1,
                         'a grid that blocks the DONE verdict must exit 1')
        self.assertIn('DONE: YES', green_out)
        self.assertEqual(green_rc, 0,
                         'a grid with nothing blocking must exit 0')

    def test_a_stale_executable_is_removed_before_the_compile(self):
        """A stale executable from an earlier run in the same `--out`
        directory must not be mistaken for THIS run's: the sanitizer counter
        reads it. With the guard gone, a stale NON-ASan binary makes the
        counter report "1 check(s) skipped" for a cell that never built."""
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d, 'reject')
            odir = os.path.join(d, 'run')
            progdir = os.path.join(d, 'programs')
            os.makedirs(odir)
            os.makedirs(progdir)
            exe = os.path.join(odir, 'class__using_var.exe')
            with open(exe, 'wb') as f:
                f.write(b'\x00a stale, non-ASan binary\x00')
            try:
                status, _clog = g.compile_program(
                    bezelc, odir, progdir, 'class__using_var',
                    'method Main()\nend Main;\n', 30)
            finally:
                os.environ.pop('FAKE_MODE', None)
            self.assertEqual(status, 'rejected')
            self.assertFalse(os.path.exists(exe),
                             'a stale executable must not survive a compile '
                             'that did not produce one')
            self.assertEqual(
                g.asan_skipped_count(odir, [('class', 'using_var')]), 0,
                'a cell that produced NO executable has no sanitizer check to '
                'skip, and must not be counted as one')

    # ------------------------------------------------- the vacuity branch
    def test_the_vacuity_rule_is_driven_by_the_grid_own_branch(self):
        """The §28.9 vacuity rule governs 36 of the 153 cells: a VAC cell
        asserts the vacuity warning IS there, a NOVAC cell asserts it is
        ABSENT. The `vacuous` fake emits it, so the grid's own branch must
        make the VAC cell green and the NOVAC cell red — with the rule
        INVERTED both flip (the reviewer measured three cells' verdicts
        flipping on `--filter string` with no test noticing)."""
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d, 'vacuous')
            odir = os.path.join(d, 'run')
            progdir = os.path.join(d, 'programs')
            os.makedirs(odir)
            os.makedirs(progdir)
            try:
                vac = g.run_cell(bezelc, odir, progdir, 'string',
                                 'using_var', 30, 30, {})
                novac = g.run_cell(bezelc, odir, progdir, 'builtin_collection',
                                   'using_var', 30, 30, {})
            finally:
                os.environ.pop('FAKE_MODE', None)
        self.assertEqual(vac[2], 'OK',
                         'a VAC cell with the §28.9 vacuity warning must be '
                         f'green (got {vac[2]!r}: {vac[3]!r})')
        self.assertEqual(novac[2], 'WRONG',
                         'a NOVAC cell must be red when the §28.9 vacuity '
                         f'warning is emitted (got {novac[2]!r}: {novac[3]!r})')

    def test_a_correct_section_rejection_makes_the_cell_green(self):
        """The REJ branch's POSITIVE direction: a diagnostic that rejects the
        program under the rule the cell names makes the cell GREEN. The
        `reject_2810` fake rejects with §28.10 — the rule
        `class/copy_local_source` names — so the cell must be OK. (Only the
        NEGATIVE direction — a §28.10 warning must not make the cell green —
        was pinned before.)"""
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d, 'reject_2810')
            odir = os.path.join(d, 'run')
            progdir = os.path.join(d, 'programs')
            os.makedirs(odir)
            os.makedirs(progdir)
            try:
                cell = g.run_cell(bezelc, odir, progdir, 'class',
                                  'copy_local_source', 30, 30, {})
            finally:
                os.environ.pop('FAKE_MODE', None)
        self.assertEqual(cell[2], 'OK',
                         'a rejection under the rule the cell names must make '
                         f'the cell green (got {cell[2]!r}: {cell[3]!r})')

    # ------------------------------------------------------------ the report
    def test_report_tsv_has_one_row_per_cell_and_five_fields(self):
        """M3: the detail column used to be a multi-line slice of the compile
        log, so 12 of the 153 rows split across physical lines."""
        rows = [('class', 'using_var', 'OK', 'clean, output []', ''),
                ('class', 'copy_local_source', 'WRONG', 'compiled',
                 'prog.bzl:80:5: line one\nline two\twith a tab')]
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'r.tsv')
            g.write_report(rows, p)
            with open(p, encoding='utf-8') as fh:
                lines = fh.read().splitlines()
        self.assertEqual(len(lines), len(rows) + 1)
        for ln in lines:
            self.assertEqual(len(ln.split('\t')), 5, ln)

    def test_the_report_path_is_cell_relative(self):
        """M4: an ABSOLUTE program path makes two runs into different --out
        directories differ byte-for-byte."""
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d, 'reject')
            os.makedirs(os.path.join(d, 'run'))
            os.makedirs(os.path.join(d, 'programs'))
            try:
                out = g.run_cell(bezelc, os.path.join(d, 'run'),
                                 os.path.join(d, 'programs'), 'class',
                                 'copy_await_source', 30, 30, {})
            finally:
                os.environ.pop('FAKE_MODE', None)
        detail = out[4]
        self.assertNotIn(d, detail, detail)
        self.assertIn('programs/class__copy_await_source.bzl', detail)

    def test_two_out_dirs_produce_byte_identical_reports(self):
        """M4: the whole point — the report must be comparable across runs."""
        def _run(d):
            bezelc = _fake_compiler(d, 'reject')
            os.makedirs(os.path.join(d, 'run'))
            os.makedirs(os.path.join(d, 'programs'))
            try:
                cell = g.run_cell(bezelc, os.path.join(d, 'run'),
                                  os.path.join(d, 'programs'), 'class',
                                  'copy_await_source', 30, 30, {})
            finally:
                os.environ.pop('FAKE_MODE', None)
            rows = [cell]
            p = os.path.join(d, 'report.tsv')
            g.write_report(rows, p)
            with open(p, encoding='utf-8') as fh:
                return fh.read()
        with tempfile.TemporaryDirectory() as a, \
                tempfile.TemporaryDirectory() as b:
            self.assertEqual(_run(a), _run(b))

    # ------------------------------------------------------------ the filter
    def test_the_filter_matches_a_form_name(self):
        """L5: the record documented `--filter copy_local_source` and the
        filter did not match it (exit 2, 'no cells match the filter')."""
        cells = g.all_cells('copy_await_source')
        self.assertTrue(cells)
        self.assertTrue(all(f == 'copy_await_source' for _h, f in cells))
        self.assertTrue(g.all_cells('class'))

    def test_the_cell_count_is_the_documented_one(self):
        self.assertEqual(len(g.all_cells()), 153)


if __name__ == '__main__':
    unittest.main()
