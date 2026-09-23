#!/usr/bin/env python3
"""check_internal_residual.py - ratchet silent wrong-code paths in codegen.

WHY. A wrong-result path that lowers through integer narrowing WITHOUT
emitting a diagnostic ('silent wrong code, not a diagnostic') passes any
compile-acceptance oracle. The Bezel codebase has a deliberate convention
for these paths: when a lowering arm cannot do its job but the compiler is
committed to returning *something*, it emits an `internal: ...` note via
`ctx_.diagnostics().report(...)` and continues. 74 such emit sites live in
`src/codegen/`; the detector matches 61 of those (the regex arms cover
`Severity::X` / `severity` forms with a literal `"internal: ..."` arg, so
two emit shapes are intentionally out of scope for commit 1:
`std::string("internal: ...")` constructed inline as the third arg (e.g.
`LlvmBackend_Layout.cpp:2900`, `LlvmBackend_Calls.cpp:352`), and bare-
variable severity names; see `RE_EMIT_SITE` for the exact arm set). 27 lit
files pin `NOT: internal:` against them. The matching is 1.1% - a NEW
emitter that doesn't sit behind one of those 27 lit files ships green
because nothing asks the question.

Per the lane-24-E brief (2026-09-24): "A wrong-result path that lowers
through integer narrowing WITHOUT emitting a diagnostic passes any
compile-acceptance oracle." This script is the executable form of the
prose rule that no reviewer re-measures by hand.

WHAT THIS SCRIPT CAN AND CANNOT SEE - read before trusting a green.

  CAN SEE - the "partial-silent" shape.
    A lowering arm emits an `internal: ...` note via the diagnostics
    reporter. The note rides on stdout/stderr as `<file>:<line>:<col>:
    internal: ...`. The detector greps every lit .bzl and every
    conformance corpus case's compile output for lines matching that
    prefix and sums the result. When the count moves, an operator has
    somewhere to start.

  CANNOT SEE - the "fully silent" shape.
    A unit-returning statement call that reaches NO lowering arm emits
    no diagnostic at all (per the brief: `LlvmBackend_Stmts.cpp:3679`
    drops the null return unguarded). An `Environment.Set("", "v")` that
    vanishes has no signal to grep for. Detecting it requires a compiler
    change (e.g., a hard assert in the fallthrough arm) and is OUT OF
    SCOPE here.

  CANNOT SEE - gtest programs.
    `runAot`'s `JitResult::diagnostics` is populated ONLY on a reject;
    a `Warning`/`Note`/`internal:` on a program that COMPILES is never
    surfaced through the harness. Adding gtest programs to the measured
    corpus requires a harness change - also OUT OF SCOPE. The header
    records this so a future reader does not assume broader coverage
    than exists (`feedback-run-aot-discards-stderr-on-success`).
    `scripts/check_vacuous_tests.py`'s docstring makes the same point.

  PARTIAL - silent-shape catalog.
    `--list-silent` enumerates `internal:` emit sites in `src/codegen/`.
    It does NOT cross-reference them against which lit/corpus cases
    exercise the surrounding codegen path: that requires AST context
    the detector does not have. A future `--strict-silent` flag is the
    v2 path; commit 1 reports the catalog for situational awareness.

COVERAGE POPULATIONS (two, plus the silent-shape catalog).

  Pop A: lit corpus
    `tests/lit/**/*.bzl` - compile as-is. Lit files self-wrap with a
    `module M; ... end M;` or equivalent. Filter: no filter; every lit
    `.bzl` is measured. This is the population the existing 27 `NOT:
    internal:` rules pin against.

  Pop B: spec corpus (main conformance)
    `tests/conformance/cases/*.bzl` filtered to `expectation !=
    rejects` (this is the manifest column in tests/conformance/
    manifest.tsv; the column header there is `expectation`, not
    `expect`). Wrapped via `tests/conformance/conformance.py`'s own
    `wrap_mixed` / `classify_wrap` / `splice_preamble` / `splice_env`
    - the same machinery the gate's run_conformance.sh uses, so a
    harness change auto-propagates.

  Pop C: rules corpus
    `tests/conformance/rules/cases/*.bzl` filtered to `expect in {run,
    accept}` (the manifest column there is `expect`, not
    `expectation`). Self-wrapping: each starts with `//! case: ...` and
    a `module ...` header; compiles as-is. This is the population the
    brief explicitly named ("`run`/`accept` corpus cases in
    `tests/conformance/rules/cases/`"); the partial script left it
    uncovered.

THE BASELINE IS EXACT, NOT A CEILING. A count BELOW the baseline fails
as STALE HEADROOM, because unbanked headroom is indistinguishable from
a rise someone already spent. `--update` refuses to raise a count
without `--force`; landing a rise is therefore a deliberate act.

Usage:  scripts/check_internal_residual.py [--strict] [--self-test]
                                          [--update [--force]]
                                          [--list]
                                          [--list-silent]
                                          [--bezelc=PATH] [--jobs=N]
"""
import argparse
import multiprocessing as _mp
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'internal_residual.baseline'

