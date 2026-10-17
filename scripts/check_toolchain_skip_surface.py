#!/usr/bin/env python3
"""check_toolchain_skip_surface.py - ratchet the runAotChecked toolchain-SKIP
surface so it cannot grow while the #3733 burn-down runs.

WHY. `runAotCheckedImpl` probes `clang --version` once per process and, on
failure, returns `ran=false` with a single `Severity::Error` diagnostic reading
"<name>: SKIP — C toolchain (clang) unavailable for the ASan link"
(tests/support/aot_support.hpp:2645–2653). Call sites that read the literal
`"SKIP"` out of the message and return without asserting ABSORB that SKIP
silently — on a box without clang the whole ASan/LSan tier reports a clean
PASS having run no sanitizer at all. The surface grew ~495 call sites in 14
days (#3733, measured 2026-09-03 → 2026-09-17); the ratchet freezes it.

THREE COUNTS, LIVE + CONTROL PAIR.

  checked_calls    - occurrences of the harness call tokens
                     (runAotChecked / runAotCheckedLenient / runAotCheckedDisposal)
                     in tests/**/*.cpp, EXCLUDING tests/support/. The
                     denominator: the surface the SKIP disarms. May
                     LEGITIMATELY rise when a new checked test is added
                     (the script's --update handles this), but ONLY when
                     absorbing_sites did not also rise.
  absorbing_sites  - the violation. A site is absorbing when, inside one
                     BEZEL_TEST body, a variable bound from one of those
                     calls reaches a guard that returns without asserting
                     on the strength of the fixed literal "SKIP". Two
                     recognised shapes — (a) a call to an in-file predicate
                     whose body tests ONLY the literal "SKIP" (the classifier
                     for which already exists at
                     check_ambiguous_ran_false.py:347 _text_checking_helpers,
                     and which this script REUSES by import — see Q7 in
                     docs/proposals/papers/oracle-ratchets-2026-09.md §8), and
                     (b) an inline `if (<var>.diagnostics ... .find("SKIP")
                     != npos) return;`. absorbing_sites may ONLY fall.
  support_calls    - the control: the same tokens counted WITHOUT the
                     tests/support/ exclusion. Its job is exactly
                     bare_grep's in check_deprecated_jit_oracle.py:201-216:
                     distinguish "a new absorbing call site" from "a file
                     moved into or out of tests/support/".

BASELINE IS EXACT, NOT A CEILING. A count below its baseline fails as STALE
HEADROOM (same rule as every sibling ratchet). absorbing_sites may only fall.
checked_calls may rise, but only when absorbing_sites did not rise.

Usage:  scripts/check_toolchain_skip_surface.py [--strict] [--self-test]
                                                 [--update [--force]] [--list]
"""
import argparse
import pathlib
import re
import sys
import tempfile

# Reuse the in-file predicate classifier from check_ambiguous_ran_false.
# Per spec §1 / Q7 in oracle-ratchets-2026-09.md §8: import, don't fork — the
# two predicates must not drift. The script is module-level (has ROOT/BASELINE
# at file scope) but its body is fully behind `if __name__ == '__main__':`, so
# `import scripts.check_ambiguous_ran_false` is side-effect-free. Verified
# 2026-09-17 at b57a712e — see the lane brief for the measurement.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_ambiguous_ran_false as car  # noqa: E402


ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'toolchain_skip_surface.baseline'

# The three checked-harness entry points whose SKIP the loud-half counter
# (#3733 fix #1) counts. Each MUST be matched as a whole call token (the
# `(` follows immediately) so `runAotChecked(` is not double-counted under
# `runAotCheckedLenient(`. The control below counts the same three with the
# same whole-token discipline.
CHECKED_CALL_RE = re.compile(
    r'\b(?:runAotChecked|runAotCheckedLenient|runAotCheckedDisposal)\s*\(')
SUPPORT_CALL_RE = CHECKED_CALL_RE  # same regex, different file filter

# A BIND that mirrors check_ambiguous_ran_false.BIND but for the three
# checked-harness entry points — `auto|JitResult|bezel::test::JitResult` on
# the left, the call on the right. Only `=` is allowed (no compound binding);
# the existing BIND in car is the same shape and we reuse it directly.
BIND = re.compile(
    r'\b(?:auto|JitResult|bezel::test::JitResult)\s+(\w+)\s*=\s*'
    r'(?:bezel::test::)?'
    r'(?:runAotChecked|runAotCheckedLenient|runAotCheckedDisposal)\s*\(')

