#!/usr/bin/env python3
"""behaviour_differential.py — the behaviour-preservation oracle for a PURE REFACTOR.

WHY THIS EXISTS
  A deduplication / unification change is supposed to ship NO behaviour change,
  so it can ship no test that fails without it. `[[regression-test-pins-the-bug]]`
  — every fix ships a test that FAILS WITHOUT and PASSES WITH — has nothing to
  bite on. The suite still runs, but a green suite only says "the behaviours the
  suite already pins did not move"; it says nothing about the ~2,800 programs in
  the two corpora that the suite does not assert on individually.

  The project has done this ad hoc before (one lane compiled+ran 683 programs and
  compared exit status and stdout), and the recipe lived only in a commit body,
  so every lane re-derived it. This project's own standard is that A PROSE
  RATCHET IS NOT A RATCHET. This is the executable form.

WHY EXECUTABLE MODE, NOT `cmp` ON THE OBJECT FILES
  Object-level comparison is ASYMMETRIC: `SAME` is conclusive, `DIFF` is not,
  because `bezelc` is not byte-reproducible against ITSELF. Three measured,
  independent sources of legitimate byte noise:

    1. generated symbol names embed RAW AST POINTERS (`bz$taskElemDispose$<ptr>`)
       at ~10 sites in src/codegen/ (#3548);
    2. async/iterator spill ORDER is pointer-hash dependent (#3504) — captured
       locals iterate an `unordered_map` keyed by node pointer;
    3. a change that alters `sizeof(sema::LocalSymbol)` by one bool reorders the
       spill groups: identical instruction multiset, different bytes.

  All three are invisible to "compile it, link it, RUN it, read the exit status
  and the stdout". So that is what this does. What a user of the language can
  observe is exactly what a refactor must preserve.

WHAT IS COMPARED, per program, under BASELINE and CANDIDATE:
    compile exit status | compiler diagnostics (normalized)
    run exit status     | program stdout | program stderr
  and the mismatch classes are named for those:
    compile-outcome  — the compile exit status differs (an accept/reject FLIP is
                       the severe form and is labelled as such)
    diagnostic-text  — same compile status, different compiler output
    run-outcome      — one side produced a runnable artifact and the other did not
    exit-status      — the linked program exited differently
    stdout / run-stderr — the linked program printed differently

FAIL-CLOSED (this is the part that is easy to get wrong)
  A green from a comparator that compared NOTHING is worth nothing — the exact
  failure mode `check_vacuous_tests.py`, `check_imported_symbol_reads.py` and
  `check_enumerable_grid_spellings.py` each exist to guard, and each of those
  reported a clean tree from a matcher that matched zero things on its first
  draft. So this refuses to report clean unless it MEASURED that it did work:

    * `discovered` must clear a per-corpus floor (a corpus that silently shrank);
    * every program must yield an observation from BOTH compilers (a harness
      error on more than `--max-harness-error-frac` of them fails the run);
    * at least one program must COMPILE CLEANLY under both (otherwise the whole
      comparison is over rejects, i.e. the compiler is broken or the corpus is);
    * `executed == runnable_both` EXACTLY — every program that compiled in
      executable mode under both compilers must have been RUN under both. This
      is an invariant, not a floor, so it cannot drift: it fails the moment the
      run phase silently stops happening, which is the way this tool would most
      plausibly become a false green.

  A program that fails to compile under BOTH compilers for an unrelated reason is
  NOT a mismatch — that is the whole point of a differential — but it is counted
  and printed, so "the corpus stopped compiling" is visible rather than silent.

NOISE TRIAGE (why a reported mismatch is worth believing)
  A raw diff between two runs of two compilers is not conclusive on its own: a
  corpus program may print a pointer, a time, or a hash-ordered sequence. So
  every candidate mismatch is CONFIRMED before it is reported: re-observe under
  BOTH compilers and require the baseline to agree with itself, the candidate to
  agree with itself, and the two to still disagree. Anything else is reported
  under `nondeterministic` and excluded from the verdict. Confirmation costs two
  extra observations per mismatch, which is free when the answer is clean.

USAGE
  scripts/behaviour_differential.sh --baseline BEZELC --candidate BEZELC
                                    [--corpus conformance|lit|both]
                                    [--jobs N] [--limit N] [--max-report N]
                                    [--keep] [--json FILE]
  scripts/behaviour_differential.sh --self-test

  Prefer the .sh wrapper: it runs this under the shared resource-capped slice
  (scripts/dev_heavy.sh) so several lanes can run it concurrently without
  oversubscribing the box.

CORPORA
  conformance — tests/conformance/cases, wrapped by the EXISTING authority
                (tests/conformance/conformance.py's `classify_wrap` / `wrap_mixed`
                / preamble+env splice). This module does NOT re-derive the wrap
                heuristic; a second, drifting spelling of that decision is the
                dominant defect generator this run exists to remove.
                ⚠ Unlike the conformance GATE, this includes the `skip` rows: a
                differential does not need a case to PASS, only to behave the
                same under both compilers, so a `skip` case is free coverage.
  lit         — every tests/lit/**/*.bzl, compiled as its own single file.
                Multi-file (`--import %t`) tests simply fail under both and land
                in `failed-both`.

Stdlib only. No new dependency (CLAUDE.md: dependencies are debt).
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Timeouts. A compile that hangs is a behaviour change, so a timeout is an
# OBSERVATION (sentinel status), not a harness error: two compilers that both
# hang agree, and one that newly hangs is a mismatch this must report.
# ---------------------------------------------------------------------------
COMPILE_TIMEOUT_S = 120
RUN_TIMEOUT_S = 30
TIMEOUT_RC = 124          # `timeout(1)`'s convention, reused for legibility
SPAWN_FAIL_RC = -1        # could not even start the process

# ---------------------------------------------------------------------------
# CORPUS FLOORS — measured 2026-08-30 on 907ce0f, set ~10% below the measurement.
# These catch a corpus that silently SHRANK (a moved directory, a bad glob, a
# manifest that stopped parsing). They are floors and only ever move UP; print
# both the measured value and the floor on every run so drift is visible rather
# than asserted. Override with --min-discovered / --min-runnable.
#
# `min_runnable` is deliberately about programs that reach EXECUTION under both
# compilers, because the executable phase is the entire reason this tool is not
# an object-file `cmp`, and it is the phase that would fail silently.
# ---------------------------------------------------------------------------
FLOORS = {
    # corpus:       (min_discovered, min_runnable)
    'conformance': (600, 60),
    'lit':         (1950, 300),
}

# A harness error (could not spawn, source vanished mid-run) is not an
# observation. More than this fraction of the corpus in that state means the
# harness is broken and its verdict means nothing.
MAX_HARNESS_ERROR_FRAC = 0.02

# `method Main` — the single fact that decides executable vs object mode, for
# BOTH corpora. Spelled once here rather than twice.
MAIN_RE = re.compile(r'\bmethod\s+Main\b')


# ===========================================================================
# Observation — what one compiler did with one program. PURE data.
# ===========================================================================
class Obs:
    __slots__ = ('compile_rc', 'compile_out', 'ran', 'run_rc', 'run_out',
                 'run_err', 'harness_error', 'secs')

    def __init__(self, compile_rc=0, compile_out='', ran=False, run_rc=0,
                 run_out='', run_err='', harness_error='', secs=0.0):
        self.compile_rc = compile_rc
        self.compile_out = compile_out
        self.ran = ran
        self.run_rc = run_rc
        self.run_out = run_out
        self.run_err = run_err
        self.harness_error = harness_error
        # Wall time is DELIBERATELY not part of key(): timing under a shared
        # slice is noisy and must never decide the verdict. It is reported as an
        # advisory so a refactor that makes the compiler 10x slower is visible.
        self.secs = secs

    def key(self):
        return (self.compile_rc, self.compile_out, self.ran, self.run_rc,
                self.run_out, self.run_err)

    def __eq__(self, other):
        return isinstance(other, Obs) and self.key() == other.key()

    def to_json(self):
        return {'compile_rc': self.compile_rc, 'compile_out': self.compile_out,
                'ran': self.ran, 'run_rc': self.run_rc, 'run_out': self.run_out,
                'run_err': self.run_err, 'harness_error': self.harness_error}


class Program:
    __slots__ = ('pid', 'source', 'mode', 'origin', 'slot')

    def __init__(self, pid, source, mode, origin=None):
        self.pid = pid            # stable, printable id
        self.source = source      # the .bzl actually handed to bezelc
        self.mode = mode          # 'exe' | 'obj'
        self.origin = origin or source   # what a human should go read
        # A UNIQUE artifact-name slot, assigned once the corpus is known. The
        # printable id sanitizes to a filename, and two DIFFERENT ids can
        # sanitize to the SAME one (`lit:sema/foo_bar.bzl` and
        # `lit:sema_foo/bar.bzl` both become `lit_sema_foo_bar.bzl`). Two
        # threads would then write one output path and read each other's
        # artifact — a wrong observation with nothing to show for it. The
        # corpus happens to be collision-free today (measured: 2,861 ids, 2,861
        # distinct names), which is exactly the kind of accident that stops
        # being true without anyone noticing, so the slot makes it structural.
        self.slot = 0


# ===========================================================================
# Normalization. PURE.
#
# The two compilers write their outputs to DIFFERENT directories, and a
# diagnostic or a link error can name one. Nothing else about the invocation
# differs: both compile the SAME source path (the conformance wrap is written
# ONCE, into a shared directory, precisely so the path inside a diagnostic is
# byte-identical on both sides). Trailing whitespace is dropped because it
# carries no behaviour.
# ===========================================================================
def normalize(text, subs):
    for needle, rep in subs:
        if needle:
            text = text.replace(needle, rep)
    lines = [ln.rstrip() for ln in text.replace('\r\n', '\n').split('\n')]
    while lines and lines[-1] == '':
        lines.pop()
    return '\n'.join(lines)


# ===========================================================================
# Comparison. PURE — this is the function the self-test fixtures drive.
#
# Returns a list of (klass, base_repr, cand_repr). An empty list means the two
# observations are indistinguishable to a user of the language.
#
# A compile-status difference SUBSUMES the text difference that always comes
# with it, so it is reported alone; the printed record shows both texts anyway.
# ===========================================================================
CLASSES = ('compile-outcome', 'diagnostic-text', 'run-outcome', 'exit-status',
           'stdout', 'run-stderr')

# ---------------------------------------------------------------------------
# CHANNEL WITNESSES (issue 3607). Measured 2026-08-30 over the real corpora at
# 907ce0f: of the 467 programs that ran under both compilers, only 24 printed
# ANYTHING and only 5 wrote to stderr. So a bare "467 executed, 0 stdout
# mismatches" overstates its own evidence by a factor of ~19 — the stdout
# comparison is really over 24 programs. A tool that does not say so is doing
# precisely what check_vacuous_tests.py exists to catch: claiming coverage it
# does not have. (The corpus gap itself is filed as issue 3607; this is the
# instrument that makes it visible on every run rather than once.)
#
# So every observable channel carries a WITNESS COUNT — the number of programs
# in which that channel had anything to compare — printed on every run, and a
# channel with ZERO witnesses is a HARD FAILURE, because it was not tested at
# all. (key, label, population counter, what a witness is)
# ---------------------------------------------------------------------------
CHANNELS = (
    ('w_diagnostics', 'compile diagnostics', 'both_rejected',
     'rejected by both, carrying diagnostic text'),
    ('w_warnings', 'compiler output on ACCEPT', 'both_accepted',
     'accepted by both, and still printed something'),
    ('w_exit_nonzero', 'process exit status', 'executed',
     'executed, and exited NON-ZERO'),
    ('w_stdout', 'program stdout', 'executed',
     'executed, and printed to stdout'),
    ('w_stderr', 'program stderr', 'executed',
     'executed, and wrote to stderr'),
)

# A channel's CLEAN result is thin evidence when it carried both a small SHARE
# of its population and a small ABSOLUTE number of programs, and it is labelled
# so. Both halves are needed: 24 of 467 (measured, program stdout) is thin on
# either reading, while 111 of 1254 (compiler output on accept) is a small share
# of a large corpus and is real evidence. The label never suppresses a mismatch
# — one witness is enough to REPORT a difference; it calibrates what a GREEN
# means, which is the number a reader will otherwise over-read.
THIN_CHANNEL_FRAC = 0.10
THIN_CHANNEL_ABS = 100


# subprocess reports a signal death as a NEGATIVE returncode. A crash is not a
# rejection: the direction of a crash<->reject change is the whole §6c
# loud-versus-quiet question, so it is spelled out rather than shown as a number.
_SIGNALS = {2: 'SIGINT', 4: 'SIGILL', 6: 'SIGABRT', 8: 'SIGFPE', 9: 'SIGKILL',
            11: 'SIGSEGV', 13: 'SIGPIPE', 15: 'SIGTERM'}


def _status_word(rc):
    if rc == TIMEOUT_RC:
        return 'TIMED OUT'
    if rc == SPAWN_FAIL_RC:
        return 'could not spawn'
    if rc < 0:
        return 'CRASHED on %s' % _SIGNALS.get(-rc, 'signal %d' % -rc)
    return 'accepted (rc=0)' if rc == 0 else 'rejected (rc=%d)' % rc


def classify(base, cand):
    diffs = []
    if base.compile_rc != cand.compile_rc:
        flip = (base.compile_rc == 0) != (cand.compile_rc == 0)
        crashed = (base.compile_rc < 0) != (cand.compile_rc < 0)
        klass = 'compile-outcome'
        b = _status_word(base.compile_rc)
        c = _status_word(cand.compile_rc)
        if flip:
            b += '  [ACCEPT/REJECT FLIP]'
        if crashed:
            b += ('  [NEW CRASH]' if cand.compile_rc < 0
                  else '  [crash fixed]')
        diffs.append((klass, b + _tail(base.compile_out), c + _tail(cand.compile_out)))
        return diffs
    if base.compile_out != cand.compile_out:
        diffs.append(('diagnostic-text', base.compile_out, cand.compile_out))
    if base.ran != cand.ran:
        diffs.append(('run-outcome',
                      'ran' if base.ran else 'did not run',
                      'ran' if cand.ran else 'did not run'))
        return diffs
    if base.ran:
        if base.run_rc != cand.run_rc:
            diffs.append(('exit-status', 'exit %d' % base.run_rc,
                          'exit %d' % cand.run_rc))
        if base.run_out != cand.run_out:
            diffs.append(('stdout', base.run_out, cand.run_out))
        if base.run_err != cand.run_err:
            diffs.append(('run-stderr', base.run_err, cand.run_err))
    return diffs


def _tail(text):
    return ('\n' + text) if text else ''


# ===========================================================================
# Corpus discovery.
# ===========================================================================
def _sanitize(pid):
    return re.sub(r'[^A-Za-z0-9_.-]', '_', pid)


def artifact_stem(prog, tag=''):
    """The per-program artifact basename. The slot prefix is load-bearing, not
    cosmetic — see Program.slot."""
    return '%05d.%s%s' % (prog.slot, _sanitize(prog.pid), tag)


def discover_lit(root, workdir):
    """Every tests/lit/**/*.bzl, compiled as itself. Executable mode iff the
    source declares a `Main` — the same rule the conformance side uses."""
    progs = []
    base = pathlib.Path(root) / 'tests' / 'lit'
    for path in sorted(base.rglob('*.bzl')):
        try:
            text = path.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        mode = 'exe' if MAIN_RE.search(text) else 'obj'
        progs.append(Program('lit:' + str(path.relative_to(base)), str(path),
                             mode))
    return progs


def discover_conformance(root, workdir):
    """The spec-example corpus, wrapped by tests/conformance/conformance.py.

    The wrap heuristic is NOT re-derived here. `classify_wrap` / `wrap_mixed` /
    `splice_preamble` / `splice_env` are the existing authority for "what source
    does this doc fragment become", they are already unit-tested
    (tests/conformance/test_conformance.py), and a second spelling of that
    decision here would be exactly the duplicated-classification-predicate
    defect this run exists to remove.

    Unlike the conformance GATE this keeps the `skip` rows: a differential does
    not need a case to PASS, only to behave the SAME under both compilers.
    """
    conf_dir = pathlib.Path(root) / 'tests' / 'conformance'
    sys.path.insert(0, str(conf_dir))
    try:
        import conformance as C
    finally:
        sys.path.pop(0)

    manifest = conf_dir / 'manifest.tsv'
    if not manifest.is_file():
        return []
    preamble = None
    if (conf_dir / 'preamble.bzl').is_file():
        preamble = (conf_dir / 'preamble.bzl').read_text(encoding='utf-8')
    env_entries = {}
    if (conf_dir / 'env.bzl').is_file():
        env_entries = C.parse_env((conf_dir / 'env.bzl').read_text(encoding='utf-8'))

    src_dir = pathlib.Path(workdir) / 'conformance-src'
    src_dir.mkdir(parents=True, exist_ok=True)

    progs = []
    for case in C.read_manifest(str(manifest)):
        case_id = case['case_id']
        path = conf_dir / 'cases' / (case_id + '.bzl')
        if not path.is_file():
            continue
        code = path.read_text(encoding='utf-8')
        tokens = C.note_tokens(case.get('notes'))
        try:
            if 'wrap:mixed' in tokens:
                _mode, wrapped = C.wrap_mixed(code)
            else:
                _mode, wrapped = C.classify_wrap(code)
            if 'preamble' in tokens and preamble is not None:
                wrapped = C.splice_preamble(wrapped, preamble)
            names = C.env_names(case.get('notes'))
            if names:
                wrapped = C.splice_env(wrapped, names, env_entries)
        except Exception as exc:            # a wrap that cannot be produced is
            # not a compiler behaviour question; skip it rather than pretend.
            print('  [note] conformance %s: wrap failed (%s) — excluded'
                  % (case_id, exc), file=sys.stderr)
            continue
        out = src_dir / (case_id + '.bzl')
        out.write_text(wrapped, encoding='utf-8')
        mode = 'exe' if MAIN_RE.search(wrapped) else 'obj'
        progs.append(Program('conf:' + case_id, str(out), mode,
                             origin=str(path)))
    return progs


DISCOVERERS = {'conformance': discover_conformance, 'lit': discover_lit}


# ===========================================================================
# The subprocess seam. Injectable, so the self-test drives the WHOLE pipeline
# (discovery -> observation -> confirmation -> verdict) without a real bezelc.
# ===========================================================================
def _spawn(argv, timeout, cwd=None):
    """(rc, stdout, stderr). A timeout yields TIMEOUT_RC; a spawn failure yields
    SPAWN_FAIL_RC with the reason on stderr."""
    try:
        p = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout)
        return (p.returncode,
                p.stdout.decode('utf-8', 'replace'),
                p.stderr.decode('utf-8', 'replace'))
    except subprocess.TimeoutExpired:
        return TIMEOUT_RC, '', '<timed out after %ss>' % timeout
    except OSError as exc:
        return SPAWN_FAIL_RC, '', 'spawn failed: %s' % exc


def observe(bezelc, prog, outdir, subs, spawn=_spawn, tag=''):
    """Compile (and, in executable mode, RUN) one program under one compiler."""
    t0 = time.time()
    os.makedirs(outdir, exist_ok=True)
    stem = artifact_stem(prog, tag)
    out = os.path.join(outdir, stem + ('.o' if prog.mode == 'obj' else '.exe'))
    argv = [bezelc] + (['-c'] if prog.mode == 'obj' else []) + \
        ['-o', out, prog.source]
    rc, so, se = spawn(argv, COMPILE_TIMEOUT_S)
    if rc == SPAWN_FAIL_RC:
        return Obs(compile_rc=rc, harness_error=se, secs=time.time() - t0)
    obs = Obs(compile_rc=rc, compile_out=normalize(so + se, subs))
    if prog.mode == 'exe' and rc == 0 and os.path.exists(out) \
            and os.access(out, os.X_OK):
        rrc, rso, rse = spawn([out], RUN_TIMEOUT_S)
        if rrc == SPAWN_FAIL_RC:
            obs.harness_error = rse
        else:
            obs.ran = True
            obs.run_rc = rrc
            obs.run_out = normalize(rso, subs)
            obs.run_err = normalize(rse, subs)
    try:
        os.unlink(out)
    except OSError:
        pass
    obs.secs = time.time() - t0
    return obs


# ===========================================================================
# The sweep.
# ===========================================================================
class Result:
    def __init__(self):
        self.rows = []            # (Program, base Obs, cand Obs, diffs)
        # (Program, why, base Obs, cand Obs). Kept WITH their observations: a
        # nondeterministic program was still compiled and still ran under both
        # compilers, so it counts toward the volume the run measured even though
        # it cannot be adjudicated. Dropping it from those counters would make
        # the fail-closed floors read low for a reason that is not a shortfall.
        self.nondeterministic = []
        self.harness_errors = []  # (Program, why)
        self.counts = {}


def _pair(prog, base_bezelc, cand_bezelc, base_dir, cand_dir, subs, spawn, tag=''):
    b = observe(base_bezelc, prog, base_dir, subs, spawn=spawn, tag=tag)
    c = observe(cand_bezelc, prog, cand_dir, subs, spawn=spawn, tag=tag)
    return b, c


def sweep(programs, base_bezelc, cand_bezelc, workdir, jobs, spawn=_spawn,
          confirm=True, progress=True):
    base_dir = os.path.join(workdir, 'out-baseline')
    cand_dir = os.path.join(workdir, 'out-candidate')
    subs = [(base_dir, '<OUT>'), (cand_dir, '<OUT>'),
            (os.path.abspath(base_bezelc), '<BEZELC>'),
            (os.path.abspath(cand_bezelc), '<BEZELC>'),
            (base_bezelc, '<BEZELC>'), (cand_bezelc, '<BEZELC>'),
            (workdir, '<WORK>')]
    for i, prog in enumerate(programs):
        prog.slot = i
    res = Result()
    done = [0]
    t0 = time.time()

    def task(prog):
        return prog, _pair(prog, base_bezelc, cand_bezelc, base_dir, cand_dir,
                           subs, spawn)

    suspects = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for prog, (b, c) in ex.map(task, programs):
            done[0] += 1
            if progress and done[0] % 250 == 0:
                print('  ... %d/%d observed (%.0fs)'
                      % (done[0], len(programs), time.time() - t0),
                      file=sys.stderr, flush=True)
            if b.harness_error or c.harness_error:
                res.harness_errors.append(
                    (prog, b.harness_error or c.harness_error))
                continue
            diffs = classify(b, c)
            if diffs:
                suspects.append((prog, b, c, diffs))
            else:
                res.rows.append((prog, b, c, []))

    # ---- CONFIRMATION. A raw diff is not conclusive: a corpus program may
    # print a pointer or a hash-ordered sequence. Require BOTH sides to agree
    # with themselves across a second observation and to still disagree with
    # each other. Anything else is `nondeterministic` and excluded.
    if confirm and suspects:
        print('  ... confirming %d candidate mismatch(es) with a second '
              'observation of each side' % len(suspects),
              file=sys.stderr, flush=True)

        def reconfirm(item):
            prog, b, c, diffs = item
            b2, c2 = _pair(prog, base_bezelc, cand_bezelc, base_dir, cand_dir,
                           subs, spawn, tag='.confirm')
            return prog, b, c, diffs, b2, c2

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
            for prog, b, c, diffs, b2, c2 in ex.map(reconfirm, suspects):
                if b2.harness_error or c2.harness_error:
                    res.harness_errors.append(
                        (prog, b2.harness_error or c2.harness_error))
                    continue
                if b != b2:
                    res.nondeterministic.append(
                        (prog, 'BASELINE disagreed with itself across two runs',
                         b, c))
                    continue
                if c != c2:
                    res.nondeterministic.append(
                        (prog, 'CANDIDATE disagreed with itself across two runs',
                         b, c))
                    continue
                again = classify(b, c2)
                if not again:
                    res.nondeterministic.append(
                        (prog, 'the difference did not reproduce', b, c))
                    continue
                res.rows.append((prog, b, c, again))
    else:
        res.rows.extend(suspects)
    return res


# ===========================================================================
# Verdict + report.
# ===========================================================================
def tally(programs, res):
    c = {k: 0 for k in CLASSES}
    n = {'discovered': len(programs), 'observed': 0, 'both_accepted': 0,
         'both_rejected': 0, 'exe_programs': 0, 'runnable_both': 0,
         'executed': 0, 'failed_both': 0, 'mismatched': 0,
         'nondeterministic': len(res.nondeterministic),
         'harness_errors': len(res.harness_errors),
         'baseline_secs': 0.0, 'candidate_secs': 0.0}
    for key, _l, _p, _w in CHANNELS:
        n[key] = 0
    adjudicated = [(p, b, c, d) for p, b, c, d in res.rows]
    unadjudicated = [(p, b, c, None) for p, _w, b, c in res.nondeterministic]
    for prog, b, cd, diffs in adjudicated + unadjudicated:
        n['observed'] += 1
        n['baseline_secs'] += b.secs
        n['candidate_secs'] += cd.secs
        if prog.mode == 'exe':
            n['exe_programs'] += 1
        if b.compile_rc == 0 and cd.compile_rc == 0:
            n['both_accepted'] += 1
            if prog.mode == 'exe':
                n['runnable_both'] += 1
        elif b.compile_rc != 0 and cd.compile_rc != 0:
            n['both_rejected'] += 1
            n['failed_both'] += 1
        if b.ran and cd.ran:
            n['executed'] += 1
            # Witnesses are counted on the BASELINE observation: the question a
            # witness answers is "did this channel carry anything for the
            # comparison to be over", and the baseline is the reference side.
            if b.run_rc != 0:
                n['w_exit_nonzero'] += 1
            if b.run_out.strip():
                n['w_stdout'] += 1
            if b.run_err.strip():
                n['w_stderr'] += 1
        if b.compile_rc != 0 and cd.compile_rc != 0 and b.compile_out.strip():
            n['w_diagnostics'] += 1
        if b.compile_rc == 0 and cd.compile_rc == 0 and b.compile_out.strip():
            n['w_warnings'] += 1
        if diffs:
            n['mismatched'] += 1
            for k, _, _ in diffs:
                c[k] += 1
    return n, c


def effective_floors(corpora, min_discovered, min_runnable):
    """Resolve the two floors actually in force. One function, so the report
    and the verdict can never print different numbers."""
    disc = min_discovered if min_discovered is not None \
        else sum(FLOORS[c][0] for c in corpora)
    run = min_runnable if min_runnable is not None \
        else sum(FLOORS[c][1] for c in corpora)
    return disc, run


def floor_violations(corpora, n, min_discovered, min_runnable):
    """The fail-closed guards. Returns a list of loud strings; empty is clean.

    A green from a comparator that compared nothing is worth nothing, so these
    are checked BEFORE the mismatch count and reported with the same weight."""
    bad = []
    want_disc, want_run = effective_floors(corpora, min_discovered, min_runnable)

    if n['discovered'] < want_disc:
        bad.append('CORPUS SHRANK: discovered %d program(s), floor is %d for '
                   'corpus %s. A comparator that found nothing to compare is '
                   'not a green. (--min-discovered overrides.)'
                   % (n['discovered'], want_disc, '+'.join(corpora)))
    allowed = int(n['discovered'] * MAX_HARNESS_ERROR_FRAC)
    if n['harness_errors'] > allowed:
        bad.append('HARNESS BROKEN: %d program(s) yielded no observation from '
                   'one or both compilers (cap %d = %.0f%% of %d). The verdict '
                   'over the rest means nothing until this is explained.'
                   % (n['harness_errors'], allowed,
                      MAX_HARNESS_ERROR_FRAC * 100, n['discovered']))
    if n['observed'] and n['both_accepted'] == 0:
        bad.append('NOTHING COMPILED: 0 of %d program(s) compiled cleanly under '
                   'BOTH compilers, so the whole comparison is over rejections. '
                   'Either a compiler is broken or the corpus is.'
                   % n['observed'])
    if n['executed'] != n['runnable_both']:
        bad.append('RUN PHASE DROPPED %d PROGRAM(S): %d compiled in executable '
                   'mode under both compilers but only %d were RUN under both. '
                   'Executable comparison is the entire reason this tool is not '
                   'an object-file cmp; a silent drop here is how it becomes a '
                   'false green.'
                   % (n['runnable_both'] - n['executed'], n['runnable_both'],
                      n['executed']))
    for key, label, pop, what in CHANNELS:
        if n.get(pop, 0) and n.get(key, 0) == 0:
            bad.append('CHANNEL NOT EXERCISED: `%s` had ZERO witnesses across '
                       '%d program(s) %s. A clean result on a channel nothing '
                       'was compared through is not evidence: either the corpus '
                       'stopped producing it or the harness stopped capturing '
                       'it.' % (label, n[pop], what))
    if n['runnable_both'] < want_run:
        bad.append('TOO FEW PROGRAMS REACHED EXECUTION: %d ran under both, '
                   'floor is %d for corpus %s. (--min-runnable overrides.)'
                   % (n['runnable_both'], want_run, '+'.join(corpora)))
    return bad


def _preview(text, lines=12, width=200):
    out = []
    for ln in (text or '<empty>').split('\n')[:lines]:
        out.append(ln[:width])
    if len((text or '').split('\n')) > lines:
        out.append('...')
    return out


TEXT_CLASSES = ('diagnostic-text', 'stdout', 'run-stderr')


def _unified(base_text, cand_text, lines=24, width=200):
    """A unified diff of two multi-line observations. Printed instead of two
    previews for the TEXT classes: a 120-line diagnostic that differs in five
    lines renders as two identical-looking blobs otherwise, which is how a real
    finding gets skimmed past."""
    import difflib
    out = []
    for ln in difflib.unified_diff(base_text.split('\n'), cand_text.split('\n'),
                                   'baseline', 'candidate', lineterm='', n=1):
        out.append(ln[:width])
        if len(out) >= lines:
            out.append('... (diff truncated)')
            break
    return out


def report(programs, res, n, c, corpora, max_report, base_bezelc, cand_bezelc,
           floors_bad, elapsed, floors):
    mism = [r for r in res.rows if r[3]]
    mism.sort(key=lambda r: (CLASSES.index(r[3][0][0]), r[0].pid))
    shown = 0
    for prog, b, cd, diffs in mism:
        if shown >= max_report:
            break
        shown += 1
        for klass, bs, cs in diffs:
            print('MISMATCH [%s] %s' % (klass, prog.origin))
            if klass in TEXT_CLASSES:
                for ln in _unified(bs, cs):
                    print('    ' + ln)
            else:
                for ln in _preview(bs):
                    print('    baseline  | ' + ln)
                for ln in _preview(cs):
                    print('    candidate | ' + ln)
        print('')
    if len(mism) > shown:
        print('(%d further mismatched program(s) not shown; --max-report to raise)\n'
              % (len(mism) - shown))

    if res.nondeterministic:
        print('NONDETERMINISTIC (excluded from the verdict — a side disagreed '
              'with itself across two observations):')
        for prog, why, _b, _c in res.nondeterministic[:max_report]:
            print('    %s — %s' % (prog.origin, why))
        if len(res.nondeterministic) > max_report:
            print('    (%d more)' % (len(res.nondeterministic) - max_report))
        print('')
    if res.harness_errors:
        print('HARNESS ERRORS (no observation obtained):')
        for prog, why in res.harness_errors[:max_report]:
            print('    %s — %s' % (prog.origin, why))
        if len(res.harness_errors) > max_report:
            print('    (%d more)' % (len(res.harness_errors) - max_report))
        print('')

    want_disc, want_run = floors
    print('=== measured ===')
    print('  corpus                    %s' % '+'.join(corpora))
    print('  baseline                  %s' % base_bezelc)
    print('  candidate                 %s' % cand_bezelc)
    print('  wall                      %.0fs' % elapsed)
    print('  discovered                %6d   (floor %d)' % (n['discovered'], want_disc))
    print('  compared                  %6d' % n['observed'])
    print('  compiled under both       %6d' % n['both_accepted'])
    print('  rejected by both          %6d   (not a mismatch — see the header)'
          % n['both_rejected'])
    print('  executable-mode programs  %6d' % n['exe_programs'])
    print('  runnable under both       %6d   (floor %d)' % (n['runnable_both'], want_run))
    print('  EXECUTED under both       %6d   (must equal `runnable under both`)'
          % n['executed'])
    print('  nondeterministic          %6d   (excluded)' % n['nondeterministic'])
    print('  harness errors            %6d' % n['harness_errors'])
    print('  compiler wall, baseline   %6.0fs  ADVISORY — timing under a shared'
          % n['baseline_secs'])
    print('  compiler wall, candidate  %6.0fs  slice is noisy and never decides'
          % n['candidate_secs'])
    print('                                    the verdict.')
    print('')
    print('=== observable channels actually exercised ===')
    print('  A CLEAN result is only as strong as the number of programs in which')
    print('  the channel carried anything to compare. Thin channels are marked.')
    for key, label, pop, what in CHANNELS:
        population = n.get(pop, 0)
        wit = n.get(key, 0)
        thin = population and wit < THIN_CHANNEL_ABS \
            and wit < max(1, int(population * THIN_CHANNEL_FRAC))
        print('  %-26s %5d witness(es) of %5d %s%s'
              % (label, wit, population, what, '   <- THIN' if thin else ''))
    print('')
    print('=== mismatches (confirmed by a second observation of each side) ===')
    for k in CLASSES:
        print('  %-24s %6d' % (k, c[k]))
    print('  %-24s %6d  program(s), %d finding(s)'
          % ('TOTAL', n['mismatched'], sum(c.values())))
    print('')

    if floors_bad:
        print('## BEHAVIOUR DIFFERENTIAL: FAIL — the run did not measure enough '
              'to certify anything:')
        for v in floors_bad:
            print('  * ' + v)
        return 1
    if n['mismatched']:
        print('## BEHAVIOUR DIFFERENTIAL: FAIL — %d program(s) behave differently '
              'under the candidate compiler.' % n['mismatched'])
        print('   A pure refactor must change none of them. Each finding above '
              'was re-observed on both')
        print('   sides and reproduced, so it is not corpus noise.')
        return 1
    print('behaviour_differential: clean — %d program(s) compared, %d compiled '
          'under both, %d executed' % (n['observed'], n['both_accepted'],
                                       n['executed']))
    print('  under both, 0 confirmed mismatches.')
    return 0


# ===========================================================================
# Job sizing. Mirrors tests/support/aot_support.hpp's aotLinkTokenCount():
# min(PHYSICAL cores, MemAvailable/1.5GiB), overridable by BEZEL_AOT_LINK_TOKENS
# — the machine-wide knob for "several agents are running heavy work at once".
# Physical, not logical: a compile+link is largely single-threaded and RAM-heavy.
# ===========================================================================
def physical_cores():
    try:
        text = pathlib.Path('/proc/cpuinfo').read_text()
    except OSError:
        return max(1, (os.cpu_count() or 8) // 2)
    cores = siblings = 0
    for line in text.split('\n'):
        if not cores and line.startswith('cpu cores'):
            cores = int(line.split(':')[1])
        if not siblings and line.startswith('siblings'):
            siblings = int(line.split(':')[1])
    logical = os.cpu_count() or 8
    if cores and siblings:
        return max(1, logical * cores // siblings)
    return max(1, logical // 2)


def memory_cap():
    try:
        for line in pathlib.Path('/proc/meminfo').read_text().split('\n'):
            if line.startswith('MemAvailable:'):
                kib = int(line.split()[1])
                return max(2, int(kib / 1024.0 / 1024.0 / 1.5))
    except (OSError, ValueError, IndexError):
        pass
    return 1 << 20


def default_jobs():
    env = os.environ.get('BEZEL_AOT_LINK_TOKENS', '')
    if env.strip():
        try:
            n = int(env)
            if n > 0:
                return n
        except ValueError:
            pass
    return max(1, min(physical_cores(), memory_cap()))


# ===========================================================================
# Workdir. Small-file churn (two artifacts per program) belongs on the fast
# disk for the reason CLAUDE.md records for the AOT bincache: sda3 btrfs does
# ~5 durable creates/s against the NVMe's ~846. Same policy as
# scripts/lane_setup.sh — key on whether the fast root is a MOUNT POINT, never
# on it merely existing (an unmounted /mnt/fast is plain sda3 underneath).
# ===========================================================================
def choose_workroot():
    override = os.environ.get('BEZEL_DIFF_WORKROOT')
    if override:
        return override
    fast = os.environ.get('BEZEL_AOT_FAST_ROOT', '/mnt/fast')
    if fast and os.path.ismount(fast):
        return os.path.join(fast, 'behaviour-diff')
    return '/var/tmp/bezel-scratch/behaviour-diff'


# ===========================================================================
# SELF-TEST — the detector must go RED on a known positive before it is allowed
# to certify anything. Two levels, because each catches what the other cannot:
#
#   1. PURE FIXTURES over classify(): every mismatch class, plus the negatives
#      (identical observations, and same-difference-different-noise).
#   2. END-TO-END, over two SYNTHETIC compilers and a synthetic corpus, driving
#      the real discovery -> observe -> confirm -> tally -> floors pipeline.
#      A fixture table cannot catch a wiring bug — a run phase that never fires,
#      a floor that is never evaluated, a confirmation that swallows every
#      difference — and those are exactly the ways this tool would silently
#      become a false green.
#
# `check_vacuous_tests.py`'s first draft reported the whole tree clean because
# its regex matched zero tests. The end-to-end arm is the control run that makes
# the same thing impossible here.
# ===========================================================================
def _obs(**kw):
    return Obs(**kw)


CLASSIFY_FIXTURES = [
    ('identical-reject',
     _obs(compile_rc=1, compile_out='a.bzl:1:1: error: nope'),
     _obs(compile_rc=1, compile_out='a.bzl:1:1: error: nope'), []),
    ('identical-accept-and-run',
     _obs(compile_rc=0, ran=True, run_rc=0, run_out='hello'),
     _obs(compile_rc=0, ran=True, run_rc=0, run_out='hello'), []),
    ('accept-to-reject FLIP',
     _obs(compile_rc=0, ran=True, run_rc=0, run_out='hi'),
     _obs(compile_rc=1, compile_out='a.bzl:1:1: error: newly rejected'),
     ['compile-outcome']),
    ('reject-to-accept FLIP',
     _obs(compile_rc=1, compile_out='a.bzl:1:1: error: old'),
     _obs(compile_rc=0, ran=True, run_rc=0, run_out='hi'),
     ['compile-outcome']),
    ('rc 1 -> rc 2 is still a compile-outcome change',
     _obs(compile_rc=1, compile_out='x'), _obs(compile_rc=2, compile_out='x'),
     ['compile-outcome']),
    ('newly hangs',
     _obs(compile_rc=0, ran=True), _obs(compile_rc=TIMEOUT_RC),
     ['compile-outcome']),
    ('both hang — agreement, not a mismatch',
     _obs(compile_rc=TIMEOUT_RC, compile_out='<timed out after 120s>'),
     _obs(compile_rc=TIMEOUT_RC, compile_out='<timed out after 120s>'), []),
    ('diagnostic text drifted',
     _obs(compile_rc=1, compile_out='a.bzl:3:1: error: expected `;` (§17.4)'),
     _obs(compile_rc=1, compile_out='a.bzl:3:1: error: expected `;` (§17.5)'),
     ['diagnostic-text']),
    ('a NEW warning on an accepted program',
     _obs(compile_rc=0, compile_out='', ran=True, run_rc=0, run_out='x'),
     _obs(compile_rc=0, compile_out='a.bzl:2:1: warning: unused', ran=True,
          run_rc=0, run_out='x'),
     ['diagnostic-text']),
    ('exit status changed',
     _obs(compile_rc=0, ran=True, run_rc=0, run_out='x'),
     _obs(compile_rc=0, ran=True, run_rc=3, run_out='x'), ['exit-status']),
    ('stdout changed',
     _obs(compile_rc=0, ran=True, run_rc=0, run_out='1 2 3'),
     _obs(compile_rc=0, ran=True, run_rc=0, run_out='1 3 2'), ['stdout']),
    ('stdout AND exit status changed — both reported',
     _obs(compile_rc=0, ran=True, run_rc=0, run_out='a'),
     _obs(compile_rc=0, ran=True, run_rc=1, run_out='b'),
     ['exit-status', 'stdout']),
    ('program stderr changed (an unhandled exception message)',
     _obs(compile_rc=0, ran=True, run_rc=70, run_out='', run_err='E: boom'),
     _obs(compile_rc=0, ran=True, run_rc=70, run_out='', run_err='E: bang'),
     ['run-stderr']),
    ('linked but did not run on one side',
     _obs(compile_rc=0, ran=True, run_rc=0, run_out='x'),
     _obs(compile_rc=0, ran=False), ['run-outcome']),
    ('both failed to compile, identically — the corpus, not the compiler',
     _obs(compile_rc=1, compile_out='a.bzl:1:1: error: unknown name `Frobnicate`'),
     _obs(compile_rc=1, compile_out='a.bzl:1:1: error: unknown name `Frobnicate`'),
     []),
    ('a crash the candidate FIXED into a clean rejection',
     _obs(compile_rc=-11),
     _obs(compile_rc=1, compile_out='a.bzl:4:9: error: expected `;`'),
     ['compile-outcome']),
    ('a clean rejection the candidate turned into a CRASH',
     _obs(compile_rc=1, compile_out='a.bzl:4:9: error: expected `;`'),
     _obs(compile_rc=-11), ['compile-outcome']),
    ('both crash the same way — agreement, not a mismatch',
     _obs(compile_rc=-11), _obs(compile_rc=-11), []),
    ('the SIGNAL changed',
     _obs(compile_rc=-11), _obs(compile_rc=-6), ['compile-outcome']),
]


# The rendered LABEL is load-bearing, not cosmetic: the direction of a
# crash<->reject change is the §6c loud-versus-quiet question, and a reader
# skimming the report decides severity from this line. A class-only assertion
# cannot see it, so the wording is pinned directly.
LABEL_FIXTURES = [
    (_obs(compile_rc=-11), _obs(compile_rc=1, compile_out='e'),
     ['CRASHED on SIGSEGV', 'crash fixed'], ['NEW CRASH', 'FLIP']),
    (_obs(compile_rc=1, compile_out='e'), _obs(compile_rc=-11),
     ['CRASHED on SIGSEGV', 'NEW CRASH'], ['crash fixed', 'FLIP']),
    (_obs(compile_rc=0), _obs(compile_rc=1, compile_out='e'),
     ['ACCEPT/REJECT FLIP', 'accepted (rc=0)', 'rejected (rc=1)'],
     ['CRASH', 'crash fixed']),
    (_obs(compile_rc=0), _obs(compile_rc=-11),
     ['ACCEPT/REJECT FLIP', 'NEW CRASH', 'CRASHED on SIGSEGV'], ['crash fixed']),
    (_obs(compile_rc=0, ran=True), _obs(compile_rc=TIMEOUT_RC),
     ['TIMED OUT'], ['CRASH']),
]


def _write(path, text, mode=0o644):
    p = pathlib.Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding='utf-8')
    os.chmod(path, mode)


def _fake_compiler(path, behaviour):
    """A stand-in `bezelc`. Understands `[-c] -o OUT SRC`. `behaviour` is python
    source evaluated with `src` (the source text) in scope and expected to leave
    `rc`, `diag`, `prog` set: `prog` is the shell body of the executable it
    "links" (None ⇒ emit nothing, as a `-c` compile does)."""
    _write(path, '''#!/usr/bin/env python3
import os, sys
args = sys.argv[1:]
obj = '-c' in args
out = args[args.index('-o') + 1]
srcp = [a for a in args if not a.startswith('-') and a != out][0]
src = open(srcp).read()
rc = 0; diag = ''; prog = None
%s
if diag:
    sys.stderr.write(diag)
if rc == 0 and prog is not None and not obj:
    open(out, 'w').write('#!/bin/sh\\n' + prog + '\\n')
    os.chmod(out, 0o755)
elif rc == 0:
    open(out, 'w').write('OBJ')
sys.exit(rc)
''' % behaviour, mode=0o755)


# Synthetic corpus. Every OBSERVABLE CHANNEL must have a witness here, or the
# end-to-end arm certifies a pipeline it never drove through that channel — the
# same vacuity the CHANNELS guard exists to catch on the real corpora. So the
# corpus deliberately contains a program that is rejected (diagnostics), one
# accepted WITH compiler output (the warning channel), one that exits non-zero,
# one that writes to stderr, ones that print, one that is object-only, and one
# that is genuinely nondeterministic.
SELFTEST_CORPUS = {
    'ok_hello':   'module M;\nclass W\n  public static method Main()\n'
                  '    Console.WriteLine("hello");\n  end Main;\nend W;\n',
    'ok_two':     'module M;\nclass W\n  public static method Main()\n'
                  '    Console.WriteLine("two");\n  end Main;\nend W;\n',
    'ok_three':   'module M;\nclass W\n  public static method Main()\n'
                  '    Console.WriteLine("three");\n  end Main;\nend W;\n',
    'ok_four':    'module M;\nclass W\n  public static method Main()\n'
                  '    Console.WriteLine("four");\n  end Main;\nend W;\n',
    'lib_only':   'module M;\nclass Helper\n  public X: int;\nend Helper;\n',
    'lib_two':    'module M;\nclass Other\n  public Y: int;\nend Other;\n',
    'rejected':   'module M;\nREJECT this is not Bezel\n',
    'flaky':      'module M;\nclass W\n  public static method Main()\n'
                  '    FLAKY\n  end Main;\nend W;\n',
    'warns':      'module M;\nclass W\n  public static method Main()\n'
                  '    WARNME\n  end Main;\nend W;\n',
    'exits3':     'module M;\nclass W\n  public static method Main()\n'
                  '    EXIT3\n  end Main;\nend W;\n',
    'errline':    'module M;\nclass W\n  public static method Main()\n'
                  '    ERRLINE\n  end Main;\nend W;\n',
}

# The baseline synthetic compiler. Each variant below is this text plus ONE
# delta line, so a known positive differs from the control in exactly the way
# its name says and nothing else.
_SELF_BASE = """
if 'REJECT' in src:
    rc = 1; diag = srcp + ':2:1: error: unknown declaration (SELFTEST)\\n'
