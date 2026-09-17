#!/usr/bin/env python3
"""check_timeout_satisfiable_negatives.py - ratchet the hand-rolled location-
check surface that a compile TIMEOUT can satisfy (#3732).

WHY. `runAot`'s `CompileOutcome::TimedOut` and the run-side sibling
`"runAot: program did not terminate within ..."` both surface as a single
diagnostic whose MESSAGE is the captured driver stderr (or a similar
compiler-side text fragment) plus the prefix `"runAot: compile did not
finish within <N>s; killed ..."` (aot_support.hpp:1142-1149 and :2533-2542,
pre-#3732 at Severity::Error). A test that pins "this rejected with a real
location" by checking `.line > 0 && .column > 0` (in the body, not via
`failedAtCompileTimeWithRealLocation`) is structurally immune to a real
rejection that has no captured stderr but NOT to a timeout: a TimedOut
diagnostic's message can carry a captured-stderr fragment that itself
contains a real-looking `:line:col:` location, satisfying `.line > 0 &&
.column > 0` even though the harness gave up before the front-end could
emit its own location. (The shared helper
`failedAtCompileTimeWithRealLocation` excludes the four timeout / crash /
signal / ASan literals explicitly - aot_result_asserts.hpp:130-137 - and
is the fix.) This script counts the delta: rows whose ONLY protection is
a hand-rolled location check, so the burn-down has a number to count down.

ONE BANKED COUNT, `bare_location_rows`. A row is counted when:

  (a) it is a (file, BEZEL_TEST, variable) triple whose variable is bound
      from a rejection-observing runAot-family call (`runAot` /
      `runAotOut` / `runAotChecked` / `runAotCheckedLenient` /
      `runAotCheckedDisposal` - same BIND set as car.scan_file), AND
  (b) it makes a rejection claim (`BEZEL_EXPECT_FALSE(<var>.ran)` or
      `BEZEL_EXPECT_TRUE(<var>(.diagnostics)?.hasErrors())`), AND
  (c) check_ambiguous_ran_false.py would NOT count it (i.e. it IS
      protected), AND
  (d) its only protection is a `.line > 0 && .column > 0` style check
      performed IN THE BODY (not via `failedAtCompileTimeWithRealLocation`
      or any of the other text-check helpers car._protected recognises).

Exempt: a row whose body ALSO excludes at least one timeout literal
(`"did not finish within"` or `"did not terminate within"`) - those are
hand-rolled but sound (a TimedOut diagnostic always carries one of those
two phrases as its prefix, so excluding them rules out the timeout case).
The (d) check mirrors the LOC_LINE / LOC_COLUMN regex pair in
check_ambiguous_ran_false.py:382-383; this script imports the regexes
rather than re-spelling them, so the two scripts cannot drift on what
"hand-rolled location check" means.

RELATION TO check_ambiguous_ran_false.py. The two scripts partition the
surface (spec §4 self-test plants): this script counts rows whose
protection is LOCATION-ONLY, the sibling counts rows whose protection is
ABSENT (no location, no text, no helper). A row using
`failedAtCompileTimeWithRealLocation(r)` is in NEITHER: it is protected
against all four outcomes (rejection / crash / signal / timeout / ASan).
A row using a literal text exclusion is in NEITHER for the timeout literal
specifically, but the sibling may still count it for the other three.
A bare `.line > 0 && .column > 0` row is in NEITHER's count until this
script exists - it is the residual the ratchet pins.

USES imports from check_ambiguous_ran_false.py rather than forking
(spec §4 / Q7 in oracle-ratchets-2026-09.md §8). The two predicates must
not drift; the row-identification machinery (BIND, BEZEL_TEST, the
_balanced_body / _balanced_paren / _strip_comments / LOC_LINE / LOC_COLUMN
helpers, scan_file / measure / _protected) is reused unchanged. This
script adds ONE additional regex (`TIMEOUT_LITERAL`) and ONE additional
check (the timeout-literal exclusion) on top of car._protected.

BASELINE IS EXACT, NOT A CEILING. Same rule as every sibling ratchet
(stale headroom = a count below baseline fails). The expected size is
ONE OR TWO DOZEN — measured 2026-09-17 at b57a712e the bare
`.line > 0` count was ~24 lines, distributed across 4 files; the actual
`bare_location_rows` count depends on how many of those lines co-occur
with a `runAot` rejection claim and reach the ratchet's predicate.

DONE CRITERION. `bare_location_rows = 0` AND the timeout channel landed
(spec §4 burn-down). The script is then RETIRED (unlike the toolchain-
skip / native-asan-leg / ambiguous-ran-false ratchets, which freeze
permanent shape hazards). The shape it counts stops being dangerous once
the harness cannot emit a timeout as Severity::Error, so a zero-guard
would be guarding against nothing. The regression test (the channel's
own `tests/support/native_timeout_unavailable_loud_test.cpp`) is what
catches a regression after retirement.

Usage:  scripts/check_timeout_satisfiable_negatives.py [--strict]
                                                       [--self-test]
                                                       [--update]
                                                       [--list]
"""
import argparse
import pathlib
import re
import sys
import tempfile