# A claim that the runAotChecked result is rejecting/clean — used to identify
# the variable an absorbing guard wants to guard. We look for both the
# `BEZEL_EXPECT_FALSE(r.ran)` shape and a bare `if (!r.ran)` shape, because a
# guard that returns on a `if (!r.ran) return;` is also absorbing.
CLAIM_RAN_FALSE = re.compile(r'(?:!\s*|\.ran\s*\)\s*,\s*true|hasErrors\s*\(\s*\)\s*\)\s*,\s*true)')
RAN_FALSE_BARE = re.compile(r'BEZEL_EXPECT_FALSE\s*\(\s*(\w+)\s*\.\s*ran\s*\)')
HAS_ERRORS_BARE = re.compile(
    r'BEZEL_EXPECT_TRUE\s*\(\s*(\w+)\s*(?:\.\s*diagnostics)?\s*\.\s*'
    r'hasErrors\s*\(\s*\)\s*\)')

# Inline `if (<var>.diagnostics... .find("SKIP") != npos) return;` shape —
# spec §1 "two recognised shapes" (b). Conservative: requires a `find("SKIP")`
# test on the same expression; bare `wasSkipped(r)` calls are handled by
# the helper-classifier path.
INLINE_SKIP_GUARD = re.compile(
    r'\b(\w+)\b[^;{}]*?\.\s*message\b[^;{}]*?\.\s*find\s*\(\s*"SKIP"\s*\)')


def _iter_files(tests_dir, cpp_only, skip_support):
    if not tests_dir.is_dir():
        return
    for p in sorted(tests_dir.rglob('*')):
        if not p.is_file():
            continue
        rel = p.relative_to(tests_dir)
        if skip_support and rel.parts and rel.parts[0] == 'support':
            continue
        if cpp_only and p.suffix != '.cpp':
            continue
        yield p


def _count_checked_calls(text):
    return len(CHECKED_CALL_RE.findall(text))


def _absorbing_sites_in_body(body, text, text_helpers, bound_vars):
    """-> number of absorbing sites in one BEZEL_TEST body.

    A site is absorbing when a variable bound from one of the checked calls
    (in `bound_vars`) reaches a guard that returns without asserting on the
    strength of "SKIP". Two shapes:

      (a) a call to an in-file predicate whose body tests ONLY the literal
          `"SKIP"` — the classifier for which is the COMPLEMENT of
          `text_helpers` (car._text_checking_helpers). `text_helpers` returns
          the PROTECTIVE helpers (those whose body tests something beyond
          SKIP — a positive rejection-text substring, for example); the
          SKIP-only helpers are everything `car.HELPER` finds in `text` that
          `_text_checking_helpers` did NOT classify as protective. We compute
          the SKIP-only set here from `text` (the file-wide scan result).
      (b) an inline `<var>.diagnostics ... .find("SKIP") != npos` guard
          before a return.

    Both shapes are absorbing if and only if NO positive-substring check on
    the rejection text runs in the body. (A positive-substring check on a
    REAL rejection is exactly the fix — see the burn-down in spec §1.)
    """
    # SKIP-only helpers: every in-file predicate that takes a JitResult and
    # reads .message, MINUS the protective set returned by
    # _text_checking_helpers. The remainder is the SKIP-only set that the
    # #3733 spec calls "absorbing".
    skip_only_helpers = set()
    for m in car.HELPER.finditer(text):
        name = m.group(1)
        params = m.group(2)
        if 'JitResult' not in params:
            continue
        if name in text_helpers:
            continue
        # Helper takes JitResult and is NOT classified as protective → it
        # tests ONLY the SKIP literal. Count it.
        skip_only_helpers.add(name)

    absorbing = 0
    for var in sorted(bound_vars):
        # Shape (b): inline `if (<var>... .find("SKIP") != npos) return;`.
        if INLINE_SKIP_GUARD.search(body):
            absorbing += 1
            continue
        # Shape (a): in-file predicate classified as SKIP-only.
        for name in skip_only_helpers:
            if re.search(r'\b' + re.escape(name) + r'\s*\(\s*' +
                         re.escape(var) + r'\s*[,)]', body):
                absorbing += 1
                break
    return absorbing


def scan_file(path, text):
    """-> list of (test_name, n_absorbing) for this file's text."""
    if 'BEZEL_TEST' not in text:
        return []
    # Same comment-stripping rationale as car.scan_file: a comment quoting
    # the literal "SKIP" must not match the guard pattern.
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
        bound_vars = {m.group(1) for m in BIND.finditer(body)}
        if not bound_vars:
            continue
        n = _absorbing_sites_in_body(body, text, text_helpers, bound_vars)
        if n:
            out.append((tname, n))
    return out


