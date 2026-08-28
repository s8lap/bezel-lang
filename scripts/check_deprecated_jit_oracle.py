#!/usr/bin/env python3
"""check_deprecated_jit_oracle.py - ratchet the deprecated JIT test oracle.

WHY. `runJit` / `runJitChecked` are the LENIENT test oracle: they skip the
ownership and definite-assignment passes and they skip `verifyModule`. A test
written on them proves strictly less than its name suggests - it can pass over
IR that the real AOT path would reject outright. CLAUDE.md's rule is that new
tests go on `runAot` only, that a JIT test is ported whenever it is touched,
and that THE COUNT MAY ONLY EVER GO DOWN.

WHAT THIS SCRIPT IS FOR - and it is not the counting.

The rule above already existed, written down, with the measured numbers beside
it. It still failed. On 2026-08-23 `fix #3345` added a `runJitOnly()` helper
that was never called, taking the count 60 -> 61, and that rise survived FIVE
DAYS and at least two full gate cycles before a coordinator re-measured by
hand and filed #3439. Nothing had gone wrong with anyone's diligence: the
count simply lived in PROSE, and prose is not executable.

Its sibling ratchets are scripts wired into `gate_full_cycle.sh` with their own
`--self-test`. This one was a paragraph. THAT is the defect this script fixes;
the counting is incidental. A ratchet a human has to remember to run is a
convention, and conventions are what this file exists to stop relying on.

THREE COUNTS, AND WHY THE CONTROL EARNS ITS KEEP.

  live_calls  - `runJit(` in tests/**/*.cpp, EXCLUDING tests/support/.
                The number that matters: call sites in real tests.
  bare_grep   - `runJit(` anywhere under tests/, no filters at all.
                Always larger, because it also counts the harness's own
                definitions under tests/support/.
  checked     - `runJitChecked(` under the same filters as live_calls.

`bare_grep` is not redundant. CLAUDE.md records that a recipe which does not
reproduce the number beside it "reads as a violated ratchet; it cost one false
alarm already". The two numbers together tell you WHICH kind of change you are
looking at:

    both moved by the same amount  -> a real call site was added or removed
    only live_calls moved          -> a file moved into or out of tests/support/
    only bare_grep moved           -> a change inside the harness itself

The first is a ratchet violation. The second and third are not, and without the
control you cannot tell them apart - you can only re-derive the distinction by
hand, at the exact moment you are least inclined to trust the tooling. That is
why the control is measured and ratcheted rather than left in a comment.

THE BASELINE IS EXACT, NOT A CEILING. A count BELOW its baseline fails too, as
STALE HEADROOM. This is deliberate and it is the same rule the enumerable-grid
ratchet uses: headroom is indistinguishable from a rise that someone already
spent, so a port that forgets to `--update` silently buys room for the next
regression. Porting a test is a two-line change; re-baselining is one command.

WHEN A TEST IS PORTED. The count goes down; re-baseline with `--update`, which
REFUSES to raise a count. Landing a rise therefore takes a deliberate hand-edit
of the baseline file, which is exactly the friction a ratchet is for.

⚠ CONTROL-RUN BEFORE TRUSTING A GREEN. `--self-test` runs FIRST in the gate,
because a detector that has silently stopped detecting reports the same green
as a clean tree. It builds synthetic trees whose answers are known and asserts
the counts, the ratchet's fail-closed directions, and `--update`'s refusal to
raise.

Usage:  scripts/check_deprecated_jit_oracle.py [--strict] [--self-test]
                                               [--update [--force]] [--list]
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'deprecated_jit_oracle.baseline'

# `runJit(` / `runJitChecked(` as a whole call token. `runJitChecked(` must NOT
# be counted as a `runJit(` hit, so the live-call pattern requires the `(` to
# follow `runJit` immediately.
RE_RUNJIT = re.compile(r'\brunJit\(')
RE_RUNJIT_CHECKED = re.compile(r'\brunJitChecked\(')
# The bare control counts BOTH spellings, matching `grep -rn 'runJit(' tests/`,
# which is a substring search and so also matches inside `runJitChecked(`?  No:
# `runJitChecked(` does not contain the substring `runJit(`.  The control
# differs from live_calls ONLY by dropping the two filters.
RE_BARE = re.compile(r'runJit\(')

SUPPORT_DIR = 'support'


def _iter_files(tests_dir, cpp_only, skip_support):
    if not tests_dir.is_dir():
        return
    for p in sorted(tests_dir.rglob('*')):
        if not p.is_file():
            continue
        rel = p.relative_to(tests_dir)
        if skip_support and rel.parts and rel.parts[0] == SUPPORT_DIR:
            continue
        if cpp_only and p.suffix != '.cpp':
            continue
        yield p


def measure(root):
    """Return (live_calls, bare_grep, checked, per_file) for a repo root."""
    tests_dir = root / 'tests'
    live = 0
    bare = 0
    checked = 0
    per_file = {}
    for p in _iter_files(tests_dir, cpp_only=True, skip_support=True):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        n = len(RE_RUNJIT.findall(text))
        c = len(RE_RUNJIT_CHECKED.findall(text))
        live += n
        checked += c
        if n or c:
            per_file[str(p.relative_to(root))] = (n, c)
    # The control: no `--include=*.cpp`, no tests/support/ exclusion.
    for p in _iter_files(tests_dir, cpp_only=False, skip_support=False):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        bare += len(RE_BARE.findall(text))
    return live, bare, checked, per_file


def _parse_baseline(text):
    vals = {}
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        if s.startswith('<<<<<<<') or s.startswith('=======') \
                or s.startswith('>>>>>>>'):
            sys.stderr.write(
                'scripts/deprecated_jit_oracle.baseline has unresolved merge '
                'conflict markers.\n'
                '  Resolve by taking EITHER side (the counts are re-measured, '
                'so it does not matter which), then re-run with --update on '
                'the merged tree:\n'
                '      git checkout --ours scripts/deprecated_jit_oracle.baseline\n'
                '      python3 scripts/check_deprecated_jit_oracle.py --update\n'
                '  Do NOT hand-merge the counts: --update measures the merged '
                'tree, which is the only tree whose counts are correct.\n')
            raise SystemExit(2)
        if '=' not in s:
            continue
        k, _, v = s.partition('=')
        try:
            vals[k.strip()] = int(v.strip())
        except ValueError:
            sys.stderr.write(
                'scripts/deprecated_jit_oracle.baseline: cannot parse %r\n' % s)
            raise SystemExit(2)
    for key in ('live_calls', 'bare_grep', 'checked'):
        if key not in vals:
            sys.stderr.write(
                'scripts/deprecated_jit_oracle.baseline is missing %r.\n' % key)
            raise SystemExit(2)
    return vals['live_calls'], vals['bare_grep'], vals['checked']


def read_baseline(path=BASELINE):
    if not path.exists():
        sys.stderr.write('missing baseline: %s\n' % path)
        raise SystemExit(2)
    return _parse_baseline(path.read_text())


def write_baseline(live, bare, checked, path=BASELINE):
    path.write_text('\n'.join([
        '# deprecated_jit_oracle.baseline - the ratchet for the lenient JIT',
        '# test oracle (CLAUDE.md; the rise that motivated it is #3439).',
        '#',
        '# Regenerate with scripts/check_deprecated_jit_oracle.py --update after',
        '# PORTING a test from runJit to runAot.  --update refuses to raise a',
        '# count: landing a rise takes a deliberate hand-edit of this file.',
        '#',
        '# These are EXACT, not ceilings.  A count below its baseline fails as',
        '# STALE HEADROOM - a port that forgot to --update silently buys room',
        '# for the next regression.',
        '#',
        '# live_calls: `runJit(` in tests/**/*.cpp, excluding tests/support/.',
        '# bare_grep : `runJit(` anywhere under tests/, unfiltered (the control',
        '#             that distinguishes a real new call site from a recipe',
        '#             discrepancy - see the script header).',
        '# checked   : `runJitChecked(` under the live_calls filters.',
        'live_calls = %d' % live,
        'bare_grep = %d' % bare,
        'checked = %d' % checked,
        '']))


def diagnose(live_b, live_m, bare_b, bare_m):
    """The control's whole purpose: name WHICH kind of change this is."""
    dl = live_m - live_b
    db = bare_m - bare_b
    if dl == db and dl != 0:
        return ('a real call site was added or removed (both counts moved by '
                '%+d, which is the signature of a genuine change)' % dl)
    if dl != 0 and db == 0:
        return ('live_calls moved %+d but the unfiltered control did NOT move '
                '- that is the signature of a file moving into or out of '
                'tests/support/, not of a new call site. Check the move before '
                'treating this as a violation.' % dl)
    if dl == 0 and db != 0:
        return ('the unfiltered control moved %+d but live_calls did not - '
                'that is a change INSIDE the harness (tests/support/) or in a '
                'non-.cpp file, not a new test call site.' % db)
    return ('live_calls moved %+d and the control %+d - they disagree, so '
            'establish which before acting.' % (dl, db))


