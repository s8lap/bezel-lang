#!/usr/bin/env python3
"""check_vacuous_tests.py - find gtest assertions that CANNOT FAIL.

WHY (issue #3138). `runAot` populates `JitResult::diagnostics` ONLY on a
compile/link reject, a compiler crash/timeout, a fired sanitizer report, or a
signal (tests/support/aot_support.hpp) - and EVERY one of those paths `return`s
BEFORE `r.ran = WIFEXITED(status)`. So `r.ran == true` implies `r.diagnostics`
is EMPTY, and a **Warning** emitted on a program that COMPILES SUCCESSFULLY
never reaches it. A body of the shape

    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);              // the program compiles and runs
    BEZEL_EXPECT_FALSE(hasSomeWarning(r)); // scans r.diagnostics -> always empty

has a second assertion that is a TAUTOLOGY given the first. Measured instances
when this was written: all 7 tests in
tests/codegen/using_var_autodrop_chain_guard_2992_test.cpp (the entire #2992
regression file, which additionally carried a HOW-TO-VERIFY-FAILS-WITHOUT recipe
that could not reproduce). The limitation was ALREADY documented in-tree at
tests/codegen/call_lowered_as_unit_vs_declined_2963_test.cpp - so this is a known
rule that was not applied, which is the shape that recurs until it is enforced
mechanically rather than remembered.

WHAT IS *NOT* FLAGGED - and why the exemption is DERIVED, not special-cased.
The tautology only *misleads* when the assertion claims coverage that
`EXPECT_TRUE(r.ran)` does not already give. That splits the shapes cleanly:

  CLASS A (silent). The predicate is REJECT-ONLY: it can never answer `true`
  for a program that ran, independently of the harness's diagnostics behaviour.
  Established by either
     * a `Severity::Error` filter on every match (an Error and a successful
       compile are mutually exclusive by definition), or
     * an explicit `if (<param>.ran) return false;` early-out, or
     * the degenerate direct form `<var>.diagnostics.hasErrors()`.
  Such an assertion is belt-and-braces on the reject path: redundant, but its
  claim is TRUE and the test's real oracle is elsewhere (a value, an exit code).
  Note the third bullet is not a hand-written exception - it is what the first
  bullet reduces to, which is the sign the rule is the right one.

  CLASS B (REPORTED). The predicate can match a **Warning / Note / `internal:`**
  diagnostic - one that CAN coexist with a successful compile. The intent is
  therefore unsatisfiable through this harness: the note it hunts for can never
  be present, so the test claims coverage it does not have. This is #3138's
  shape, and it is what `--strict` fails on.

HOW HELPERS ARE CLASSIFIED. By their BODY, not their name. The first draft
matched helper names against `has|contains|found|includes`, which silently
missed `diagSaysEncodingOnlyDirect(r)` - the identical shape under a different
name. This version parses every in-file function taking a `JitResult` whose body
reads `.diagnostics`, and asks whether that body is reject-only.

REMEDY when Class B fires: move the oracle to the lit track (real bezelc +
FileCheck over diagnostic output, where a Warning on a compiling program IS
observable - see tests/lit/*_no_*_note_*.bzl for the established
`FileCheck --check-prefix=NODIAG --allow-empty` + `CHECK-NOT` form), or assert on
something the harness actually carries (exit code, stdout).

Usage:  scripts/check_vacuous_tests.py [--paths tests] [--strict] [--show-benign]
        --strict makes any Class B finding exit 1 (this is a gate phase,
        scripts/gate_full_cycle.sh); default is advisory.
        --show-benign additionally lists the Class A redundancies.
        --self-test runs the built-in known-positive/known-negative fixtures and
        exits non-zero if the detector does not classify them correctly.
"""
import argparse, pathlib, re, sys, tempfile

# A result binding from a harness call that only fills diagnostics on a reject.
BIND = re.compile(r'\b(?:auto|JitResult|AotResult|bezel::test::JitResult)\s+(\w+)\s*=\s*'
                  r'(runAot|runAotOut|runAotChecked|runAotCheckedLenient|'
                  r'runAotCheckedDisposal)\s*\(')