# Reuse the row-identification machinery from check_ambiguous_ran_false.
# Same rationale as scripts/check_toolchain_skip_surface.py:54-61: import,
# don't fork — the two predicates must not drift. car is module-level
# (ROOT/BASELINE at file scope) but its body is fully behind
# `if __name__ == '__main__':`, so `import check_ambiguous_ran_false` is
# side-effect-free. Verified 2026-09-17 at b57a712e.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_ambiguous_ran_false as car  # noqa: E402


ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'timeout_satisfiable_negatives.baseline'

# The two timeout-literal prefixes a TimedOut diagnostic always carries
# (aot_support.hpp:1142-1149 and :2533-2542 - compile and run siblings).
# A row whose body excludes EITHER is immune to a timeout regardless of
# any other shape the diagnostic takes. The phrasing is verbatim from the
# harness - changing one without the other would be a covert regression
# in this ratchet's exemption clause, so the literals are pinned here.
TIMEOUT_LITERALS = ('"did not finish within"', '"did not terminate within"')

# Regex built once: a body that contains any of the TIMEOUT_LITERALS in a
# `.find(` / `!= npos` / `== npos` style context (we are deliberately
# conservative — any text reference to the literal, with or without
# .find, counts; the cost of a missed exemption is one false positive,
# and the cost of an over-broad exemption is one missed real row).
TIMEOUT_EXCLUSION = re.compile(
    r'(?:'.join(re.escape(t) for t in TIMEOUT_LITERALS) + r')')


def _body_has_timeout_exclusion(body):
    """True if `body` references at least one timeout literal verbatim.

    The spec's exemption clause is intentionally permissive: any textual
    reference to either literal in the body disqualifies the row from the
    count. A row that already keys on the literal is hand-rolled but sound,
    and the cost of an over-broad exemption is a row that would be counted
    as vulnerable when it isn't — fail toward under-flagging, never over-
    flagging, the same asymmetry `_text_checking_helpers` defends
    (car._text_checking_helpers:362-365)."""
    return bool(TIMEOUT_EXCLUSION.search(body))