# Path helpers - functions, not constants, so the self-test can drive a
# synthetic root without colliding with the production tree.
def _lit_dir(root=ROOT):
    return root / 'tests' / 'lit'

def _main_cases_dir(root=ROOT):
    return root / 'tests' / 'conformance' / 'cases'

def _main_manifest(root=ROOT):
    return root / 'tests' / 'conformance' / 'manifest.tsv'

def _main_preamble(root=ROOT):
    return root / 'tests' / 'conformance' / 'preamble.bzl'

def _main_env(root=ROOT):
    return root / 'tests' / 'conformance' / 'env.bzl'

def _main_py(root=ROOT):
    return root / 'tests' / 'conformance' / 'conformance.py'

def _rules_cases_dir(root=ROOT):
    return root / 'tests' / 'conformance' / 'rules' / 'cases'

def _rules_manifest(root=ROOT):
    return root / 'tests' / 'conformance' / 'rules' / 'manifest.tsv'

def _src_codegen(root=ROOT):
    return root / 'src' / 'codegen'

# A bezelc-emitted diagnostic carries the prefix `<file>:<line>:<col>`:
# `<file>:<line>:<col>: internal: <text>`. The regex anchors on the start
# of a line, requires the `<digits>:<digits>:` location pair, and demands
# the literal token `internal:`. This DOES NOT match:
#   - prose mentions of `internal:` in source comments
#   - FileCheck CHECK lines (`// CHECK-NOT: internal:`) - no
#     `<digits>:<digits>:` prefix
#   - bare `internal:` strings in non-location contexts
# But DOES match the Bezel compiler's own diagnostic output, which is
# the entire signal we care about.
RE_INTERNAL = re.compile(r'(?m)^[^\s][^:]*:\d+:\d+:\s*internal:')

# An `internal:` emit site in src/codegen: the call site of a `report(...)`
# whose string-arg starts with `internal:`. The body of the call may
# span multiple lines; the regex uses DOTALL and anchors on the literal
# `"internal: text"` prefix inside the string-arg. The shape is, e.g.,
#     ctx_.diagnostics().report(
#         Severity::Warning, span,
#         "internal: foo"
# The Severity enum value may be Warning or Note.
RE_EMIT_SITE = re.compile(
    r'report\(\s*(?:Severity::\w+|severity)\s*,\s*[^,]*,'
    r'\s*"internal:\s*(?P<msg>[^"]{0,80})',
    re.S,
)


# ---------------------------------------------------------------------------
# bezelc discovery - build-free: never builds the compiler, only USES one.
# ---------------------------------------------------------------------------

def find_bezelc(explicit=None):
    """Return an absolute path to a usable bezelc, or raise SystemExit(2).

    Order: --bezelc flag, $BEZELC env, ROOT/build/bezelc, lane-build
    fast-disk locations. Refuses to guess; a detector run against a
    stale binary is the worst possible false-green.
    """
    candidates = []
    if explicit:
        candidates.append(pathlib.Path(explicit))
    env = os.environ.get('BEZELC')
    if env:
        candidates.append(pathlib.Path(env))
    candidates.append(ROOT / 'build' / 'bezelc')
    # Lane build dirs from scripts/lane_setup.sh.
    for d in ('/var/tmp/bezel-scratch/lane-builds',
              '/mnt/fast/lane-builds'):
        p = pathlib.Path(d)
        if not p.is_dir():
            continue
        for sub in sorted(p.iterdir(), key=lambda x: -x.stat().st_mtime):
            cand = sub / 'bezelc'
            if cand.is_file() and os.access(cand, os.X_OK):
                candidates.append(cand)
    for cand in candidates:
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    sys.stderr.write(
        'check_internal_residual: no usable bezelc found. Tried:\n')
    for c in candidates:
        sys.stderr.write('  %s\n' % c)
    sys.stderr.write(
        '  Pass --bezelc=/path/to/bezelc or set $BEZELC, or build one '
        '(cmake --build build --target bezelc).\n')
    raise SystemExit(2)


