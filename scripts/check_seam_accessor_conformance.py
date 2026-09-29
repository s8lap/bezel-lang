#!/usr/bin/env python3
"""check_seam_accessor_conformance.py - ratchet cross-module test coverage
for the "both-sources accessor" family (#4539).

WHY (umbrella #3152, #4539). check_imported_symbol_reads.py ratchets the
READS of the AST-vs-symbol-table seam - a real dynamic_cast<ast::X*>(sym->decl)
that is unconditionally null for an imported symbol. #4539 named the defect
class that ratchet is BLIND to: a read that is itself correct and necessary
for the local path, consumed by code whose FAILURE branch has no imported-side
fallback. Three such consumers shipped silently in one cycle (#4480, #4508,
#4510) before anyone wrote a cross-module test for the shape; #4510's own
investigation named the precedent that already existed twice over -
`formalOutParamType2` (#2657/#3186/#3188/#3206) and `formalCtorOutParamType`
(#2668/#3772) are BOTH-SOURCES ACCESSORS: they resolve through the AST when it
is there and through the symbol-table side (`ParameterSymbol::resolvedDeclType`)
when it is not.

#4539 itself proposes three candidates for "a way to see the consumers, not
just the reads": (1) detect a consumer that lacks a fallback outright, (2) make
the seam hard to consume unsafely (route every read through a small set of
both-sources accessors), (3) a cross-module conformance obligation: for each
accessor family, a test that the imported path answers identically to the
single-module twin - already demonstrated twice (the #4508/#4510 tests both do
exactly this: identical program text run in one module as its own oracle).

THIS SCRIPT IMPLEMENTS (3), DELIBERATELY NOT (1). (1) was investigated first
(see the #4539 lane's own report): classifying a `dynamic_cast<ast::X*>(...)`
site as "needs a fallback and doesn't have one" versus "provably local-only by
construction" versus "already fixed via a differently-shaped mechanism"
required, in practice, reading each candidate site's whole enclosing function
AND its sibling call sites AND (for one candidate) the wire-metadata format -
exactly the kind of judgement a text-pattern scanner over `src/` cannot
reliably replicate without being either vacuous (misses the real ones, as the
three pre-existing bugs prove) or noisy (the same investigation ruled out ~40
superficially-similar sites as ALREADY-CORRECT or structurally unreachable).
A gate-blocking ratchet on that kind of heuristic trains people to route
around it, which is the exact failure this project's whole ratchet discipline
exists to avoid (see check_vacuous_tests.py's and check_imported_symbol_reads.
py's own "a green from a matcher that matched nothing is worth nothing").
(2) is the durable fix but is a `src/` refactor, out of a scripts-only lane's
charter. (3) is the one that is both CHEAP and MECHANICAL: it does not try to
find new bugs, it keeps the coverage that caught the last three from silently
eroding, and it makes registering a NEW accessor (the moment a #4539-class bug
is fixed) carry an enforced test-coverage obligation rather than a remembered
convention.

WHAT IS CHECKED. `ACCESSORS` below is a small, curated, file-reviewable
registry (same shape as check_imported_symbol_reads.py's SANCTIONED list) -
one entry per known both-sources accessor, naming the ONE question it answers
and the issue(s) whose fix established or extended it. For each entry, at
least one file under tests/module/ (the dedicated cross-module test area -
CLAUDE.md's test-binary split puts every xmod_*.cpp there, and the accessors'
own fixes already live in exactly that directory: xmod_method_out_var_2657_
test.cpp, xmod_ctor_out_var_typing_2668_test.cpp, xmod_imported_hof_lambda_
param_inference_4510_test.cpp, etc.) must cite at least one of the entry's
issue numbers. A registered accessor with ZERO matching tests/module/ coverage
is an ERROR - not "the count changed", an unconditional failure, the same
"zero AST reads is an error" shape check_imported_symbol_reads.py's SANCTIONED
check already uses for the read side.

ADDING AN ENTRY is a visible, reviewable act (exactly like SANCTIONED): when a
#4539-shaped bug is fixed by giving a consumer a both-sources accessor (or by
routing it through an EXISTING one), register the accessor here in the SAME
commit as the fix that lands its first tests/module/ coverage - the register-
and-cover step lands together, so this check can never regress a family that
was covered a moment ago. This is why it is a registry, not a scan: a scan
would have to rediscover "is this function a both-sources accessor" from
scratch on every run, which is exactly the judgement call (1) above declines
to automate.

WHAT THIS DOES NOT DO. It does not find a fourth #4539-class bug before
someone fixes it - see #4589/#4590/#4591/#4592, filed by reading src/ by hand
during this same investigation, which is the honest, un-automatable half of
this problem. It only stops the three we already have from losing their test
coverage silently, and gives the next fix a place to register.

--self-test proves the registry-vs-coverage cross-check fires on a synthetic
tree before trusting a green on the real one.

Usage: scripts/check_seam_accessor_conformance.py [--strict] [--self-test] [--list]
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TESTS_MODULE_DIR = 'tests/module'

# name -> {issues: [int, ...], site: 'file:line', question: str}
#
# `issues` is every issue whose fix established or extended this accessor -
# coverage is satisfied by a tests/module/ file citing ANY one of them, because
# a later extension's test commonly re-exercises the earlier issue's shape in
# the same file rather than duplicating a whole new xmod_*.cpp (measured: the
# #4510 test file's own header cites #2657/#3186/#3772 in prose alongside its
# own #4510 repro).
ACCESSORS = {
    'formalOutParamType2': {
        'issues': [2657, 3186, 3188, 3206],
        'site': 'src/sema/ExprTypecheck_Exprs.cpp:7883-7895',
        'question': "an out-var recipient's formal parameter type, AST or "
                    "symbol side",
    },
    'formalCtorOutParamType': {
        'issues': [2668, 3772],
        'site': 'src/sema/ExprTypecheck_Exprs.cpp:8907-8919',
        'question': "a constructor out-param's formal type, AST or symbol "
                    "side (the `new` path's sibling of formalOutParamType2)",
    },
    'formalFnParamType2': {
        'issues': [4510],
        'site': 'src/sema/ExprTypecheck_Exprs.cpp:7931-7939',
        'question': "a call-argument lambda's parameter type inferred from "
                    "the callee's formal, AST or symbol side (delegates to "
                    "formalOutParamType2 after re-applying the #505 "
                    "generic-callee guard)",
    },
}

ISSUE_RE = re.compile(r'#(\d+)\b')


def find_coverage(root, tests_module_dir=TESTS_MODULE_DIR):
    """{issue_number: [relpath, ...]} for every #NNNN cited anywhere under
    tests/module/*.cpp. A MISSING directory yields an empty map (fail closed
    downstream - every accessor then reports zero coverage), never an error -
    matching every other reader in this repo's "absence makes the check
    stricter, never looser" convention.
    """
    out = {}
    d = root / tests_module_dir
    if not d.is_dir():
        return out
    for p in sorted(d.rglob('*.cpp')):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        rel = str(p.relative_to(root))
        for m in ISSUE_RE.finditer(text):
            out.setdefault(int(m.group(1)), []).append(rel)
    return out


def check(root, accessors=ACCESSORS, tests_module_dir=TESTS_MODULE_DIR):
    """-> (ok, messages, summary). summary = {name: [covering_paths]}."""
    coverage = find_coverage(root, tests_module_dir)
    bad = []
    summary = {}
    for name, meta in sorted(accessors.items()):
        hits = []
        for issue in meta['issues']:
            hits += coverage.get(issue, [])
        # de-dup, keep order stable for --list
        seen = set()
        hits = [h for h in hits if not (h in seen or seen.add(h))]
        summary[name] = hits
        if not hits:
            bad.append(
                '%s (%s) has ZERO tests/module/ coverage citing any of its '
                'issues %s - a both-sources accessor with no cross-module '
                'conformance test cannot prove the imported path still '
                'answers identically to the local twin (#4539). It answers: '
                '%s'
                % (name, meta['site'],
                   '/'.join('#%d' % i for i in meta['issues']),
                   meta['question']))
    return (not bad), bad, summary


def format_summary(summary):
    lines = []
    for name, hits in sorted(summary.items()):
        lines.append('%-24s %d file(s): %s'
                      % (name, len(hits), ', '.join(hits) or '(none)'))
    return '\n'.join(lines)


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _mk(root, rel, text):
    p = pathlib.Path(root) / rel
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

    import tempfile
    with tempfile.TemporaryDirectory() as td:
        registry = {
            'exampleAccessor': {'issues': [111, 222], 'site': 'x.cpp:1',
                                 'question': 'an example both-sources read'},
        }

        # No tests/module/ directory at all - fails closed, not vacuously.
        good, msgs, _ = check(pathlib.Path(td), registry, 'tests/module')
        chk('a missing tests/module/ directory fails (zero coverage)',
            not good)
        chk('the message names the accessor with zero coverage',
            any('exampleAccessor' in m for m in msgs))

        # A tests/module/ file exists but cites neither issue number.
        _mk(td, 'tests/module/xmod_unrelated_test.cpp',
            '// regression for #999, nothing to do with this family\n')
        good, msgs, _ = check(pathlib.Path(td), registry, 'tests/module')
        chk('a tests/module/ dir with only unrelated issues still FAILS',
            not good)

        # Coverage citing the EARLIER of the two issues is sufficient.
        _mk(td, 'tests/module/xmod_example_111_test.cpp',
            '// regression for #111, the earlier extension\n')
        good, msgs, summary = check(pathlib.Path(td), registry, 'tests/module')
        chk('coverage citing EITHER registered issue number PASSES',
            good and not msgs)
        chk('the summary names the covering file',
            summary['exampleAccessor'] ==
            ['tests/module/xmod_example_111_test.cpp'])

        # A file OUTSIDE tests/module/ (e.g. tests/sema/) citing the issue
        # must NOT count - the whole point is CROSS-MODULE coverage, and a
        # same-module test proves nothing about the imported path.
        registry2 = {
            'otherAccessor': {'issues': [333], 'site': 'y.cpp:1',
                               'question': 'another example'},
        }
        _mk(td, 'tests/sema/same_module_333_test.cpp',
            '// regression for #333, but this is NOT a cross-module test\n')
        good, msgs, _ = check(pathlib.Path(td), registry2, 'tests/module')
        chk('a same-module (tests/sema/) citation does NOT satisfy coverage',
            not good)
        chk('the failing accessor is named, not the passing one',
            any('otherAccessor' in m for m in msgs)
            and not any('exampleAccessor' in m for m in msgs))

        # Multiple accessors, mixed coverage: only the uncovered one fails,
        # and both are reported in the summary regardless of verdict.
        registry3 = dict(registry)
        registry3.update(registry2)
        good, msgs, summary = check(pathlib.Path(td), registry3,
                                     'tests/module')
        chk('a mixed registry fails on the ONE uncovered accessor', not good)
        chk('a mixed registry summary reports every accessor',
            set(summary) == {'exampleAccessor', 'otherAccessor'})

        # The real registry against the real tree, if this checkout has one -
        # a light live smoke-test, not a hard assertion (self-test must not
        # depend on repo state), so this block only prints, never fails.
        real_good, real_msgs, real_summary = check(ROOT)
        print('  (informational) live tree: %s'
              % ('OK' if real_good else '%d accessor(s) uncovered'
                 % len(real_msgs)))

    print('check_seam_accessor_conformance --self-test: '
          + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--strict', action='store_true',
                     help='gate mode: any uncovered accessor is a nonzero exit')
    ap.add_argument('--self-test', action='store_true',
                     help='prove the detector still detects')
    ap.add_argument('--list', action='store_true',
                     help='list every registered accessor and its coverage')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    good, msgs, summary = check(ROOT)

    if a.list:
        print(format_summary(summary))
        return 0

    if good:
        print('check_seam_accessor_conformance: OK (%d accessor(s), all '
              'tests/module/-covered)\n%s'
              % (len(summary), format_summary(summary)))
        return 0

    print('check_seam_accessor_conformance: %d of %d accessor(s) have no '
          'cross-module conformance coverage (#4539)\n' % (len(msgs), len(summary)))
    for m in msgs:
        print('  ' + m)
    print('\nRegister a NEW both-sources accessor in ACCESSORS (this file) '
          'in the SAME commit as the fix that gives it its first '
          'tests/module/ test - never as a follow-up.')
    return 1 if a.strict else 0


if __name__ == '__main__':
    sys.exit(main())