def check(root, baseline_path=BASELINE, stale=True):
    """Return (ok, messages).

    stale=True  (the GATE): each count must EQUAL its baseline. Both a rise
                and stale headroom fail.
    stale=False (--update): only a rise matters; a count that DROPPED is the
                good direction and is what --update is for.

    The split is not cosmetic - the enumerable-grid ratchet's precedent
    conflated them, and `--update` then refused to LOWER a count while printing
    that the baseline may only go down.
    """
    live_b, bare_b, checked_b = read_baseline(baseline_path)
    live_m, bare_m, checked_m, _ = measure(root)
    bad = []
    for name, b, m in (('live_calls', live_b, live_m),
                       ('bare_grep', bare_b, bare_m),
                       ('checked', checked_b, checked_m)):
        if m > b:
            bad.append('%s ROSE: baseline %d, tree has %d (+%d). The '
                       'deprecated JIT oracle may only ever go DOWN - write '
                       'new tests on runAot.' % (name, b, m, m - b))
        elif stale and m < b:
            bad.append('%s baseline claims %d but the tree has %d - STALE '
                       'HEADROOM (%+d). The baseline is exact, not a ceiling: '
                       're-run --update on the merged tree so the port is '
                       'banked.' % (name, b, m, m - b))
    if bad and (live_m != live_b or bare_m != bare_b):
        bad.append('DIAGNOSIS: ' + diagnose(live_b, live_m, bare_b, bare_m))
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
        # Two real tests: 3 live runJit( calls, 1 runJitChecked(.
        _mk(root, 'tests/codegen/a_test.cpp',
            'auto r = runJit(src);\nauto q = runJit(src2);\n')
        _mk(root, 'tests/sema/b_test.cpp',
            'auto r = runJit(s);\nauto c = runJitChecked(s);\n')
        # The harness's own definitions - excluded from live_calls, counted by
        # the bare control. This is the entire reason the two numbers differ.
        _mk(root, 'tests/support/aot_support.hpp',
            'Result runJit(const std::string&);\n')
        _mk(root, 'tests/support/harness.cpp',
            'Result runJit(const std::string& s) { return {}; }\n')
        # A non-.cpp file outside support/: invisible to live_calls (which is
        # .cpp-only), visible to the unfiltered control.
        _mk(root, 'tests/codegen/notes.md', 'we should stop using runJit(x)\n')

        live, bare, checked, per_file = measure(root)
        chk('live_calls counts only tests/**/*.cpp outside support/ (3)',
            live == 3)
        chk('runJitChecked( is NOT double-counted as runJit( (checked==1)',
            checked == 1)
        chk('bare control sees support/ and non-.cpp too (6)', bare == 6)
        chk('per-file map names both real test files',
            set(per_file) == {'tests/codegen/a_test.cpp',
                              'tests/sema/b_test.cpp'})

        bpath = root / 'baseline'
        write_baseline(live, bare, checked, bpath)
        good, msgs = check(root, bpath, stale=True)
        chk('a tree matching its baseline passes', good and not msgs)

        # A RISE must fail, and the diagnosis must name it as a real call site
        # (both counts move together) - the discrimination the control exists
        # for, and the exact shape of the #3439 escape.
        _mk(root, 'tests/codegen/c_test.cpp', 'auto r = runJit(s);\n')
        good, msgs = check(root, bpath, stale=True)
        chk('a new runJit( call site FAILS the ratchet', not good)
        chk('the rise is diagnosed as a real call site',
            any('real call site' in m for m in msgs))
        chk('--update REFUSES to bank a rise',
            not check(root, bpath, stale=False)[0])

        # An uncalled helper - #3439's literal shape. A definition in a test
        # file is a call-token hit and must ratchet exactly like a live call,
        # because a dead helper is how the count rose in the first place.
        (root / 'tests/codegen/c_test.cpp').unlink()
        _mk(root, 'tests/codegen/d_test.cpp',
            'static int runJitOnly(const std::string& s) { return runJit(s); }\n')
        good, _ = check(root, bpath, stale=True)
        chk('an UNCALLED helper still counts (the #3439 shape)', not good)
        (root / 'tests/codegen/d_test.cpp').unlink()

        # STALE HEADROOM: a port that forgot to --update.
        (root / 'tests/sema/b_test.cpp').write_text(
            'auto r = runAot(s);\nauto c = runJitChecked(s);\n')
        good, msgs = check(root, bpath, stale=True)
        chk('a port that did not --update fails as STALE HEADROOM',
            not good and any('STALE HEADROOM' in m for m in msgs))
        chk('--update ACCEPTS the drop', check(root, bpath, stale=False)[0])
        live2, bare2, checked2, _ = measure(root)
        write_baseline(live2, bare2, checked2, bpath)
        chk('re-baselined tree is green again',
            check(root, bpath, stale=True)[0])
        chk('the port actually lowered live_calls', live2 < live)

        # A file moving into support/ moves live_calls WITHOUT moving the
        # control. That is not a violation, and the diagnosis must say so
        # rather than reporting a phantom regression.
        src = root / 'tests/codegen/a_test.cpp'
        dst = root / 'tests/support/a_test.cpp'
        shutil.move(str(src), str(dst))
        good, msgs = check(root, bpath, stale=True)
        chk('a move into support/ is diagnosed as a MOVE, not a new call site',
            not good and any('into or out of' in m for m in msgs))

        # A missing baseline must fail loudly, never pass by default.
        missing = root / 'no-such-baseline'
        try:
            check(root, missing, stale=True)
            chk('a missing baseline fails closed', False)
        except SystemExit as e:
            chk('a missing baseline fails closed', e.code == 2)

    print('check_deprecated_jit_oracle --self-test: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--strict', action='store_true',
                    help='gate mode: counts must EQUAL the baseline')
    ap.add_argument('--self-test', action='store_true',
                    help='prove the detector still detects')
    ap.add_argument('--update', action='store_true',
                    help='re-baseline after porting a test (refuses to raise)')
    ap.add_argument('--force', action='store_true',
                    help='with --update, bank a rise anyway (deliberate)')
    ap.add_argument('--list', action='store_true',
                    help='list every file still on the deprecated oracle')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    if a.list:
        live, bare, checked, per_file = measure(ROOT)
        for f, (n, c) in sorted(per_file.items(), key=lambda kv: -kv[1][0]):
            print('%3d runJit  %3d runJitChecked  %s' % (n, c, f))
        print('--- live_calls=%d bare_grep=%d checked=%d' % (live, bare, checked))
        return 0

    if a.update:
        live, bare, checked, _ = measure(ROOT)
        if not BASELINE.exists():
            # Bootstrap. There is nothing to raise ABOVE, so the refusal below
            # has no baseline to consult; writing the first one is the only way
            # the ratchet can ever start.
            write_baseline(live, bare, checked)
            print('created baseline: live_calls=%d bare_grep=%d checked=%d'
                  % (live, bare, checked))
            return 0
        if not a.force:
            good, msgs = check(ROOT, stale=False)
            if not good:
                sys.stderr.write(
                    'check_deprecated_jit_oracle --update REFUSES to raise a '
                    'count:\n')
                for m in msgs:
                    sys.stderr.write('  ' + m + '\n')
                sys.stderr.write(
                    '  A rise means a new test was written on the deprecated '
                    'lenient oracle. Port it to runAot instead. If the rise is '
                    'genuinely intended, re-run with --force and say why in the '
                    'commit.\n')
                return 1
        write_baseline(live, bare, checked)
        print('re-baselined: live_calls=%d bare_grep=%d checked=%d'
              % (live, bare, checked))
        return 0

    good, msgs = check(ROOT, stale=True)
    if good:
        live, bare, checked, _ = measure(ROOT)
        print('check_deprecated_jit_oracle: OK (live_calls=%d bare_grep=%d '
              'checked=%d)' % (live, bare, checked))
        return 0
    for m in msgs:
        sys.stderr.write('check_deprecated_jit_oracle: ' + m + '\n')
    sys.stderr.write(
        'The deprecated JIT oracle skips the ownership and '
        'definite-assignment passes and skips verifyModule; a test on it '
        'proves less than its name suggests. Write new tests on runAot. '
        'After PORTING one, re-run with --update.\n')
    return 1 if a.strict else 0


if __name__ == '__main__':
    sys.exit(main())