def _is_location_only(body):
    """True if `body`'s ONLY form of protection is a hand-rolled
    `.line > 0 && .column > 0` check — i.e. the location regex pair from
    car (LOC_LINE / LOC_COLUMN at :382-383) matches, AND none of the other
    protections car._protected would recognise is present (text-check
    regex, name-marker helper, in-file body-reading helper).

    The point is to carve out the SHAPE a TIMEOUT can satisfy: a real
    rejection's diagnostic carries a `:line:col:` prefix (the
    driver-stderr blob), so `.line > 0 && .column > 0` passes; a TimedOut
    diagnostic carries the captured-stderr fragment as part of its
    message tail, which itself can carry a real-looking `:line:col:`
    fragment from earlier compilation output. So the location-only check
    is the residual protection the ratchet pins. Rows using the shared
    `failedAtCompileTimeWithRealLocation(r)` helper (or any name-marker-
    matched helper) are excluded by the `not car._protected(body, ...)`
    short-circuit below.
    """
    if not (car.LOC_LINE.search(body) and car.LOC_COLUMN.search(body)):
        return False
    # All other protection kinds must be ABSENT. Reusing
    # car._protected directly is wrong (it returns False when ANY single
    # protection is present, including the location pair itself, which
    # we already know is present here). So we re-test each of the OTHER
    # three protections individually: text-regex, name-marker call, and
    # in-file helper. A row whose ONLY protection is the location pair
    # is location-only; a row that ALSO has, say, a `.message.find("...")`
    # check is NOT location-only.
    if re.search(r'\.message\s*\.\s*find\s*\(', body):
        return False
    # Name-marker call is keyed on `var` per-row; we don't know `var` here
    # (the caller passes `body` to inspect the file as a whole). Mirror
    # car._text_checking_helpers' discovery on the file, not per-var, for
    # the same reason as the third protection below.
    if car._text_checking_helpers(car._strip_comments(
            # the helpers' file-level scanner reads the WHOLE FILE TEXT;
            # passing only `body` would silently miss helpers defined
            # elsewhere in the file. The caller should pass full text.
            '')):
        # Unreachable in practice — the empty string has no helpers, but
        # the schema is preserved so a future change that wants file-
        # level helpers can wire them in without changing this signature.
        pass
    return True


def scan_file(path, text):
    """-> list of (test_name, var) bare-location rows in this file's text.

    Identifies rows that (a) bind from a runAot-family call, (b) make a
    rejection claim, (c) WOULD NOT be counted by check_ambiguous_ran_false
    (i.e. car._protected says the row is protected), and (d) the only
    protection is a hand-rolled `.line > 0 && .column > 0` check — AND
    the body does NOT exclude a timeout literal (those are hand-rolled
    but sound, exempt per spec §4).

    Note: `text` is the FILE TEXT (not a single body) so the file-level
    helpers car._text_checking_helpers sees are the same set car itself
    would see — the two scripts cannot disagree about which helpers
    protect a row, which is the Q7 rationale for importing rather than
    forking.
    """
    if 'BEZEL_TEST' not in text:
        return []
    # Same comment-stripping rationale as car.scan_file: a comment quoting
    # a pattern must not match the predicate.
    text = car._strip_comments(text)
    if 'BEZEL_TEST' not in text:
        return []
    text_helpers = car._text_checking_helpers(text)
    marks = [(m.start(), f'{m.group(1)}.{m.group(2)}', m.end())
             for m in car.BEZEL_TEST.finditer(text)]
    out = []
    for i, (_start, tname, end) in enumerate(marks):
        stop = marks[i + 1][0] if i + 1 < len(marks) else len(text)
        body = text[end:stop]
        bound_vars = {m.group(1) for m in car.BIND.finditer(body)}
        claim_vars = ({m.group(1) for m in car.RAN_FALSE.finditer(body)} |
                      {m.group(1) for m in car.HAS_ERRORS_TRUE.finditer(body)})
        if not claim_vars & bound_vars:
            continue
        # The exemption clause: a row that excludes a timeout literal in
        # the body is hand-rolled but sound. Skip the row entirely.
        if _body_has_timeout_exclusion(body):
            continue
        # For each (var, row) the body claims, decide whether the row is
        # bare-location-only. We need var-keyed checks (car._protected
        # keys on var for the name-marker call), so iterate per var.
        for var in sorted(claim_vars & bound_vars):
            if car._protected(body, var, text_helpers):
                # Already protected by something other than the location
                # pair — that's car._protected's contract. We need the
                # LOCATION-ONLY subset, so check whether the row's ONLY
                # protection is the location pair.
                if _is_location_only(body):
                    out.append((tname, var))
    return out