elif 'WARNME' in src:
    diag = srcp + ':2:1: warning: accepted, with compiler output (SELFTEST)\\n'
    prog = 'echo warned'
elif 'EXIT3' in src:
    prog = 'echo three-out\\nexit 3'
elif 'ERRLINE' in src:
    prog = 'echo out\\necho boom >&2'
elif 'FLAKY' in src:
    import random
    prog = 'echo ' + str(random.randint(0, 10**9))
elif 'method Main' in src:
    prog = 'echo ' + src.split('"')[1]
"""

# (delta appended to _SELF_BASE, the class it must be reported under, how many
#  programs it must be reported over). The counts are derived from the corpus
#  above and are pinned rather than computed, so a corpus edit that changes what
#  is exercised fails the self-test instead of silently weakening it.
_SELF_VARIANTS = {
    # A program the candidate newly REJECTS: only `ok_three` matches.
    'flip': ("""
if 'method Main' in src and 'three' in src:
    rc = 1; diag = srcp + ':1:1: error: newly rejected (SELFTEST)\\n'; prog = None
""", 'compile-outcome', 1),
    # An extra line on stdout for every program that runs. Two of the eight
    # runnable programs do NOT report: `flaky` is excluded by the confirmation
    # step, and `exits3` exits before the appended line ever executes. That
    # second exclusion is the point of a differential: it reports what the
    # program DID, not what the compiler was asked to emit.
    'stdout': ("""