def measure(root):
    """-> (checked_calls, support_calls, absorbing_sites, per_file) for a
    repo root."""
    tests_dir = root / 'tests'
    live = 0
    support = 0
    absorbing = 0
    per_file = {}
    for p in _iter_files(tests_dir, cpp_only=True, skip_support=True):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        n = _count_checked_calls(text)
        live += n
        if not n:
            continue
        rows = scan_file(p, text)
        absorbing += sum(r[1] for r in rows)
        # Only files with at least one absorbing row belong in the
        # per-file map; the JIT ratchet's precedent
        # (check_deprecated_jit_oracle.py:122) keys the map on "(n or c)"
        # to surface every file with a call, but for the absorbing map
        # we surface only files that fail the ratchet — otherwise the
        # `--list` output is noisy with files that are not violations.
        if rows:
            per_file[str(p.relative_to(root))] = rows
    # The control: same regex, no `.cpp` filter, no support/ exclusion.
    for p in _iter_files(tests_dir, cpp_only=False, skip_support=False):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        support += len(SUPPORT_CALL_RE.findall(text))
    return live, support, absorbing, per_file


def _parse_baseline(text):
    vals = {}
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        if s.startswith('<<<<<<<') or s.startswith('=======') \
                or s.startswith('>>>>>>>'):
            sys.stderr.write(
                'scripts/toolchain_skip_surface.baseline has unresolved merge '
                'conflict markers. Resolve by taking EITHER side (the counts '
                'are re-measured by --update), then re-run --update on the '
                'merged tree:\n'
                '      git checkout --ours scripts/toolchain_skip_surface.baseline\n'
                '      python3 scripts/check_toolchain_skip_surface.py --update\n'
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
                'scripts/toolchain_skip_surface.baseline: cannot parse %r\n'
                % s)
            raise SystemExit(2)
    for key in ('absorbing_sites', 'checked_calls', 'support_calls'):
        if key not in vals:
            sys.stderr.write(
                'scripts/toolchain_skip_surface.baseline is missing %r.\n'
                % key)
            raise SystemExit(2)
    return vals['absorbing_sites'], vals['checked_calls'], vals['support_calls']


def read_baseline(path=BASELINE):
    if not path.exists():
        sys.stderr.write('missing baseline: %s\n' % path)
        raise SystemExit(2)
    return _parse_baseline(path.read_text())


def write_baseline(absorbing, checked, support, path=BASELINE):
    path.write_text('\n'.join([
        '# toolchain_skip_surface.baseline - ratchet for issue #3733:',
        '# the runAotChecked toolchain-SKIP surface (2,046 call sites in 634',
        '# files at 2026-09-17). Freezes the surface so it cannot grow while',
        '# the burn-down runs.',
        '#',
        '# Regenerate with scripts/check_toolchain_skip_surface.py --update',
        '# after a burn-down step. --update REFUSES to raise absorbing_sites',
        '# (it may only fall) and REFUSES to raise checked_calls without a',
        '# contemporaneous drop in absorbing_sites (a real rise means a new',
        '# absorbing call site, which is the defect the ratchet exists to',
        '# catch).',
        '#',
        '# EXACT, not a ceiling: a count below its baseline fails as STALE',
        '# HEADROOM (same rule as every sibling ratchet in this family).',
        '#',
        '# absorbing_sites : the violation count. Sites where a runAotChecked',
        '#                   result reaches a "return on SKIP" guard with no',
        '#                   positive rejection-text check (#3733).',
        '# checked_calls   : the denominator. runAotChecked{,,Lenient,',
        '#                   Disposal} calls in tests/**/*.cpp outside',
        '#                   tests/support/. May rise legitimately when a',
        '#                   new checked test is added (the test_main.cpp',
        '#                   additive-field / run_parallel.sh floor wires',
        '#                   the loud half).',
        '# support_calls   : the control — same tokens, no filters. Differs',
        '#                   from checked_calls only when a file moves into',
        '#                   or out of tests/support/.',
        'absorbing_sites = %d' % absorbing,
        'checked_calls   = %d' % checked,
        'support_calls   = %d' % support,
        '']))