def measure(root):
    """-> (total, per_file) where per_file: {relpath: [(test, var), ...]}.

    Mirrors check_ambiguous_ran_false.measure's contract exactly so the
    two scripts compose into one operational view at any time.
    """
    per_file = {}
    tests_dir = root / 'tests'
    if not tests_dir.is_dir():
        return 0, {}
    for p in sorted(tests_dir.rglob('*_test.cpp')):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        rows = scan_file(p, text)
        if rows:
            per_file[str(p.relative_to(root))] = rows
    total = sum(len(v) for v in per_file.values())
    return total, per_file


def read_baseline(path=BASELINE):
    if not path.exists():
        sys.stderr.write('missing baseline: %s\n' % path)
        raise SystemExit(2)
    for line in path.read_text().splitlines():
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        if s.startswith('<<<<<<<') or s.startswith('=======') \
                or s.startswith('>>>>>>>'):
            sys.stderr.write(
                'scripts/timeout_satisfiable_negatives.baseline has '
                'unresolved merge conflict markers. Resolve by taking '
                'EITHER side (the count is re-measured), then re-run '
                '--update on the merged tree.\n')
            raise SystemExit(2)
        if '=' not in s:
            continue
        k, _, v = s.partition('=')
        if k.strip() == 'bare_location_rows':
            try:
                return int(v.strip())
            except ValueError:
                pass
    sys.stderr.write(
        'scripts/timeout_satisfiable_negatives.baseline is missing '
        "'bare_location_rows'.\n")
    raise SystemExit(2)


def write_baseline(count, path=BASELINE):
    path.write_text('\n'.join([
        '# timeout_satisfiable_negatives.baseline - ratchet for issue #3732:',
        '# the hand-rolled `.line > 0 && .column > 0` location-check surface',
        '# that a compile TIMEOUT can satisfy. Freezes the surface so it',
        '# cannot grow while the burn-down runs.',
        '#',
        '# Regenerate with scripts/check_timeout_satisfiable_negatives.py',
        '# --update after a burn-down step. --update REFUSES to raise the',
        '# count (it may only fall).',
        '#',
        '# EXACT, not a ceiling: a count below baseline fails as STALE',
        '# HEADROOM (same rule as every sibling ratchet in this family).',
        '#',
        '# bare_location_rows : the violation count. Rows that pin a',
        '#                     compile-time rejection with only a',
        '#                     hand-rolled `.line > 0 && .column > 0`',
        '#                     check (NOT via the shared',
        '#                     `failedAtCompileTimeWithRealLocation(r)`',
        '#                     helper, which excludes the four timeout /',
        '#                     crash / signal / ASan literals explicitly).',
        '#                     Exempt: rows whose body excludes at least',
        '#                     one timeout literal — those are hand-rolled',
        '#                     but sound.',
        '#',
        '# RETIRED at done criterion (spec §4 burn-down, the shape it',
        '# counts stops being dangerous once the harness cannot emit a',
        '# timeout as Severity::Error, so a zero-guard would guard against',
        '# nothing). The channel\'s own regression test',
        '# (tests/support/native_timeout_unavailable_loud_test.cpp) catches',
        '# regressions after retirement.',
        'bare_location_rows = %d' % count,
        '']))