if prog is not None and rc == 0:
    prog = prog + '\\necho EXTRA'
""", 'stdout', 6),
    # A different exit status. `exits3` already exits before the appended line,
    # so it is unaffected; `flaky` is excluded. 6 report.
    'exit': ("""
if prog is not None and rc == 0 and 'exit' not in prog:
    prog = prog + '\\nexit 7'
""", 'exit-status', 6),
    # A reworded REJECTION diagnostic.
    'diag': ("""
if rc != 0:
    diag = diag.replace('unknown declaration', 'unknown declaration, reworded')
""", 'diagnostic-text', 1),
    # A reworded WARNING on an ACCEPTED program — the channel a differential
    # over exit-status-and-stdout alone cannot see at all.
    'warn-text': ("""
if rc == 0 and diag:
    diag = diag.replace('with compiler output', 'with DIFFERENT compiler output')
""", 'diagnostic-text', 1),
    # A changed message on the program's own stderr.
    'stderr': ("""
if prog is not None and 'boom' in prog:
    prog = prog.replace('boom', 'bang')
""", 'run-stderr', 1),
    # The nastiest false-green shape: a candidate that compiles everything but
    # silently stops emitting a runnable artifact. The run phase then never
    # fires, and a comparator that only diffs stdout sees "empty on both sides".
    'no-artifact': ("""