def check(root, baseline_path=BASELINE, stale=True):
    """Return (ok, messages).

    stale=True (the GATE): each count must EQUAL its baseline. A rise AND a
                stale drop both fail.
    stale=False (--update): only rises matter. absorbing_sites may never
                rise; checked_calls may rise ONLY when absorbing_sites did
                not also rise.

    The split is not cosmetic — the JIT ratchet's precedent conflated them
    and `--update` then refused to LOWER a count while printing that the
    baseline may only go down.
    """
    abs_b, chk_b, sup_b = read_baseline(baseline_path)
    chk_m, sup_m, abs_m, _ = measure(root)
    bad = []
    if abs_m > abs_b:
        bad.append(
            'absorbing_sites ROSE: baseline %d, tree has %d (+%d). A new '
            'runAotChecked call site reaches a "return on SKIP" guard with '
            'no positive rejection-text check — the #3733 silent-skip '
            'defect. The fix is to either delete the guard (the runtime '
            'half makes it dead weight) or replace it with '
            'tests/support/aot_result_asserts.hpp::'
            'failedAtCompileTimeWithRealLocation(r).'
            % (abs_b, abs_m, abs_m - abs_b))
    if chk_m > chk_b and abs_m >= abs_b:
        bad.append(
            'checked_calls ROSE: baseline %d, tree has %d (+%d). A new '
            'runAotChecked{,,Lenient,Disposal} call site was added — that '
            'is fine ON ITS OWN, but it is paired here with absorbing_sites '
            'not falling, which is the shape of a real new absorbing site. '
            'Either bank a drop in absorbing_sites alongside the rise, or '
            'verify the new call site does not absorb a toolchain SKIP.'
            % (chk_b, chk_m, chk_m - chk_b))
    if stale:
        if abs_m < abs_b:
            bad.append(
                'absorbing_sites baseline claims %d but the tree has %d — '
                'STALE HEADROOM (%+d). The baseline is exact, not a '
                'ceiling: re-run --update on the tree that fixed these '
                'rows so the improvement is banked.'
                % (abs_b, abs_m, abs_m - abs_b))
        if chk_m < chk_b:
            bad.append(
                'checked_calls baseline claims %d but the tree has %d — '
                'STALE HEADROOM (%+d). A runAotChecked call site was '
                'removed or moved into tests/support/. Re-run --update on '
                'the merged tree.' % (chk_b, chk_m, chk_m - chk_b))
        if sup_m < sup_b:
            bad.append(
                'support_calls baseline claims %d but the tree has %d — '
                'STALE HEADROOM (%+d). The control moved; re-run --update.'
                % (sup_b, sup_m, sup_m - sup_b))
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
        # Synthetic tree mirroring the live layout: tests/codegen and
        # tests/support with the two files below.
        # ── File A: a real call with a SKIP-only `wasSkipped`-shaped helper.
        # This is the ABSORBING shape — counted in absorbing_sites.
        _mk(root, 'tests/codegen/a_test.cpp',
            'namespace { bool wasSkipped(const JitResult& r) {\n'
            '  if (r.ran) return false;\n'
            '  for (const auto& d : r.diagnostics.diagnostics())\n'
            '    if (d.message.find("SKIP") != std::string::npos) return true;\n'
            '  return false;\n'
            '} }\n'
            'BEZEL_TEST(codegen_a, runs_under_missing_toolchain) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  if (wasSkipped(r)) return;\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')
        # ── File B: the same call guarded by a predicate that ALSO tests real
        # rejection text — NOT absorbing.
        _mk(root, 'tests/codegen/b_test.cpp',
            'namespace { bool wasSkipped(const JitResult& r) {\n'
            '  if (r.ran) return false;\n'
            '  for (const auto& d : r.diagnostics.diagnostics())\n'
            '    if (d.message.find("SKIP") != std::string::npos) return true;\n'
            '  if (d.message.find("IR verification failed") != std::string::npos) return false;\n'
            '  return false;\n'
            '} }\n'
            'BEZEL_TEST(codegen_b, runs_under_real_rejection) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  if (wasSkipped(r)) return;\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')
        # ── File C: a file in tests/support/ — moves support_calls, NOT
        # checked_calls.
        _mk(root, 'tests/support/x_test.cpp',
            'inline JitResult runAotChecked(std::string_view s) { return {}; }\n'
            'BEZEL_TEST(support_x, harness) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '}\n')
        # ── File D: a file in tests/codegen/ with a non-absorbing helper.
        _mk(root, 'tests/codegen/d_test.cpp',
            'BEZEL_TEST(codegen_d, no_guard) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')
        # ── File E: inline `if (.diagnostics ... .find("SKIP") ...)` shape.
        _mk(root, 'tests/codegen/e_test.cpp',
            'BEZEL_TEST(codegen_e, inline_skip_guard) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  if (r.diagnostics.diagnostics().front().message.find("SKIP")\n'
            '      != std::string::npos) return;\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')

        live, support, absorbing, per_file = measure(root)
        # Files A, B, D, E each have one runAotChecked( call (live).
        chk('checked_calls counts only tests/**/*.cpp outside support/ (4)',
            live == 4)
        # Plus file X (tests/support/) has TWO runAotChecked( hits — one
        # the harness's own definition, one a call site. Both count under
        # the unfiltered control, which is its entire purpose (matches
        # check_deprecated_jit_oracle.py:201-216's bare_grep discipline).
        chk('support_calls counts everything including support/ (6)',
            support == 6)
        # Files A and E are absorbing; B (positive-substring predicate) and
        # D (no guard) are NOT. So 2 absorbing sites.
        chk('absorbing_sites counts only the SKIP-only / inline guards (2)',
            absorbing == 2)
        chk('per-file map names every absorbing row',
            set(per_file) == {'tests/codegen/a_test.cpp',
                              'tests/codegen/e_test.cpp'})

        bpath = root / 'baseline'
        write_baseline(absorbing, live, support, bpath)
        good, msgs = check(root, bpath, stale=True)
        chk('a tree matching its baseline passes', good and not msgs)

        # A RISE in absorbing_sites must fail the ratchet.
        _mk(root, 'tests/codegen/f_test.cpp',
            'namespace { bool wasSkipped(const JitResult& r) {\n'
            '  if (r.ran) return false;\n'
            '  for (const auto& d : r.diagnostics.diagnostics())\n'
            '    if (d.message.find("SKIP") != std::string::npos) return true;\n'
            '  return false;\n'
            '} }\n'
            'BEZEL_TEST(codegen_f, new_absorbing) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  if (wasSkipped(r)) return;\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')
        good, msgs = check(root, bpath, stale=True)
        chk('a new absorbing call site FAILS the ratchet', not good)
        chk('the rise is named as absorbing_sites',
            any('absorbing_sites ROSE' in m for m in msgs))

        # --update REFUSES to raise absorbing_sites.
        good, _ = check(root, bpath, stale=False)
        chk('--update refuses a rise in absorbing_sites', not good)

        # A RISE in checked_calls (NOT paired with a drop in absorbing_sites)
        # must also fail.
        _mk(root, 'tests/codegen/g_test.cpp',
            'BEZEL_TEST(codegen_g, no_guard) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  auto q = runAotChecked("module M;");\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')
        good, msgs = check(root, bpath, stale=True)
        chk('a checked_calls rise without an absorbing_sites drop FAILS',
            not good)
        chk('the rise is named as checked_calls',
            any('checked_calls ROSE' in m for m in msgs))

        # --update ACCEPTS the rise (banking it without absorbing_sites
        # falling is a deliberate decision the operator must make — the
        # diagnostic names the trade).
        (root / 'tests/codegen/f_test.cpp').unlink()
        (root / 'tests/codegen/g_test.cpp').unlink()
        live2, support2, absorbing2, _ = measure(root)
        write_baseline(absorbing2, live2, support2, bpath)
        chk('re-baselined tree is green again',
            check(root, bpath, stale=True)[0])
        # absorbing_sites did not rise (cycle was symmetric: rise, refuse,
        # remove). checked_calls also back to its original value (the
        # `g_test.cpp` rise was added then removed).
        chk('absorbing_sites did not rise after cycle', absorbing2 == absorbing)
        chk('checked_calls returned to its original value',
            live2 == live)

        # STALE HEADROOM: a port that forgot to --update.
        (root / 'tests/codegen/e_test.cpp').unlink()
        good, msgs = check(root, bpath, stale=True)
        chk('a port that did not --update fails as STALE HEADROOM',
            not good and any('STALE HEADROOM' in m for m in msgs))
        chk('--update ACCEPTS the drop',
            check(root, bpath, stale=False)[0])

        # A move INTO tests/support/ moves checked_calls WITHOUT moving
        # support_calls — that is the whole point of the unfiltered control,
        # and it is NOT a violation in its own right.
        #
        # Issue #6385: this used to be ONE assertion and it was provably
        # vacuous. It read
        #     not good and any('support_calls' in m for m in msgs)
        # *after* the e_test.cpp unlink above had already emitted a
        # `support_calls ... STALE HEADROOM (-1)` message — the move emits
        # no support_calls message at all, because the control has no file
        # filter and cannot see a move. Deleting the move entirely left the
        # predicate True (replayed and measured), so the assertion reported
        # coverage of a detection it never exercised. Baseline the PRE-move
        # tree here so every delta below is attributable to the move alone,
        # and assert the control's actual defining property: it does NOT move.
        pre = measure(root)
        moved_calls = _count_checked_calls(
            (root / 'tests/codegen/a_test.cpp').read_text())
        write_baseline(pre[2], pre[0], pre[1], bpath)
        src = root / 'tests/codegen/a_test.cpp'
        dst = root / 'tests/support/a_test.cpp'
        import shutil
        shutil.move(str(src), str(dst))
        good, msgs = check(root, bpath, stale=True)
        post = measure(root)
        chk("a move into support/ lowers checked_calls by exactly the moved "
            "file's checked calls", post[0] == pre[0] - moved_calls)
        chk('a move into support/ leaves the unfiltered control support_calls '
            'unchanged', post[1] == pre[1])
        # The support_calls half is load-bearing: it is the conjunct the
        # pre-#6385 assertion got for free from the earlier unlink.
        chk('the move is reported as a checked_calls STALE HEADROOM, never '
            'as a support_calls one',
            not good
            and any(m.startswith('checked_calls baseline claims')
                    for m in msgs)
            and not any('support_calls' in m for m in msgs))
        chk('a move into support/ alone raises nothing, so --update accepts '
            'it', check(root, bpath, stale=False)[0])

        # A missing baseline must fail loudly, never pass by default.
        missing = root / 'no-such-baseline'
        try:
            check(root, missing, stale=True)
            chk('a missing baseline fails closed', False)
        except SystemExit as e:
            chk('a missing baseline fails closed', e.code == 2)

        # ── --force: the flag a bank actually uses.
        #
        # Issue #6385: --force had ZERO coverage. Every assertion above that
        # says "--update ..." calls check(root, bpath, stale=False) — the
        # PREDICATE. `main()`'s `--update` branch is what reads --force, so
        # nothing exercised the real CLI path: neither the refusal nor the
        # escape hatch. This matters more than the average gap because a
        # wrong --force fails SILENTLY — it accepts a count change it should
        # question, and the baseline simply moves.
        #
        # Drive main() itself against a fixture tree + baseline. ROOT and
        # BASELINE are module globals and both check() and write_baseline()
        # bind their baseline default at def time, so redirect all four.
        import contextlib
        import io

        def _drive_main(tree, baseline, argv):
            g = globals()
            real_check = g['check']
            real_write = g['write_baseline']
            saved = {k: g[k] for k in
                     ('ROOT', 'BASELINE', 'check', 'write_baseline')}
            saved_argv = sys.argv
            buf = io.StringIO()

            def _chk(root, baseline_path=None, stale=True):
                return real_check(
                    root,
                    baseline if baseline_path is None else baseline_path,
                    stale)

            def _wr(absorbing, checked, support, path=None):
                return real_write(
                    absorbing, checked, support,
                    baseline if path is None else path)

            g['ROOT'], g['BASELINE'] = tree, baseline
            g['check'], g['write_baseline'] = _chk, _wr
            sys.argv = ['check_toolchain_skip_surface.py'] + list(argv)
            try:
                with contextlib.redirect_stdout(buf), \
                        contextlib.redirect_stderr(buf):
                    rc = main()
            finally:
                for k, v in saved.items():
                    g[k] = v
                sys.argv = saved_argv
            return rc, buf.getvalue()

        # A green fixture, then a new ABSORBING site — the rise --force
        # exists to override.
        ff = root / 'force_fixture'
        fb = ff / 'baseline'
        _mk(ff, 'tests/codegen/plain_test.cpp',
            'BEZEL_TEST(codegen_force, plain) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')
        write_baseline(0, 1, 1, fb)
        chk('the --force fixture starts green',
            check(ff, fb, stale=True)[0])
        _mk(ff, 'tests/codegen/new_absorbing_test.cpp',
            'namespace { bool wasSkipped(const JitResult& r) {\n'
            '  if (r.ran) return false;\n'
            '  for (const auto& d : r.diagnostics.diagnostics())\n'
            '    if (d.message.find("SKIP") != std::string::npos) return true;\n'
            '  return false;\n'
            '} }\n'
            'BEZEL_TEST(codegen_force, new_absorbing) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  if (wasSkipped(r)) return;\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')

        # Without --force the CLI refuses — and refuses by RETURNING, having
        # written nothing (the reviewer confirmed this is logical, not an
        # EACCES on the baseline: main() returns before write_baseline).
        # The byte-comparison is the half that matters: a refusal that still
        # moved the baseline would bank the rise it just rejected.
        pre_bytes = fb.read_bytes()
        rc, out = _drive_main(ff, fb, ['--update'])
        chk('--update REFUSES a rise in absorbing_sites at the CLI', rc == 1)
        chk('the refused --update leaves the baseline byte-identical',
            fb.read_bytes() == pre_bytes)
        chk('the refusal names the --force escape hatch',
            '--force' in out and 'absorbing_sites ROSE' in out)

        # With --force the same rise is banked deliberately.
        rc, out = _drive_main(ff, fb, ['--update', '--force'])
        chk('--update --force banks the rise at the CLI', rc == 0)
        chk('--force writes the risen absorbing_sites into the baseline',
            read_baseline(fb)[0] == 1)
        chk('the forced re-baseline leaves the tree green',
            check(ff, fb, stale=True)[0])

        # ── U18: --update must be handed the legitimate-DROP half of the
        # predicate.
        #
        # Why every --force fixture above misses it: each one is built so the
        # RISE alone triggers the refusal, and a rise fires identically under
        # stale=True and stale=False. The pre-existing drop assertions call
        # check() DIRECTLY (the predicate), so they cannot observe which
        # argument main() passed either. Net: what --update REFUSES is now
        # pinned, but WHY it refuses is not. Swapping
        # check(ROOT, stale=False) -> check(ROOT, stale=True) in main() leaves
        # all 27 assertions green (measured on this file before this block
        # existed).
        #
        # The consequence is a real unsatisfiable loop, not a cosmetic one: on
        # a genuine burn-down step the tree is BELOW its baseline, so
        # check(stale=False) gives good=True (accept and bank the
        # improvement) while check(stale=True) gives good=False (every count
        # is below its baseline -> STALE HEADROOM). --update would then
        # refuse a legitimate improvement and print "re-run --update".
        df = root / 'drop_fixture'
        db = df / 'baseline'
        _mk(df, 'tests/codegen/burn_test.cpp',
            'namespace { bool wasSkipped(const JitResult& r) {\n'
            '  if (r.ran) return false;\n'
            '  for (const auto& d : r.diagnostics.diagnostics())\n'
            '    if (d.message.find("SKIP") != std::string::npos) return true;\n'
            '  return false;\n'
            '} }\n'
            'BEZEL_TEST(codegen_drop, burned) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  if (wasSkipped(r)) return;\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')
        _mk(df, 'tests/codegen/kept_test.cpp',
            'BEZEL_TEST(codegen_drop, kept) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')
        dropped_calls = _count_checked_calls(
            (df / 'tests/codegen/burn_test.cpp').read_text())
        dpre = measure(df)
        write_baseline(dpre[2], dpre[0], dpre[1], db)
        chk('the --update drop fixture starts green',
            check(df, db, stale=True)[0])
        # The genuine burn-down step: a port fixed the row, so one file's call
        # sites (and its absorbing site) went away. The baseline is now AHEAD
        # of the tree on every count.
        (df / 'tests/codegen/burn_test.cpp').unlink()
        dpost = measure(df)
        # Sensitivity guard: this is the property that makes the fixture able
        # to see main()'s argument at all. The two modes MUST differ here --
        # if they ever agreed, the fixture below would pass for the wrong
        # reason and the assertion would stop being falsifiable.
        chk('a genuine drop is invisible to --update and visible to the gate',
            check(df, db, stale=False)[0] and not check(df, db, stale=True)[0])
        rc, out = _drive_main(df, db, ['--update'])
        chk('--update ACCEPTS a legitimate burn-down drop at the CLI',
            rc == 0 and 're-baselined' in out and 'REFUSES' not in out)
        # The bank is real: the drop is non-zero, so this cannot be satisfied
        # by a zero-delta no-op re-render of the same counts.
        chk('the accepted drop is BANKED into the baseline',
            dpost[0] == dpre[0] - dropped_calls
            and read_baseline(db) == (dpost[2], dpost[0], dpost[1]))

        # ── U20/U6: the control's SECOND filter axis, and the branch that
        # reports a control-only drop.
        #
        # U20: EVERY fixture file above is .cpp, so the control loop's
        # cpp_only=False is inert across the whole self-test -- swapping it
        # for True leaves every assertion green (measured). That is a live
        # blind spot rather than a cosmetic one: the unfiltered control is
        # exactly what makes a file moving INTO tests/support/ invisible to
        # it, which is the property A2 asserts. Measured on the LIVE tree, the
        # control reads support_calls 12 HIGHER than it would under cpp_only=
        # True -- the 5 non-.cpp files under tests/ that carry a harness token
        # (tests/README.md, tests/run_parallel.sh and three
        # tests/support/*.hpp, 1+1+1+8+1 calls).
        #
        # So A2 rests specifically on skip_support=False; the cpp_only axis
        # was the unpinned one, and this block pins it.
        #
        # U6: check()'s `if sup_m < sup_b:` branch -- the control-only STALE
        # HEADROOM -- is the branch saying "the control moved, re-run
        # --update". #6385 removed the only assertion in this file that
        # mentioned 'support_calls' in msgs, so after that commit the branch
        # had NO self-test coverage at all, in a commit whose stated theme was
        # de-vacuating that control. Deleting the branch leaves every assertion
        # green (measured). Unlinking the non-.cpp fixture below moves
        # support_calls ALONE -- checked_calls and absorbing_sites do not move
        # -- which is the only shape that can reach that branch.
        cf = root / 'ctrl_fixture'
        cb = cf / 'baseline'
        _mk(cf, 'tests/codegen/real_test.cpp',
            'BEZEL_TEST(codegen_ctrl, plain) {\n'
            '  auto r = runAotChecked("module M;");\n'
            '  BEZEL_EXPECT_FALSE(r.ran);\n'
            '}\n')
        _mk(cf, 'tests/codegen/header_test.hpp',
            'inline void harness_use() {\n'
            '  auto r = runAotChecked("module M;");\n'
            '}\n')
        cpre = measure(cf)
        chk('the control counts a NON-.cpp file that checked_calls ignores',
            cpre[0] == 1 and cpre[1] == 2)
        write_baseline(cpre[2], cpre[0], cpre[1], cb)
        chk('the control fixture is green against its own baseline',
            check(cf, cb, stale=True)[0])
        (cf / 'tests/codegen/header_test.hpp').unlink()
        good, msgs = check(cf, cb, stale=True)
        chk('a control-only drop is reported as support_calls STALE '
            'HEADROOM, and never as the other two counts',
            not good
            and any(m.startswith('support_calls baseline claims')
                    for m in msgs)
            and not any(m.startswith('checked_calls baseline claims')
                        or m.startswith('absorbing_sites baseline claims')
                        for m in msgs))
        chk('--update ACCEPTS a control-only drop (only rises matter)',
            check(cf, cb, stale=False)[0])
        cpost = measure(cf)
        chk('a control-only drop leaves checked_calls and absorbing_sites '
            'untouched',
            cpost[0] == cpre[0] and cpost[2] == cpre[2]
            and cpost[1] == cpre[1] - 1)

    print('check_toolchain_skip_surface --self-test: ' +
          ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--strict', action='store_true',
                    help='gate mode: counts must EQUAL the baseline')
    ap.add_argument('--self-test', action='store_true',
                    help='prove the detector still detects')
    ap.add_argument('--update', action='store_true',
                    help='re-baseline after a burn-down step (refuses to '
                         'raise absorbing_sites; checked_calls may rise '
                         'only when absorbing_sites does not)')
    ap.add_argument('--force', action='store_true',
                    help='with --update, bank a rise anyway (deliberate)')
    ap.add_argument('--list', action='store_true',
                    help='list every absorbing call site, then print the '
                         'three counts')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    if a.list:
        chk, sup, abs_, per_file = measure(ROOT)
        for f, rows in sorted(per_file.items(), key=lambda kv: -sum(r[1] for r in kv[1])):
            n_abs = sum(r[1] for r in rows)
            print('  %d absorbing site(s) in %s' % (n_abs, f))
            for tname, n in rows:
                print('    [%d] %s' % (n, tname))
        print('--- checked_calls=%d support_calls=%d absorbing_sites=%d ---'
              % (chk, sup, abs_))
        return 0

    if a.update:
        chk_m, sup_m, abs_m, _ = measure(ROOT)
        if not BASELINE.exists():
            write_baseline(abs_m, chk_m, sup_m)
            print('created baseline: checked_calls=%d support_calls=%d '
                  'absorbing_sites=%d' % (chk_m, sup_m, abs_m))
            return 0
        if not a.force:
            good, msgs = check(ROOT, stale=False)
            if not good:
                sys.stderr.write(
                    'check_toolchain_skip_surface --update REFUSES:\n')
                for m in msgs:
                    sys.stderr.write('  ' + m + '\n')
                sys.stderr.write(
                    '  A rise in absorbing_sites is a new silent-skip call '
                    'site — that is exactly the defect the ratchet exists '
                    'to catch. Fix the new site (delete the guard or '
                    'replace it with '
                    'tests/support/aot_result_asserts.hpp::'
                    'failedAtCompileTimeWithRealLocation) before '
                    're-running --update. If the rise is genuinely intended '
                    '— say, a new harness entry point you also want to '
                    'ratchet — re-run with --force and explain why in the '
                    'commit body.\n')
                return 1
        write_baseline(abs_m, chk_m, sup_m)
        print('re-baselined: checked_calls=%d support_calls=%d '
              'absorbing_sites=%d' % (chk_m, sup_m, abs_m))
        return 0

    good, msgs = check(ROOT, stale=True)
    if good:
        chk_m, sup_m, abs_m, _ = measure(ROOT)
        print('check_toolchain_skip_surface: OK '
              '(absorbing_sites=%d checked_calls=%d support_calls=%d)'
              % (abs_m, chk_m, sup_m))
        return 0
    for m in msgs:
        sys.stderr.write('check_toolchain_skip_surface: ' + m + '\n')
    sys.stderr.write(
        'The runAotChecked toolchain-SKIP surface (#3733) has GROWN, or '
        'the baseline is stale (an unbanked drop). A box without clang '
        'silently reports clean PASS for absorbing rows; the runtime '
        'half (counter + print + shard JSON + run_parallel.sh floor) '
        'makes the absence loud, but the burn-down only finishes when '
        'absorbing_sites reaches 0. Re-run --update only after fixing '
        'the rows the diagnostics name.\n')
    return 1 if a.strict else 0


if __name__ == '__main__':
    sys.exit(main())