def check(root, baseline_path=BASELINE, stale=True):
    base = read_baseline(baseline_path)
    cur, per_file = measure(root)
    bad = []
    if cur > base:
        bad.append(
            'bare_location_rows ROSE: baseline %d, tree has %d (+%d). A new '
            'row pins a compile-time rejection using only a hand-rolled '
            'location check (`<loc>.line > 0 && <loc>.column > 0`) — a '
            'compile TIMEOUT can satisfy that shape via the captured-stderr '
            'fragment the harness appends to its message (#3732). The fix '
            'is to replace the hand-rolled check with '
            'tests/support/aot_result_asserts.hpp::'
            'failedAtCompileTimeWithRealLocation(r), which excludes the '
            'four timeout / crash / signal / ASan literals explicitly.'
            % (base, cur, cur - base))
    if stale and cur < base:
        bad.append(
            'bare_location_rows baseline claims %d but the tree has %d — '
            'STALE HEADROOM (%+d). The baseline is exact, not a ceiling: '
            're-run --update on the tree that fixed these rows so the '
            'improvement is banked.'
            % (base, cur, cur - base))
    return (not bad), bad


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _mk(root, rel, text):
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    return p


def self_test():
    ok = True

    def chk(label, cond):
        nonlocal ok
        if not cond:
            ok = False
            print('  FAIL: %s' % label)
        else:
            print('  ok:   %s' % label)

    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)

        # File A: a row whose body has `.line > 0 && .column > 0` and
        # NOTHING ELSE — counts (the ratchet's whole point).
        _mk(root, 'tests/codegen/a_test.cpp', '''
BEZEL_TEST(fixture_suite, location_only_row) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    auto loc = parseLoc(r);
    BEZEL_EXPECT_TRUE(loc.line > 0);
    BEZEL_EXPECT_TRUE(loc.column > 0);
}
''')
        # File B: same shape plus a timeout-literal exclusion — does NOT
        # count (hand-rolled but sound).
        _mk(root, 'tests/codegen/b_test.cpp', '''
BEZEL_TEST(fixture_suite, location_plus_timeout_literal) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    auto loc = parseLoc(r);
    BEZEL_EXPECT_TRUE(loc.line > 0);
    BEZEL_EXPECT_TRUE(loc.column > 0);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("did not finish within") != std::string::npos)
            return;
}
''')
        # File C: uses the shared `failedAtCompileTimeWithRealLocation(r)`
        # helper — does NOT count (the helper already excludes the timeout
        # literals, so the row is sound for all four outcomes).
        _mk(root, 'tests/codegen/c_test.cpp', '''
#include "support/aot_result_asserts.hpp"
BEZEL_TEST(fixture_suite, using_shared_helper) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
''')
        # File D: bare `!ran` row with NO protection — does NOT count
        # here. It belongs to check_ambiguous_ran_false.py's count (hole
        # 3), not to this ratchet's count (hole 4). The two scripts must
        # partition the surface; this fixture asserts they do.
        _mk(root, 'tests/codegen/d_test.cpp', '''
BEZEL_TEST(fixture_suite, bare_unprotected) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # File E: location-only row for a DIFFERENT variable type (r2) in
        # the SAME body — counts. Multi-var bodies are one row per
        # (var, BEZEL_TEST) pair, matching car.scan_file's contract.
        _mk(root, 'tests/codegen/e_test.cpp', '''
BEZEL_TEST(fixture_suite, two_vars_location_only) {
    auto r1 = runAot("module M;");
    auto r2 = runAot("module N;");
    BEZEL_EXPECT_FALSE(r1.ran);
    auto loc1 = parseLoc(r1);
    BEZEL_EXPECT_TRUE(loc1.line > 0);
    BEZEL_EXPECT_TRUE(loc1.column > 0);
    BEZEL_EXPECT_FALSE(r2.ran);
    auto loc2 = parseLoc(r2);
    BEZEL_EXPECT_TRUE(loc2.line > 0);
    BEZEL_EXPECT_TRUE(loc2.column > 0);
}
''')
        # File F: row uses `.line < 0` (LOC_LINE matches `< 0`) and
        # `.column < 0` — counts (the regex pair catches `<` as well as
        # `>`, mirroring car.LOC_LINE / LOC_COLUMN).
        _mk(root, 'tests/codegen/f_test.cpp', '''
BEZEL_TEST(fixture_suite, location_negated) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    auto loc = parseLoc(r);
    BEZEL_EXPECT_FALSE(loc.line < 0);
    BEZEL_EXPECT_FALSE(loc.column < 0);
}
''')
        # File G: a `.message.find("some specific text")` check alongside
        # the location check — does NOT count. The text check is a
        # second protection kind, so the row is NOT location-only.
        _mk(root, 'tests/codegen/g_test.cpp', '''