if 'FLAKY' not in src:
    prog = None
""", 'run-outcome', 7),
    # A real signal death, not a fixture: `subprocess` reports it as a NEGATIVE
    # returncode, which a naive comparator renders as "rejected (rc=-11)". The
    # first real run of this tool found four of these in the baseline compiler.
    'crash': ("""
if 'REJECT' in src:
    import signal
    os.kill(os.getpid(), signal.SIGSEGV)
""", 'compile-outcome', 1),
}


def _selftest_programs(srcdir):
    progs = []
    for name, text in sorted(SELFTEST_CORPUS.items()):
        p = os.path.join(srcdir, name + '.bzl')
        _write(p, text)
        progs.append(Program('self:' + name, p,
                             'exe' if MAIN_RE.search(text) else 'obj'))
    return progs


def _run_selftest_case(tmp, label, variant, jobs):
    """Drive the REAL pipeline over two synthetic compilers. Returns
    (exit_code, n, counts, res)."""
    d = os.path.join(tmp, label)
    srcdir = os.path.join(d, 'src')
    work = os.path.join(d, 'work')
    os.makedirs(work, exist_ok=True)
    base = os.path.join(d, 'bezelc-base')
    cand = os.path.join(d, 'bezelc-cand')
    _fake_compiler(base, _SELF_BASE)
    _fake_compiler(cand, _SELF_BASE if variant is None
                   else _SELF_BASE + _SELF_VARIANTS[variant][0])
    progs = _selftest_programs(srcdir)
    res = sweep(progs, base, cand, work, jobs, progress=False)
    n, c = tally(progs, res)
    return n, c, res


def self_test():
    ok = True

    print('classify() fixtures — the detector must name each class exactly:')
    for label, b, c, want in CLASSIFY_FIXTURES:
        got = [k for k, _, _ in classify(b, c)]
        good = got == want
        ok = ok and good
        print('  [%s] %-52s expected %-34s got %s'
              % ('ok  ' if good else 'FAIL', label, want, got))

    # A comparison is only meaningful if it is SYMMETRIC in the classes it can
    # report: swapping the two sides must find the same kinds of difference.
    for label, b, c, want in CLASSIFY_FIXTURES:
        got = [k for k, _, _ in classify(c, b)]
        good = sorted(got) == sorted(want)
        ok = ok and good
        if not good:
            print('  [FAIL] %s: swapping the sides changed the classes: %s vs %s'
                  % (label, got, want))
    print('  [ok  ] classify() is symmetric in the classes it reports'
          if ok else '  [FAIL] classify() is NOT symmetric')

    print('\nrendered labels — the crash/flip wording a reader judges severity by:')
    for b, c, want, unwanted in LABEL_FIXTURES:
        rendered = ' || '.join(x + ' :: ' + y for _k, x, y in classify(b, c))
        miss = [w for w in want if w not in rendered]
        leak = [u for u in unwanted if u in rendered]
        good = not miss and not leak
        ok = ok and good
        print('  [%s] %-58s%s%s'
              % ('ok  ' if good else 'FAIL', rendered.split('\n')[0][:58],
                 (' MISSING %s' % miss) if miss else '',
                 (' LEAKED %s' % leak) if leak else ''))

    # Artifact names must be unique even when two DIFFERENT program ids
    # sanitize to the SAME filename. Without the slot prefix two threads write
    # one path and read each other's artifact: a wrong observation that leaves
    # no trace. The corpus is collision-free today, so only a direct check can
    # keep this honest.
    a = Program('lit:sema/foo_bar.bzl', '/x/a.bzl', 'obj')
    b = Program('lit:sema_foo/bar.bzl', '/x/b.bzl', 'obj')
    a.slot, b.slot = 7, 8
    hazard_real = _sanitize(a.pid) == _sanitize(b.pid)
    distinct = artifact_stem(a) != artifact_stem(b)
    confirm_distinct = artifact_stem(a) != artifact_stem(a, '.confirm')
    good = hazard_real and distinct and confirm_distinct
    ok = ok and good
    print('\n  [%s] artifact names: two ids that sanitize alike (%s) still get '
          'distinct artifacts (%s vs %s), and the confirmation pass does not '
          'reuse the first pass\'s path'
          % ('ok  ' if good else 'FAIL', _sanitize(a.pid), artifact_stem(a),
             artifact_stem(b)))

    jobs = min(4, default_jobs())
    tmp = tempfile.mkdtemp(prefix='bdiff-selftest.', dir=_selftest_tmproot())
    try:
        print('\nend-to-end — two synthetic compilers over a %d-program corpus, '
              'through the REAL pipeline:' % len(SELFTEST_CORPUS))

        # 1. CONTROL: identical compilers must report CLEAN, and must report
        #    having actually done the work. This is the arm that catches "the
        #    matcher matched nothing".
        n, c, res = _run_selftest_case(tmp, 'control', None, jobs)
        bad = floor_violations(['conformance'], n, min_discovered=11,
                               min_runnable=8)
        want = {'observed': 11, 'both_accepted': 10, 'both_rejected': 1,
                'exe_programs': 8, 'runnable_both': 8, 'executed': 8,
                'mismatched': 0, 'w_diagnostics': 1, 'w_warnings': 1,
                'w_exit_nonzero': 1, 'w_stdout': 8, 'w_stderr': 1}
        off = {k: (n[k], v) for k, v in want.items() if n[k] != v}
        good = not off and not bad
        ok = ok and good
        print('  [%s] control (identical compilers): 0 mismatches over 11 '
              'programs, every channel witnessed%s'
              % ('ok  ' if good else 'FAIL',
                 '' if good else '  MISMATCHED EXPECTATIONS %s' % off))
        if bad:
            for v in bad:
                print('        ' + v)

        # 1b. The flaky program must have been TRIAGED, not reported. If the
        #     confirmation step were removed this would be a false positive on
        #     every single run, which is how a differential harness gets
        #     switched off by the people who have to read it.
        good = (len(res.nondeterministic) == 1
                and 'flaky' in res.nondeterministic[0][0].pid)
        ok = ok and good
        print('  [%s] control: the deliberately nondeterministic program was '
              'triaged as nondeterministic, not reported as a mismatch (%d found)'
              % ('ok  ' if good else 'FAIL', len(res.nondeterministic)))

        # 2. KNOWN POSITIVES: one per severe class. Each must go RED, and must
        #    go red in the RIGHT class — a detector that fires for the wrong
        #    reason is not a detector.
        for variant in sorted(_SELF_VARIANTS):
            _delta, klass, want_n = _SELF_VARIANTS[variant]
            n, c, res = _run_selftest_case(tmp, variant, variant, jobs)
            good = (c[klass] == want_n and n['mismatched'] == want_n
                    and sum(c.values()) == want_n)
            ok = ok and good
            print('  [%s] known positive `%s`: expected %d %s finding(s), got '
                  '%s (total findings %d over %d program(s))'
                  % ('ok  ' if good else 'FAIL', variant, want_n, klass,
                     c[klass], sum(c.values()), n['mismatched']))

        # 3. THE FAIL-CLOSED GUARD. A run with ZERO mismatches must still FAIL
        #    when it did not measure enough. This is the guard that separates
        #    this tool from a comparator that reports clean over an empty
        #    corpus, and it is checked with an otherwise-clean run so that the
        #    mismatch count cannot be what carries the failure.
        n, c, res = _run_selftest_case(tmp, 'control', None, jobs)
        for label, kwargs, want_sub in (
                ('corpus shrank', dict(min_discovered=12, min_runnable=1),
                 'CORPUS SHRANK'),
                ('too few executed', dict(min_discovered=1, min_runnable=99),
                 'TOO FEW PROGRAMS REACHED EXECUTION')):
            bad = floor_violations(['conformance'], n, **kwargs)
            good = bool(bad) and any(want_sub in v for v in bad) \
                and n['mismatched'] == 0
            ok = ok and good
            print('  [%s] fail-closed `%s`: a 0-mismatch run is still FAILED '
                  '(%d violation(s))'
                  % ('ok  ' if good else 'FAIL', label, len(bad)))

        # 3a-bis. THE CHANNEL GUARD. A channel with zero witnesses means the
        #    comparison never ran through it, and a clean verdict must not be
        #    able to rest on it. Checked in BOTH directions, because a guard
        #    that always fires is the same defect as a matcher that never does.
        for key, label, _pop, _w in CHANNELS:
            fake = dict(n)
            fake[key] = 0
            bad = floor_violations(['conformance'], fake, min_discovered=1,
                                   min_runnable=1)
            good = any('CHANNEL NOT EXERCISED' in v and label in v for v in bad)
            ok = ok and good
            print('  [%s] fail-closed `channel: %s`: zero witnesses is a hard '
                  'failure' % ('ok  ' if good else 'FAIL', label))

        # 3b. The run-phase invariant, which is the one that cannot drift.
        fake = dict(n)
        fake['executed'] = fake['runnable_both'] - 1
        bad = floor_violations(['conformance'], fake, min_discovered=1,
                               min_runnable=1)
        good = any('RUN PHASE DROPPED' in v for v in bad)
        ok = ok and good
        print('  [%s] fail-closed `run phase dropped`: executed != '
              'runnable_both is a hard failure' % ('ok  ' if good else 'FAIL'))

        # 3c. ...and it must NOT fire when the numbers agree, or the tool is
        #     unusable. A guard that always fires is the same defect as a
        #     matcher that never fires.
        bad = floor_violations(['conformance'], n, min_discovered=1,
                               min_runnable=1)
        good = not bad
        ok = ok and good
        print('  [%s] fail-closed: the guards stay QUIET on a healthy run'
              % ('ok  ' if good else 'FAIL'))

        # 4. DISCOVERY over the REAL tree. A detector that cannot see the real
        #    corpora is broken however green its fixtures are — the same
        #    whole-tree control run check_imported_symbol_reads.py makes.
        with tempfile.TemporaryDirectory(dir=tmp) as w:
            for corpus in ('conformance', 'lit'):
                progs = DISCOVERERS[corpus](ROOT, w)
                floor = FLOORS[corpus][0]
                exe = sum(1 for p in progs if p.mode == 'exe')
                good = len(progs) >= floor and exe > 0
                ok = ok and good
                print('  [%s] real-tree discovery: %s found %d program(s) '
                      '(floor %d), %d in executable mode'
                      % ('ok  ' if good else 'FAIL', corpus, len(progs), floor,
                         exe))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print('\nbehaviour_differential --self-test: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def _selftest_tmproot():
    root = choose_workroot()
    try:
        os.makedirs(root, exist_ok=True)
        return root
    except OSError:
        return None


# ===========================================================================
def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Compare the OBSERVABLE behaviour of two bezelc builds over '
                    'the spec-conformance and lit corpora. The oracle for a '
                    'pure refactor, which ships no test that can fail.')
    ap.add_argument('--baseline', help='the reference bezelc (usually main)')
    ap.add_argument('--candidate', help='the bezelc under test')
    ap.add_argument('--corpus', default='both',
                    choices=['conformance', 'lit', 'both'])
    ap.add_argument('--jobs', type=int, default=None,
                    help='concurrent program pairs (default: '
                         'min(physical cores, RAM/1.5GiB), or '
                         'BEZEL_AOT_LINK_TOKENS)')
    ap.add_argument('--limit', type=int, default=0,
                    help='only the first N programs (a smoke run; the floors '
                         'are relaxed to match so it cannot masquerade as an '
                         'acceptance)')
    ap.add_argument('--max-report', type=int, default=25)
    ap.add_argument('--min-discovered', type=int, default=None)
    ap.add_argument('--min-runnable', type=int, default=None)
    ap.add_argument('--no-confirm', action='store_true',
                    help='skip the second observation of each mismatch. Faster, '
                         'but a reported mismatch is then NOT triaged against '
                         'corpus nondeterminism.')
    ap.add_argument('--keep', action='store_true',
                    help='keep the work directory (wrapped sources, artifacts)')
    ap.add_argument('--json', help='write the full record to this file')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)

    if a.self_test:
        return self_test()

    missing = [f for f, v in (('--baseline', a.baseline),
                              ('--candidate', a.candidate)) if not v]
    if missing:
        print('behaviour_differential: %s required (or --self-test)'
              % ' and '.join(missing), file=sys.stderr)
        return 2
    for label, path in (('baseline', a.baseline), ('candidate', a.candidate)):
        if not os.access(path, os.X_OK) or not os.path.isfile(path):
            print('behaviour_differential: %s bezelc not executable: %s'
                  % (label, path), file=sys.stderr)
            return 2
    if os.path.realpath(a.baseline) == os.path.realpath(a.candidate):
        print('behaviour_differential: WARNING — baseline and candidate are the '
              'SAME binary. This measures corpus nondeterminism, not a '
              'compiler difference.', file=sys.stderr)

    corpora = ['conformance', 'lit'] if a.corpus == 'both' else [a.corpus]
    jobs = a.jobs or default_jobs()

    workroot = choose_workroot()
    os.makedirs(workroot, exist_ok=True)
    workdir = tempfile.mkdtemp(prefix='run.', dir=workroot)

    t0 = time.time()
    print('=== behaviour differential ===')
    print('baseline : %s' % a.baseline)
    print('candidate: %s' % a.candidate)
    print('corpus   : %s' % '+'.join(corpora))
    print('jobs     : %d' % jobs)
    print('workdir  : %s' % workdir)
    print('')
    sys.stdout.flush()

    try:
        programs = []
        for corpus in corpora:
            found = DISCOVERERS[corpus](ROOT, workdir)
            print('  %-12s %d program(s), %d in executable mode'
                  % (corpus, len(found),
                     sum(1 for p in found if p.mode == 'exe')))
            programs.extend(found)
        print('')
        sys.stdout.flush()

        min_disc, min_run = a.min_discovered, a.min_runnable
        if a.limit and a.limit < len(programs):
            programs = programs[:a.limit]
            # A truncated run must not be able to pass the corpus floors — it
            # would then read as an acceptance it is not. Relax them explicitly
            # and say so, rather than letting the run fail for the wrong reason.
            print('  --limit %d: SMOKE RUN over %d program(s). The corpus floors '
                  'are relaxed, so a clean\n  result here is NOT an acceptance.'
                  % (a.limit, len(programs)))
            print('')
            if min_disc is None:
                min_disc = 1
            if min_run is None:
                min_run = 0

        res = sweep(programs, a.baseline, a.candidate, workdir, jobs,
                    confirm=not a.no_confirm)
        n, c = tally(programs, res)
        bad = floor_violations(corpora, n, min_disc, min_run)
        floors = effective_floors(corpora, min_disc, min_run)
        if a.no_confirm:
            print('NOTE: --no-confirm — mismatches below were NOT re-observed, '
                  'so corpus nondeterminism is not excluded.\n')
        rc = report(programs, res, n, c, corpora, a.max_report, a.baseline,
                    a.candidate, bad, time.time() - t0, floors)

        if a.json:
            with open(a.json, 'w', encoding='utf-8') as f:
                json.dump({
                    'baseline': a.baseline, 'candidate': a.candidate,
                    'corpora': corpora, 'counts': n, 'classes': c,
                    'channels': {k: n.get(k, 0) for k, _l, _p, _w in CHANNELS},
                    'floor_violations': bad,
                    'floors': {'min_discovered': floors[0],
                               'min_runnable': floors[1]},
                    'mismatches': [
                        {'program': p.origin, 'source': p.source,
                         'mode': p.mode,
                         'diffs': [{'class': k, 'baseline': bs,
                                    'candidate': cs} for k, bs, cs in d],
                         'baseline_obs': b.to_json(),
                         'candidate_obs': cd.to_json()}
                        for p, b, cd, d in res.rows if d],
                    'nondeterministic': [{'program': p.origin, 'why': w}
                                         for p, w, _b, _c in res.nondeterministic],
                    'harness_errors': [[p.origin, w] for p, w in res.harness_errors],
                }, f, indent=1)
            print('\njson record: %s' % a.json)
        return rc
    finally:
        if a.keep:
            print('\nwork directory kept: %s' % workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