# NOTE: the macro is frequently WRAPPED across two lines --
#   BEZEL_TEST(suite_name,\n             case_name) {
# A single-line ^BEZEL_TEST regex silently matches ZERO tests in such a file, so
# the detector reported it clean. That is how the first draft of this script
# missed the very file (#2992) that motivated it. Split on the macro token and
# parse the two identifiers across the newline.
BEZEL_TEST = re.compile(r'BEZEL_TEST\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', re.S)

# An in-file predicate over a JitResult: `bool NAME(<...JitResult...>) {`.
HELPER = re.compile(r'\b(?:static\s+|inline\s+)*bool\s+(\w+)\s*\(([^;{)]*)\)\s*\{')


def _balanced_body(text, open_brace_idx):
    """Return the text between the brace at open_brace_idx and its match."""
    depth = 0
    for j in range(open_brace_idx, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[open_brace_idx + 1:j]
    return text[open_brace_idx + 1:]


def diagnostic_helpers(text):
    """{name: is_reject_only} for every in-file bool(JitResult) that reads
    .diagnostics.  is_reject_only == True means CLASS A."""
    out = {}
    for m in HELPER.finditer(text):
        params = m.group(2)
        if 'JitResult' not in params:
            continue
        body = _balanced_body(text, m.end() - 1)
        if '.diagnostics' not in body:
            continue
        # The JitResult parameter's own name, so the `.ran` early-out is matched
        # on the right variable rather than on any identifier.
        pm = re.search(r'JitResult\s*&?\s*(\w+)', params)
        pname = pm.group(1) if pm else r'\w+'
        # (i) every match is gated on Severity::Error -> cannot fire when ran.
        sev = re.findall(r'severity\s*==\s*(?:::)?(?:bezel::)?Severity::(\w+)', body)
        error_only = bool(sev) and all(s == 'Error' for s in sev)
        # (ii) an explicit `if (p.ran) return false;` early-out.
        ran_guard = re.search(
            r'if\s*\(\s*' + re.escape(pname) + r'\s*\.\s*ran\s*\)\s*return\s+false\s*;',
            body) is not None
        out[m.group(1)] = (error_only or ran_guard)
    return out


def ran_true(text, var):
    return re.search(r'BEZEL_EXPECT_TRUE\s*\(\s*' + re.escape(var) + r'\s*\.\s*ran\s*\)',
                     text) is not None


def classify(text, var, helpers):
    """Yield (arg_text, 'A'|'B', why) for each EXPECT_FALSE reading var's
    diagnostics."""
    for m in re.finditer(r'BEZEL_EXPECT_FALSE\s*\(([^;]*)\)\s*;', text, re.S):
        arg = ' '.join(m.group(1).split())
        # --- direct read: `var.diagnostics...`
        if re.search(re.escape(var) + r'\s*\.\s*diagnostics', arg):
            if re.search(re.escape(var) + r'\s*\.\s*diagnostics\s*\.\s*hasErrors\s*\(\s*\)', arg):
                yield arg, 'A', 'hasErrors() is Error-only'
            else:
                yield arg, 'B', 'reads ' + var + '.diagnostics directly'
            continue
        # --- through an in-file predicate
        for name, reject_only in helpers.items():
            if re.search(r'\b' + re.escape(name) + r'\s*\(\s*' + re.escape(var) + r'\s*[,)]', arg):
                yield arg, ('A' if reject_only else 'B'), (
                    name + (' is reject-only' if reject_only
                            else ' can match a Warning/Note'))
                break


def scan(paths):
    """-> (class_B_findings, class_A_findings), each a list of
    (path, test, var, arg, why)."""
    bad, benign, seen = [], [], set()
    for root in paths:
        for f in sorted(pathlib.Path(root).rglob('*_test.cpp')):
            text = f.read_text(encoding='utf-8', errors='replace')
            if 'BEZEL_EXPECT_FALSE' not in text:
                continue
            helpers = diagnostic_helpers(text)
            marks = [(m.start(), f'{m.group(1)}.{m.group(2)}', m.end())
                     for m in BEZEL_TEST.finditer(text)]
            for i, (start, tname, end) in enumerate(marks):
                stop = marks[i + 1][0] if i + 1 < len(marks) else len(text)
                body = text[end:stop]
                for bm in BIND.finditer(body):
                    var = bm.group(1)
                    if not ran_true(body, var):
                        continue          # not asserted to run -> reject path, fine
                    for arg, cls, why in classify(body, var, helpers):
                        key = (str(f), tname, var, arg)
                        if key in seen:
                            continue
                        seen.add(key)
                        (bad if cls == 'B' else benign).append(key + (why,))
    return bad, benign


# --- self-test ------------------------------------------------------------
# The brief's warning, made mechanical: the FIRST draft of this detector reported
# the whole tree clean because `^BEZEL_TEST` matched zero wrapped macros, and it
# was believed until it was control-run against a known positive. These fixtures
# are that control run, so "the detector is green" can never again mean "the
# detector matched nothing".
_FIXTURES = [
    # (filename, source, expected class or None)
    ('positive_wrapped_macro_test.cpp', '''
#include "support/aot_support.hpp"
namespace {
bool hasNote(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("internal:") != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite,
           wrapped_macro_case) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasNote(r));
}
''', 'B'),
    ('positive_direct_read_test.cpp', '''
BEZEL_TEST(fixture_suite, direct_read) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.diagnostics().empty());
}
''', 'B'),
    ('benign_error_only_test.cpp', '''
namespace {
bool hasErrorText(const bezel::test::JitResult& r, const char* n) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(n) != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, error_only_is_benign) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasErrorText(r, "x"));
}
''', 'A'),
    ('benign_ran_guard_test.cpp', '''
namespace {
bool rejectedBy(const bezel::test::JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("nope") != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, ran_guard_is_benign) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r));
}
''', 'A'),
    ('negative_reject_path_test.cpp', '''
namespace {
bool hasNote(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("internal:") != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, reject_path_is_fine) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(hasNote(r));
}
''', None),
]


def self_test():
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for name, src, _ in _FIXTURES:
            (pathlib.Path(td) / name).write_text(src)
        bad, benign = scan([td])
    got = {}
    for path, tname, var, arg, why in bad:
        got[pathlib.Path(path).name] = 'B'
    for path, tname, var, arg, why in benign:
        got.setdefault(pathlib.Path(path).name, 'A')
    for name, _, want in _FIXTURES:
        have = got.get(name)
        mark = 'ok  ' if have == want else 'FAIL'
        if have != want:
            ok = False
        print(f'  [{mark}] {name}: expected {want!r}, got {have!r}')
    print('check_vacuous_tests --self-test: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--paths', nargs='*', default=['tests'])
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--show-benign', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    bad, benign = scan(a.paths)

    if a.show_benign and benign:
        print(f'check_vacuous_tests: {len(benign)} benign (class A) redundancy(ies) '
              f'- implied by EXPECT_TRUE(.ran), but the claim is true\n')
        for path, tname, var, arg, why in benign:
            print(f'  {path}\n      {tname}: EXPECT_FALSE({arg})  [{why}]')
        print()

    if not bad:
        print('check_vacuous_tests: clean - no assert-absence-of-a-NON-ERROR-'
              'diagnostic-on-a-running-program cases found'
              + (f' ({len(benign)} benign class-A redundancies not reported)'
                 if benign and not a.show_benign else ''))
        return 0

    print(f'check_vacuous_tests: {len(bad)} VACUOUS assertion(s) - these cannot '
          f'fail (see #3138)\n')
    for path, tname, var, arg, why in bad:
        print(f'  {path}\n      {tname}')
        print(f'      asserts {var}.ran is TRUE, then EXPECT_FALSE({arg})')
        print(f'      -> {why}; {var}.diagnostics is empty by construction for a '
              f'program that compiles\n')
    print('Remedy: move the oracle to the lit track (tests/lit/*.bzl, '
          'FileCheck --check-prefix=NODIAG --allow-empty), or assert on exit '
          'code / stdout.')
    return 1 if a.strict else 0


if __name__ == '__main__':
    sys.exit(main())