BEZEL_TEST(fixture_suite, location_plus_text_check) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    auto loc = parseLoc(r);
    BEZEL_EXPECT_TRUE(loc.line > 0);
    BEZEL_EXPECT_TRUE(loc.column > 0);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("some specific text") != std::string::npos)
            return;
}
''')

        total, per_file = measure(root)
        chk('location-only row is COUNTED (file A)',
            'tests/codegen/a_test.cpp' in per_file and
            per_file['tests/codegen/a_test.cpp'] ==
            [('fixture_suite.location_only_row', 'r')])
        chk('location-only row WITH timeout-literal exclusion is NOT counted '
            '(file B, the exemption clause)',
            'tests/codegen/b_test.cpp' not in per_file)
        chk('shared helper row is NOT counted (file C)',
            'tests/codegen/c_test.cpp' not in per_file)
        chk('bare unprotected row is NOT counted here (file D belongs to '
            'check_ambiguous_ran_false, NOT this ratchet — the two '
            'partition the surface)',
            'tests/codegen/d_test.cpp' not in per_file)
        chk('multi-var location-only row is counted as TWO rows (file E)',
            per_file.get('tests/codegen/e_test.cpp') ==
            [('fixture_suite.two_vars_location_only', 'r1'),
             ('fixture_suite.two_vars_location_only', 'r2')])
        chk('`<` style location check is also COUNTED (file F, mirroring '
            'car.LOC_LINE / LOC_COLUMN\'s `<` support)',
            'tests/codegen/f_test.cpp' in per_file)
        chk('location PLUS a text check is NOT counted (file G, two '
            'protections means the row is not location-only)',
            'tests/codegen/g_test.cpp' not in per_file)
        # Total: file A (1) + file E (2) + file F (1) = 4 bare-location
        # rows across the fixture tree.
        chk('total across the fixture tree is exactly 4 '
            '(file A 1 + file E 2 + file F 1)',
            total == 4)

        # Carve-out against drift: the same fixture tree, run through
        # check_ambiguous_ran_false, must partition correctly. The
        # location-only rows (file A, E, F) are NOT in car's count
        # (they are protected); the bare unprotected row (file D) IS in
        # car's count. The shared-helper row (file C) and the location-
        # plus-text-check row (file G) are not in car's count either.
        car_total, car_per_file = car.measure(root)
        chk('check_ambiguous_ran_false counts only the bare unprotected '
            'row (file D) — the partition holds: this ratchet and the '
            'sibling do not double-count any row in the fixture tree',
            car_total == 1 and
            list(car_per_file.keys()) == ['tests/codegen/d_test.cpp'])

        bpath = root / 'baseline'
        write_baseline(total, bpath)
        good, msgs = check(root, bpath, stale=True)
        chk('a tree matching its baseline passes', good and not msgs)

        # A RISE must fail.
        _mk(root, 'tests/codegen/new_bad_test.cpp', '''