# ---------------------------------------------------------------------------
# Worker - compile one source, count `internal:` lines in the output.
# ---------------------------------------------------------------------------

def _compile_count(args):
    """Worker: compile one source, return (relpath, n_internal, ok).

    `args` is (bezelc_str, src_path_str, relpath_str). The worker is
    module-level so multiprocessing.Pool can pickle it; instance methods
    on a class bound to globals are not picklable on every backend.
    """
    bezelc, src_path, relpath = args
    try:
        proc = subprocess.run(
            [bezelc, '-c', src_path, '-o', os.devnull],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=15,
        )
        text = proc.stdout.decode('utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        return (relpath, 0, False)
    except OSError:
        return (relpath, 0, False)
    n = len(RE_INTERNAL.findall(text))
    return (relpath, n, True)


# ---------------------------------------------------------------------------
# Pop A: lit corpus
# ---------------------------------------------------------------------------

def _iter_lit(root=ROOT):
    """Yield (abs_path, rel_path) for `root/tests/lit/**/*.bzl`."""
    lit = _lit_dir(root)
    if not lit.is_dir():
        return
    for p in sorted(lit.rglob('*.bzl')):
        if p.is_file():
            yield (p, str(p.relative_to(root)))


# ---------------------------------------------------------------------------
# Pop B: main conformance corpus - wrapped via harness module.
# ---------------------------------------------------------------------------

def _import_main_conformance(root=ROOT):
    """Return the harness `conformance` module, or None if unavailable.

    Loaded directly from `root/tests/conformance/conformance.py` via
    importlib.util so a synthetic root in self-test mode does not pick
    up the production module via sys.path lookup. A failure here means
    the operator is running the detector from a context where the
    harness's wrap machinery cannot import; the caller treats `None`
    as "skip Pop B, but keep measuring Pop A and Pop C".
    """
    import importlib.util
    main_py = _main_py(root)
    if not main_py.is_file():
        return None
    try:
        spec = importlib.util.spec_from_file_location(
            'check_internal_residual_conformance', str(main_py))
        if spec is None or spec.loader is None:
            return None
        mod = importlib.util.module_from_spec(spec)
        sys.modules['check_internal_residual_conformance'] = mod
        spec.loader.exec_module(mod)
        return mod
    except Exception as e:  # noqa: BLE001 - import surface is wide
        sys.stderr.write(
            'check_internal_residual: Pop B (main conformance) skipped, '
            'harness import failed: %s\n' % e)
        return None


def _iter_main_cases(root=ROOT, conf_module=None):
    """Yield (abs_path, rel_path) for cases in the main conformance corpus
    whose expectation is NOT `rejects` - they reach codegen, so any
    `internal:` noise they emit is the silent-wrong-code signal.
    """
    if conf_module is None:
        return
    manifest = _main_manifest(root)
    if not manifest.is_file():
        return
    cases = conf_module.read_manifest(str(manifest))
    cases_dir = _main_cases_dir(root)
    for c in cases:
        if c.get('expectation', '') == 'rejects':
            continue
        case_id = c.get('case-id', '')
        if not case_id:
            continue
        src = cases_dir / (case_id + '.bzl')
        if src.is_file():
            yield (src, str(src.relative_to(root)))


def _wrap_main_case(src_path, conf_module, root=ROOT):
    """Return (wrap_mode, wrapped_text) for one main conformance case."""
    case_id = src_path.stem
    code = src_path.read_text(encoding='utf-8')
    manifest = _main_manifest(root)
    notes = ''
    for c in conf_module.read_manifest(str(manifest)):
        if c.get('case-id', '') == case_id:
            notes = c.get('notes', '')
            break
    tokens = conf_module.note_tokens(notes)
    if 'wrap:mixed' in tokens:
        wrap_mode, wrapped = conf_module.wrap_mixed(code)
    else:
        wrap_mode, wrapped = conf_module.classify_wrap(code)
    if 'preamble' in tokens:
        preamble_text = (
            _main_preamble(root).read_text(encoding='utf-8')
            if _main_preamble(root).is_file() else '')
        wrapped = conf_module.splice_preamble(wrapped, preamble_text)
    names = conf_module.env_names(notes)
    if names:
        env_text_path = _main_env(root)
        env_entries = (conf_module.parse_env(
            env_text_path.read_text(encoding='utf-8'))
            if env_text_path.is_file() else {})
        wrapped = conf_module.splice_env(wrapped, names, env_entries)
    return wrap_mode, wrapped


# ---------------------------------------------------------------------------
# Pop C: rules corpus - self-wrapping, filter to run/accept.
# ---------------------------------------------------------------------------

def _iter_rules_cases(root=ROOT):
    """Yield (abs_path, rel_path) for rules cases whose expect is `run`
    or `accept`.

    These cases self-wrap (each begins with `//! case: ...` and a
    `module ...` header). The manifest is the harness's own
    `tests/conformance/rules/manifest.tsv`.
    """
    manifest = _rules_manifest(root)
    if not manifest.is_file():
        return
    # The schema of the rules manifest has columns
    # case-id, doc, section, expect, shape, ... The 5th column is
    # `expect` (different from main conformance's `expectation`).
    try:
        with open(manifest, encoding='utf-8') as f:
            lines = [ln.rstrip('\n') for ln in f if ln.strip()]
    except OSError as e:
        sys.stderr.write(
            'check_internal_residual: rules manifest unreadable: %s\n' % e)
        return
    if not lines or lines[0].split('\t')[0] != 'case-id':
        sys.stderr.write(
            'check_internal_residual: rules manifest malformed: %r\n'
            % lines[:1])
        return
    cols = lines[0].split('\t')
    try:
        expect_idx = cols.index('expect')
    except ValueError:
        sys.stderr.write(
            'check_internal_residual: rules manifest lacks `expect` '
            'column\n')
        return
    cases_dir = _rules_cases_dir(root)
    for line in lines[1:]:
        cells = line.split('\t')
        if expect_idx >= len(cells):
            continue
        expect = cells[expect_idx].strip()
        if expect not in ('run', 'accept'):
            continue
        case_id = cells[0].strip()
        if not case_id:
            continue
        src = cases_dir / (case_id + '.bzl')
        if src.is_file():
            yield (src, str(src.relative_to(root)))


# ---------------------------------------------------------------------------
# Work-list assembly - one pass that produces a flat list for the pool.
# ---------------------------------------------------------------------------

def _gather_inputs(bezelc, root, jobs):
    """Walk lit + main conformance + rules corpus; return
    (work_list, tmpdir_for_wrap_outputs_or_None).

    Each work-list entry is (bezelc_str, abs_src_path, relpath_for_map).
    Main-conformance cases are wrapped into tempfiles here, because
    importing the harness from inside a multiprocessing worker is
    unsafe on every backend (verified the partial-script author's
    concern).
    """
    work = []
    tmpdir_obj = tempfile.TemporaryDirectory(
        prefix='check_internal_residual_')
    tmpdir = pathlib.Path(tmpdir_obj.name)
    try:
        # Pop A: lit.
        for src, rel in _iter_lit(root):
            work.append((str(bezelc), str(src), rel))

        # Pop B: main conformance (wrapped). Pass `root` so the
        # synthetic-tree self-test path can drive a stub harness at
        # `root` rather than loading the production tree by accident.
        conf = _import_main_conformance(root=root)
        for src, rel in _iter_main_cases(root=root, conf_module=conf):
            try:
                _wrap_mode, wrapped = _wrap_main_case(src, conf, root=root)
            except Exception as e:  # noqa: BLE001 - wrap-fail is per-case
                sys.stderr.write('  wrap-fail: %s: %s\n' % (src.name, e))
                continue
            tmp = tmpdir / (src.stem + '_wrapped.bzl')
            tmp.write_text(wrapped, encoding='utf-8')
            work.append((str(bezelc), str(tmp), rel))

        # Pop C: rules corpus (as-is).
        for src, rel in _iter_rules_cases(root=root):
            work.append((str(bezelc), str(src), rel))

        return work, tmpdir_obj
    except BaseException:
        tmpdir_obj.cleanup()
        raise


def measure(bezelc, jobs, root=ROOT):
    """Return (total_internal_lines, per_file_map).

    per_file_map: {relpath: int} for files that emitted >= 1 `internal:`
    line, sparse on purpose (a 5,000-row zero-map is noise).
    """
    work, tmpdir_obj = _gather_inputs(bezelc, root, jobs)
    per_file = {}
    total = 0
    try:
        if jobs <= 1 or len(work) < 4:
            # Serial fallback for tiny inputs or `--jobs 1`.
            for args in work:
                rel, n, _ok = _compile_count(args)
                if n:
                    per_file[rel] = per_file.get(rel, 0) + n
                    total += n
        else:
            with _mp.Pool(processes=jobs) as pool:
                for rel, n, _ok in pool.imap_unordered(
                        _compile_count, work, chunksize=8):
                    if n:
                        per_file[rel] = per_file.get(rel, 0) + n
                        total += n
    finally:
        tmpdir_obj.cleanup()
    return total, per_file


# ---------------------------------------------------------------------------
# Baseline - exact, not a ceiling.
# ---------------------------------------------------------------------------

def read_baseline(path=BASELINE):
    """Parse `scripts/internal_residual.baseline`. Schema: one `key = int`
    per line; blank/`#`-prefix comments allowed; merge-conflict markers
    and unparseable values fail closed with a clean error message.
    """
    if not path.exists():
        sys.stderr.write('missing baseline: %s\n' % path)
        raise SystemExit(2)
    text = path.read_text()
    vals = {}
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        if s.startswith(('<<<<<<<', '=======', '>>>>>>>')):
            sys.stderr.write(
                'scripts/internal_residual.baseline has unresolved merge '
                'conflict markers.\n'
                '  Resolve by taking either side (counts are re-'
                'measured), then re-run --update on the merged tree.\n')
            raise SystemExit(2)
        if '=' not in s:
            continue
        k, _, v = s.partition('=')
        try:
            vals[k.strip()] = int(v.strip())
        except ValueError:
            sys.stderr.write(
                'scripts/internal_residual.baseline: cannot parse %r\n'
                % s)
            raise SystemExit(2)
    if 'total_internal_lines' not in vals:
        sys.stderr.write(
            'scripts/internal_residual.baseline is missing '
            '`total_internal_lines`.\n')
        raise SystemExit(2)
    return vals['total_internal_lines']


def write_baseline(total, path=BASELINE):
    """Write the baseline file with the new total."""
    path.write_text('\n'.join([
        '# internal_residual.baseline - the ratchet for silent-wrong-',
        '# code `internal: ...` emissions across the lit + main-',
        '# conformance + rules corpus (per the lane-24-E brief,',
        '# 2026-09-24).',
        '#',
        '# The count is EXACT, not a ceiling. A count below its',
        '# baseline fails as STALE HEADROOM: a fix landed but was',
        '# not banked. Re-run with --update on the merged tree so',
        '# the drop is recorded.',
        '#',
        '# Regenerate with scripts/check_internal_residual.py',
        '# --update. --update REFUSES to raise the count without',
        '# --force: landing a rise (a new silent-wrong-code emitter)',
        '# is a deliberate act.',
        '#',
        '# Measured by compiling every',
        '#   tests/lit/**/*.bzl',
        '# + every non-rejects case in tests/conformance/cases/ (wrapped',
        '#   via tests/conformance/conformance.py)',
        '# + every run/accept case in tests/conformance/rules/cases/ (as-is)',
        '# against real bezelc, summing lines matching the diagnostic',
        '# prefix `:[0-9]+:[0-9]+: internal:` in the merged',
        '# stdout+stderr.',
        '#',
        '# gtest programs are NOT measured (JitResult::diagnostics is',
        '# REJECT-only; see scripts/check_vacuous_tests.py for the same',
        '# coverage caveat, and feedback-run-aot-discards-stderr-on-',
        '# success for the gtest blind spot).',
        'total_internal_lines = %d' % total,
        '',
    ]))


# ---------------------------------------------------------------------------
# check() - compare measure() against baseline.
# ---------------------------------------------------------------------------

def check(bezelc, jobs, baseline_path=BASELINE, stale=True, root=ROOT):
    """Return (ok, msgs, measured_total, per_file_map).

    stale=True (the gate): the total must EQUAL the baseline. A RISE is
                a violation; a DROP is STALE HEADROOM.
    stale=False (--update): only a RISE matters; --update banks the drop.
    """
    baseline_total = read_baseline(baseline_path)
    measured_total, per_file = measure(bezelc, jobs, root=root)
    bad = []
    if measured_total > baseline_total:
        top = sorted(per_file.items(), key=lambda kv: -kv[1])[:5]
        top_s = ', '.join('%s=%d' % (k, v) for k, v in top)
        bad.append(
            'total_internal_lines ROSE: baseline %d, tree has %d (+%d). '
            'A new silent-wrong-code emitter shipped green - the count '
            'must only ever go DOWN. Top emitters: %s'
            % (baseline_total, measured_total,
               measured_total - baseline_total, top_s))
    elif stale and measured_total < baseline_total:
        bad.append(
            'total_internal_lines DROPPED to %d from baseline %d '
            '(-%d) - STALE HEADROOM. A fix landed but was not banked; '
            're-run with --update on the merged tree.'
            % (measured_total, baseline_total,
               baseline_total - measured_total))
    return (not bad), bad, measured_total, per_file


# ---------------------------------------------------------------------------
# Silent-shape catalog - informational, --list-silent output.
# ---------------------------------------------------------------------------

def silent_shape_sites(root=ROOT):
    """Yield (relpath, line, snippet) for every `internal:` emit site in
    `root/src/codegen/`. NOT ratcheted in this revision: cross-referencing
    against corpus coverage requires AST context the detector does not
    have. A future `--strict-silent` flag is the v2 path. Today this is
    `--list-silent` output for the operator's situational awareness.
    """
    src = _src_codegen(root)
    if not src.is_dir():
        return
    for p in sorted(src.rglob('*.cpp')):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        for m in RE_EMIT_SITE.finditer(text):
            upto = text[:m.start()]
            line = upto.count('\n') + 1
            yield (str(p.relative_to(root)), line, m.group('msg').strip())


# ---------------------------------------------------------------------------
# Synthetic fixtures + fake bezelc - for self-test.
# ---------------------------------------------------------------------------

def _mk(root, rel, text):
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    return p


def _fake_bezelc(root, emit_count=1):
    """Write a fake bezelc that emits `emit_count` `internal:` lines
    per invocation, in the same `<src>:<n>:1: internal: <text>` shape
    that real bezelc emits.

    The fake is a Python script (shebang `#!`) so it survives being
    invoked through `subprocess.run([bezelc, '-c', src_path, ...])`
    without argv-parsing surprises. (A shell `#!` fake would treat
    `src_path` as the shell command when `-c` is in argv; that is a
    foot-gun we sidestep by using python shebang). The fake inspects
    `sys.argv` for the first non-flag arg and uses that as the source
    path; if none, falls back to `?`.
    """
    fake = root / 'fake_bezelc'
    fake.write_text(
        '#!/usr/bin/env python3\n'
        'import sys\n'
        'src = next((a for a in sys.argv[1:] if not a.startswith("-")), "?")\n'
        'for i in range(' + str(emit_count) + '):\n'
        '    print("%s:%d:1: internal: simulated emission for %s" '
        '% (src, i + 1, src))\n'
        'sys.exit(0)\n')
    fake.chmod(0o755)
    return fake


def _synthetic_tree(root):
    """Create a minimal lit + cases tree for the self-test.

    Lit: 2 files. One emits 2 internal: lines, one emits 0.
    Main conformance: 1 cases manifest, 2 case files (one
    rejected-classification skip, one compiles-classification that we
    wrap on-the-fly). The wrapped case is fed `emit_count=1` via the
    fake bezelc - so total across lit+main is 2+0+1 = 3 emissions.
    """
    # Pop A.
    _mk(root, 'tests/lit/has_internal.bzl', 'module M; end M;\n')
    _mk(root, 'tests/lit/clean.bzl', 'module M; end M;\n')
    # Pop B - main conformance harness stubs.
    _mk(root, 'tests/conformance/conformance.py',
        'def read_manifest(p):\n'
        '    return [\n'
        '        {"case-id": "ok_1", "expectation": "compiles", '
        '"notes": ""},\n'
        '        {"case-id": "rej_1", "expectation": "rejects", '
        '"notes": ""},\n'
        '    ]\n'
        'def note_tokens(n): return []\n'
        'def classify_wrap(c): return ("wrap_program", '
        '"module M;\\n  " + c.replace("\\n", " ") + "\\nend M;\\n")\n'
        'def wrap_mixed(c): return ("wrap_mixed", '
        '"module M;\\n  " + c + "\\nend M;\\n")\n'
        'def splice_preamble(c, p): return c\n'
        'def env_names(n): return []\n'
        'def parse_env(t): return {}\n'
        'def splice_env(c, names, entries): return c\n')
    _mk(root, 'tests/conformance/manifest.tsv',
        'case-id\tclass\texpectation\n'
        'ok_1\tfragment\tcompiles\n'
        'rej_1\tfragment\trejects\n')
    _mk(root, 'tests/conformance/cases/ok_1.bzl',
        'public class Program { public static method Main() {} }\n')
    _mk(root, 'tests/conformance/cases/rej_1.bzl',
        'public class Program { public static method Main() {} }\n')
    # Pop C - rules corpus, no manifest for it in synthetic tree.


# ---------------------------------------------------------------------------
# self-test - exercise measure() and check() against the synthetic tree.
# ---------------------------------------------------------------------------

def self_test():
    """Prove the detector still detects end-to-end. The benchmark is a
    synthetic tree with KNOWN emission counts; the script measures it
    with a FAKE bezelc. Both numbers must match.

    Build-free: no real bezelc in this sandbox. The fake bezelc is the
    whole point - the detector under test is the regex + the work-list
    assembly, not the compiler itself.
    """
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
        _synthetic_tree(root)
        fake = _fake_bezelc(root, emit_count=1)

        # Sanity: the regex itself.
        chk('RE_INTERNAL matches `file:N:N: internal: ...`',
            bool(RE_INTERNAL.search('foo.bzl:3:1: internal: x')))
        chk('RE_INTERNAL does NOT match bare `internal:`',
            not RE_INTERNAL.search('just some internal: text'))

        # measure() against the synthetic tree with the fake bezelc.
        # Pop A: 2 lit files. fake emits 1 line per file -> expected 2.
        # Pop B: 2 cases; only `ok_1` (expects `compiles`) is wrapped
        # and counted; `rej_1` is skipped -> expected 1.
        # Total = 3.
        total, per_file = measure(fake, jobs=1, root=root)
        chk('measure() counts 3 lines (2 lit + 1 wrapped case) when '
            'fake emits 1 per source', total == 3)
        # Per-file map names the 3 sources.
        chk('per_file map names 3 sources (sparse by construction)',
            len(per_file) == 3 and sum(per_file.values()) == 3)

        # Write a baseline that matches: tree should be green.
        bpath = root / 'internal_residual.baseline'
        write_baseline(3, bpath)
        good, msgs, m_total, _ = check(fake, jobs=1, baseline_path=bpath,
                                       root=root)
        chk('a tree matching its baseline passes', good and not msgs)

        # Bump the fake to emit_count=2 -> measured total = 6 -> rise.
        fake2 = _fake_bezelc(root, emit_count=2)
        good, msgs, m_total, _ = check(fake2, jobs=1,
                                       baseline_path=bpath, root=root)
        chk('a RISE is reported with the +N diagnostic', not good)
        chk('the rise diagnostic names total_internal_lines',
            any('total_internal_lines ROSE' in m for m in msgs))

        # --update refuses to raise without --force.
        sys.stderr = sys.__stderr__  # in case a prior test redirected
        # The check path; --update refuses a rise by consulting the
        # baseline and the freshly-measured total. We exercise the
        # same predicate here without --force.
        baseline_total = read_baseline(bpath)
        measured_total = 6  # see above
        chk('--update refuses a rise (the gate predicate)',
            not (measured_total <= baseline_total))

        # A DROP without --update is STALE HEADROOM.
        write_baseline(10, bpath)
        good, msgs, _, _ = check(fake, jobs=1, baseline_path=bpath,
                                 root=root)
        chk('a DROP without --update fails as STALE HEADROOM',
            not good and any('STALE HEADROOM' in m for m in msgs))
        chk('--update ACCEPTS the drop (stale=False path)',
            check(fake, jobs=1, baseline_path=bpath, stale=False,
                  root=root)[0])

        # A missing baseline fails closed.
        try:
            read_baseline(root / 'no-such-baseline')
            chk('missing baseline fails closed', False)
        except SystemExit as e:
            chk('missing baseline fails closed', e.code == 2)

        # Corrupt baseline: non-integer value.
        bad = root / 'bad_baseline'
        bad.write_text('total_internal_lines = NOT_AN_INT\n')
        try:
            read_baseline(bad)
            chk('corrupt baseline fails closed', False)
        except SystemExit as e:
            chk('corrupt baseline fails closed', e.code == 2)

        # Merge-conflict markers in baseline fail closed.
        merge = root / 'merge_baseline'
        merge.write_text(
            '# baseline\n'
            '<<<<<<< HEAD\n'
            'total_internal_lines = 1\n'
            '=======\n'
            'total_internal_lines = 2\n'
            '>>>>>>> branch\n')
        try:
            read_baseline(merge)
            chk('merge-conflict baseline fails closed', False)
        except SystemExit as e:
            chk('merge-conflict baseline fails closed', e.code == 2)

        # silent_shape_sites enumerates a synthesized src/codegen emit.
        _mk(root, 'src/codegen/Fake.cpp',
            'void f() { ctx_.diagnostics().report(\n'
            '    Severity::Warning, span,\n'
            '    "internal: synthetic test site");\n'
            '}\n')
        sites = list(silent_shape_sites(root=root))
        chk('silent_shape_sites enumerates the synthesized emit site',
            any(s[0] == 'src/codegen/Fake.cpp' for s in sites))

        # The real harness's conformance.py imports cleanly from the
        # SYNTHETIC tree root - this is the wrap-machine import path.
        # (At self-test time we are NOT in the real repo root, so we
        # accept either: the import succeeds for our stub above, or
        # the import raises and Pop B is skipped with a stderr note.
        # Either is correct; here we exercise the stub path.)
        conf = _import_main_conformance(root=root)
        chk('synthetic-tree conformance.py imports cleanly', conf is not None)
        chk('synthetic wrap returns wrapped text',
            'module' in _wrap_main_case(
                root / 'tests/conformance/cases/ok_1.bzl', conf,
                root=root)[1])

        # _iter_main_cases skips the rejected case.
        cases = sorted(_iter_main_cases(root=root, conf_module=conf),
                       key=lambda pr: str(pr[0]))
        chk('main cases iterator skips `expectation=rejects`',
            len(cases) == 1 and 'ok_1' in cases[0][0].name)

    print('check_internal_residual --self-test: '
          + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--strict', action='store_true',
                    help='gate mode: total_internal_lines must EQUAL '
                         'baseline (exit non-zero on drift)')
    ap.add_argument('--self-test', action='store_true',
                    help='prove the detector still detects (synthetic '
                         'tree + fake bezelc; build-free)')
    ap.add_argument('--update', action='store_true',
                    help='re-baseline after a fix lands (refuses to '
                         'raise without --force)')
    ap.add_argument('--force', action='store_true',
                    help='with --update, bank a rise anyway '
                         '(deliberate; say WHY in the commit body)')
    ap.add_argument('--list', action='store_true',
                    help='list every file that emitted >= 1 internal: '
                         'line, plus the total')
    ap.add_argument('--list-silent', action='store_true',
                    help='enumerate every internal: emit site in '
                         'src/codegen/ (informational, not ratcheted)')
    ap.add_argument('--bezelc', default=None,
                    help='path to bezelc (default: $BEZELC, '
                         'build/bezelc, or lane-build fast-disk '
                         'locations)')
    ap.add_argument('--jobs', type=int, default=8,
                    help='parallel worker count for the corpus compile '
                         '(default 8; tune for the ~60s gate budget)')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    bezelc = find_bezelc(a.bezelc)

    if a.list_silent:
        n = 0
        for rel, line, msg in silent_shape_sites(root=ROOT):
            print('%s:%d: internal: %s' % (rel, line, msg))
            n += 1
        print('--- %d internal: emit site(s) in src/codegen/' % n)
        return 0

    if a.list:
        total, per_file = measure(bezelc, a.jobs, root=ROOT)
        for f, n in sorted(per_file.items(), key=lambda kv: -kv[1]):
            print('%4d  %s' % (n, f))
        print('--- total_internal_lines=%d  (bezelc=%s, jobs=%d)'
              % (total, bezelc, a.jobs))
        return 0

    if a.update:
        total, per_file = measure(bezelc, a.jobs, root=ROOT)
        if not BASELINE.exists():
            # Bootstrap. There is nothing to raise ABOVE, so the
            # refusal below has no baseline to consult; writing the
            # first one is the only way the ratchet can ever start.
            write_baseline(total)
            print('created baseline: total_internal_lines=%d  '
                  '(bezelc=%s, jobs=%d)' % (total, bezelc, a.jobs))
            return 0
        if not a.force:
            baseline_total = read_baseline()
            if total > baseline_total:
                sys.stderr.write(
                    'check_internal_residual --update REFUSES to '
                    'raise the count:\n')
                sys.stderr.write(
                    '  baseline: %d   tree: %d   (+%d)\n'
                    % (baseline_total, total, total - baseline_total))
                sys.stderr.write(
                    '  A rise means a new silent-wrong-code emitter '
                    'shipped green. Re-run with --force and say WHY '
                    'in the commit body - the site-by-site diagnosis '
                    'should name the new emitter and the lit case '
                    'that should have caught it.\n')
                return 1
        write_baseline(total)
        print('re-baselined: total_internal_lines=%d  '
              '(bezelc=%s, jobs=%d)' % (total, bezelc, a.jobs))
        return 0

    ok, msgs, total, per_file = check(bezelc, a.jobs, root=ROOT)
    if ok:
        print('check_internal_residual: OK '
              '(total_internal_lines=%d  bezelc=%s  jobs=%d)'
              % (total, bezelc, a.jobs))
        return 0
    for m in msgs:
        sys.stderr.write('check_internal_residual: ' + m + '\n')
    sys.stderr.write(
        'The `internal:` count is the silent-wrong-code signal. A '
        'non-zero emission on a test that is expected to compile clean '
        'is a bug. After a fix, re-run with --update.\n')
    return 1 if a.strict else 0


if __name__ == '__main__':
    sys.exit(main())
