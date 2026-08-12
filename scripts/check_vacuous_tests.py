#!/usr/bin/env python3
"""check_vacuous_tests.py - find gtest cases that CANNOT FAIL.

WHY (issue #3138). `runAot` populates `JitResult::diagnostics` ONLY on a
compile/link reject, a timeout, an ASan report, or a signal
(tests/support/aot_support.hpp). A **Warning** emitted on a program that
COMPILES SUCCESSFULLY never reaches `r.diagnostics`. So a body of the shape

    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);              // the program compiles and runs
    BEZEL_EXPECT_FALSE(hasSomeNote(r));    // scans r.diagnostics -> always empty

has a second assertion that is a TAUTOLOGY given the first. Measured instances
when this was written: all 7 tests in
tests/codegen/using_var_autodrop_chain_guard_2992_test.cpp (the entire #2992
regression file, which additionally carries a HOW-TO-VERIFY-FAILS-WITHOUT recipe
that cannot reproduce), plus one in tests/codegen/box_unit_2915_test.cpp. The
limitation was ALREADY documented in-tree at
tests/codegen/call_lowered_as_unit_vs_declined_2963_test.cpp:23-31 - so this is a
known rule that was not applied, which is the shape that recurs until it is
enforced mechanically rather than remembered.

WHAT IS *NOT* FLAGGED. Asserting diagnostics on a program EXPECTED TO BE REJECTED
is correct and common - a reject is exactly when runAot populates the field. The
detector only fires when a test asserts the program RAN and then asserts the
ABSENCE of a diagnostic from that same result.

REMEDY when it fires: move the oracle to the lit track (real bezelc + FileCheck
over diagnostic output, where a Warning on a compiling program IS observable), or
assert on something the harness actually carries (exit code, stdout).

Usage:  scripts/check_vacuous_tests.py [--paths tests] [--strict]
        --strict makes any finding exit 1 (for a gate phase); default is advisory.
"""
import argparse, pathlib, re, sys

# A result binding from a harness call that only fills diagnostics on a reject.
BIND = re.compile(r'\b(?:auto|JitResult|AotResult)\s+(\w+)\s*=\s*'
                  r'(runAot|runAotOut|runAotChecked|runAotCheckedLenient)\s*\(')
# NOTE: the macro is frequently WRAPPED across two lines --
#   BEZEL_TEST(suite_name,\n             case_name) {
# A single-line ^BEZEL_TEST regex silently matches ZERO tests in such a file, so
# the detector reported it clean. That is how the first draft of this script
# missed the very file (#2992) that motivated it. Split on the macro token and
# parse the two identifiers across the newline.
BEZEL_TEST = re.compile(r'BEZEL_TEST\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', re.S)

def ran_true(text, var):
    return re.search(r'BEZEL_EXPECT_TRUE\s*\(\s*' + re.escape(var) + r'\s*\.\s*ran\s*\)', text)

def absence_of_diag(text, var):
    """EXPECT_FALSE over something that reads <var>.diagnostics, directly or via a helper."""
    hits = []
    for m in re.finditer(r'BEZEL_EXPECT_FALSE\s*\(([^;]*)\)\s*;', text, re.S):
        arg = m.group(1).strip()
        # CLASS A (harmless, not reported): `EXPECT_FALSE(r.diagnostics.hasErrors())`
        # right after `EXPECT_TRUE(r.ran)`. Redundant belt-and-braces on the reject
        # path -- the test's real oracle is elsewhere (a value, an exit code). It
        # asserts nothing new but it also claims nothing false.
        if re.search(re.escape(var) + r'\s*\.\s*diagnostics\s*\.\s*hasErrors\s*\(\s*\)', arg):
            continue
        # CLASS B (REPORTED): the test scans the diagnostics for a specific NOTE or
        # WARNING on a program it asserts RAN. That intent is unsatisfiable -- the
        # note it is hunting for can never be present -- so the test claims coverage
        # it does not have. This is #3138's shape.
        if re.search(re.escape(var) + r'\s*\.\s*diagnostics', arg):
            hits.append(('scans ' + arg[:64], 'B'))
        elif re.search(r'\b(has|contains|found|includes)\w*\s*\(\s*' + re.escape(var) + r'\s*[,)]', arg):
            hits.append((arg[:64], 'B'))
    return hits

def split_tests(text):
    """Yield (qualified_name, body) per BEZEL_TEST, macro-wrapping tolerant."""
    out = []
    marks = [(m.start(), f'{m.group(1)}.{m.group(2)}', m.end())
             for m in BEZEL_TEST.finditer(text)]
    for i, (start, name, end) in enumerate(marks):
        stop = marks[i + 1][0] if i + 1 < len(marks) else len(text)
        out.append((name, text[end:stop]))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--paths', nargs='*', default=['tests'])
    ap.add_argument('--strict', action='store_true')
    a = ap.parse_args()

    findings, seen = [], set()
    for root in a.paths:
        for f in sorted(pathlib.Path(root).rglob('*_test.cpp')):
            text = f.read_text(encoding='utf-8', errors='replace')
            if 'BEZEL_EXPECT_FALSE' not in text:
                continue
            for tname, body in split_tests(text):
                for bm in BIND.finditer(body):
                    var = bm.group(1)
                    if not ran_true(body, var):
                        continue          # not asserted to run -> reject path, fine
                    for arg, cls in absence_of_diag(body, var):
                        key = (str(f), tname, var, arg)
                        if key not in seen:
                            seen.add(key)
                            findings.append(key)

    if not findings:
        print('check_vacuous_tests: clean - no assert-absence-of-diagnostic-on-a-'
              'running-program cases found')
        return 0

    print(f'check_vacuous_tests: {len(findings)} VACUOUS assertion(s) - these '
          f'cannot fail (see #3138)\n')
    for path, tname, var, arg in findings:
        print(f'  {path}\n      {tname}')
        print(f'      asserts {var}.ran is TRUE, then EXPECT_FALSE({arg})')
        print(f'      -> {var}.diagnostics is empty by construction for a program '
              f'that compiles\n')
    print('Remedy: move the oracle to the lit track, or assert on exit code / stdout.')
    return 1 if a.strict else 0

if __name__ == '__main__':
    sys.exit(main())