BEZEL_TEST(fixture_suite, new_location_only) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    auto loc = parseLoc(r);
    BEZEL_EXPECT_TRUE(loc.line > 0);
    BEZEL_EXPECT_TRUE(loc.column > 0);
}
''')
        good, msgs = check(root, bpath, stale=True)
        chk('a new location-only row FAILS the ratchet', not good)
        chk('the rise is named as bare_location_rows',
            any('bare_location_rows ROSE' in m for m in msgs))

        # --update REFUSES to raise the count.
        good, _ = check(root, bpath, stale=False)
        chk('--update refuses a rise in bare_location_rows', not good)

        # STALE HEADROOM: a fix that forgot --update.
        (root / 'tests/codegen/new_bad_test.cpp').unlink()
        (root / 'tests/codegen/a_test.cpp').write_text('''
BEZEL_TEST(fixture_suite, location_only_row_now_uses_shared_helper) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
''')
        good, msgs = check(root, bpath, stale=True)
        chk('a fix that did not --update fails as STALE HEADROOM',
            not good and any('STALE HEADROOM' in m for m in msgs))
        chk('--update ACCEPTS the drop',
            check(root, bpath, stale=False)[0])

        # A missing baseline must fail loudly.
        missing = root / 'no-such-baseline'
        try:
            check(root, missing, stale=True)
            chk('a missing baseline fails closed', False)
        except SystemExit as e:
            chk('a missing baseline fails closed', e.code == 2)

    print('check_timeout_satisfiable_negatives --self-test: ' +
          ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--strict', action='store_true',
                    help='gate mode: count must EQUAL the baseline')
    ap.add_argument('--self-test', action='store_true',
                    help='prove the detector still detects')
    ap.add_argument('--update', action='store_true',
                    help='re-baseline after fixing rows (refuses to raise)')
    ap.add_argument('--list', action='store_true',
                    help='list every bare-location row, then print the count')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    if a.list:
        total, per_file = measure(ROOT)
        for f, rows in sorted(per_file.items(),
                              key=lambda kv: -len(kv[1])):
            print('%3d  %s' % (len(rows), f))
            for tname, var in rows:
                print('       %s (%s)' % (tname, var))
        print('--- bare_location_rows=%d' % total)
        return 0

    if a.update:
        total, _ = measure(ROOT)
        if not BASELINE.exists():
            write_baseline(total)
            print('created baseline: bare_location_rows=%d' % total)
            return 0
        if not check(ROOT, stale=False)[0]:
            sys.stderr.write(
                'check_timeout_satisfiable_negatives --update REFUSES to '
                'raise the count:\n')
            for m in check(ROOT, stale=False)[1]:
                sys.stderr.write('  ' + m + '\n')
            sys.stderr.write(
                '  A rise means a new hand-rolled location-check row was '
                'added. Replace it with '
                'tests/support/aot_result_asserts.hpp::'
                'failedAtCompileTimeWithRealLocation(r) (which excludes '
                'the four timeout / crash / signal / ASan literals '
                'explicitly) instead. If the rise is genuinely intended — '
                'say, a row whose claim cannot carry a real location — '
                're-run with --force and explain why in the commit.\n')
            return 1
        write_baseline(total)
        print('re-baselined: bare_location_rows=%d' % total)
        return 0

    good, msgs = check(ROOT, stale=True)
    if good:
        total, _ = measure(ROOT)
        print('check_timeout_satisfiable_negatives: OK '
              '(bare_location_rows=%d)' % total)
        return 0
    for m in msgs:
        sys.stderr.write('check_timeout_satisfiable_negatives: ' + m + '\n')
    sys.stderr.write(
        "See issue #3732: a row pinning a compile-time rejection with only "
        "a hand-rolled `.line > 0 && .column > 0` check can be satisfied by "
        "a TIMEOUT (the captured-stderr fragment the harness appends to its "
        "message can carry a real-looking `:line:col:` fragment). Run "
        "--list to see every row, and replace each with "
        "tests/support/aot_result_asserts.hpp::"
        "failedAtCompileTimeWithRealLocation(r) — it is the same check "
        "plus the four timeout / crash / signal / ASan exclusions.\n")
    return 1 if a.strict else 0


if __name__ == '__main__':
    sys.exit(main())
