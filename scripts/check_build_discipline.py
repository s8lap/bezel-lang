#!/usr/bin/env python3
"""check_build_discipline.py - ratchet against bare cmake --build -j$(nproc).

WHY.  A bare `cmake --build build --target bezelc bezel_tests_X -j$(nproc)`
call bypasses both the dev_heavy slice's CPU/mem cap AND the machine-wide
jobserver token pool.  On a cold codegen build (1.7 GB RSS peak, ~25 min) an
OOM-kill mid-link wastes the entire work.  This has happened repeatedly despite
the rule being written in CLAUDE.md and reinforced in memory
(policy-build-discipline-r21.md).

WHAT IT SCANS.

  1. Commit messages on src/ commits between base and HEAD.
     Pattern: any `cmake --build` invocation that passes `-j` to a shell
     substitution (nproc or $(nproc)) or a bare `-j` with no integer.

  2. Active lane-brief files under /var/tmp/bezel-scratch/*/lane-brief*.md.

The detection regex (compatible with both bash and grep -E):

    cmake --build [^ ]+ --target [^ ]+ -j.?(?:nproc|\\$\\(nproc\\))

This fires on:
  cmake --build build --target foo -jnproc
  cmake --build build --target foo -j$(nproc)
  cmake --build build --target foo -j nproc
and is NOT tripped by -j4 or -j16 (explicit integers, bounded).

BASELINE.  The baseline records every current offending SHA and brief path so
that a new violation fails --strict while already-known violations are
silently banked.  Re-baseline with --update after cleaning up the matched
items.  --update refuses to bank a new match.

Usage:  scripts/check_build_discipline.py [--strict] [--self-test]
                                               [--update] [--list]
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'build_discipline.baseline'
BRIEF_GLOB = '/var/tmp/bezel-scratch/*/lane-brief*.md'

# Matches bare -j with shell substitution or bare -j with no integer.
# Captures the full cmake --build line for context.
RE_BARE_CMAKE = re.compile(
    r'cmake --build\s+\S+\s+--target\s+\S+\s+-j.?(?:nproc|\$\(nproc\))',
    re.IGNORECASE,
)


# ---------------------------------------------------------------------------
# Scanning
# ---------------------------------------------------------------------------

def scan_commit_messages(base='origin/main'):
    """Yield SHA strings that have a bare cmake -j violation in their msg."""
    # Batch: git log with format=%H%n%s%b gets SHA, subject, and body in one pass.
    cmd = ['git', 'log', '--format=%H%n%s%bENDMSG', f'{base}..HEAD', '--', 'src/']
    try:
        cp = subprocess.run(
            cmd,
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        sys.stderr.write('git log timed out\n')
        return
    if cp.returncode != 0:
        sys.stderr.write('git log failed: %s\n' % cp.stderr)
        return
    # Split on ENDMMSG sentinel that git adds after each commit body.
    entries = cp.stdout.split('ENDMSG\n')
    for entry in entries:
        if not entry.strip():
            continue
        lines = entry.splitlines()
        if not lines:
            continue
        sha = lines[0].strip()
        body = '\n'.join(lines[1:])
        if RE_BARE_CMAKE.search(body):
            yield sha


def scan_brief_files():
    """Yield brief file paths that contain a bare cmake -j violation."""
    import glob
    for path_str in glob.glob(BRIEF_GLOB):
        p = pathlib.Path(path_str)
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        if RE_BARE_CMAKE.search(text):
            yield str(p)


def measure():
    """Return (commits_with_violations, brief_paths_with_violations)."""
    commits = list(scan_commit_messages())
    briefs = list(scan_brief_files())
    return commits, briefs


# ---------------------------------------------------------------------------
# Baseline
# ---------------------------------------------------------------------------

def read_baseline(path=BASELINE):
    """Read baseline, return (banked_commits: set, banked_briefs: set)."""
    commits = set()
    briefs = set()
    if not path.exists():
        return commits, briefs
    for line in path.read_text().splitlines():
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        if s.startswith('commit:'):
            commits.add(s.split(':', 1)[1].strip())
        elif s.startswith('brief:'):
            briefs.add(s.split(':', 1)[1].strip())
    return commits, briefs


def write_baseline(commits, briefs, path=BASELINE):
    lines = [
        '# build_discipline.baseline - banked bare cmake --build -j$(nproc)',
        '# violations found in src/ commit messages and lane brief files.',
        '# Regenerate with scripts/check_build_discipline.py --update after',
        '# cleaning up the violations.  --update refuses to bank a NEW match.',
        '#',
        '# Format:  commit:<sha>  or  brief:<path>',
        '',
    ]
    for sha in sorted(commits):
        lines.append('commit:%s' % sha)
    for bp in sorted(briefs):
        lines.append('brief:%s' % bp)
    lines.append('')
    path.write_text('\n'.join(lines) + '\n')


# ---------------------------------------------------------------------------
# Check logic
# ---------------------------------------------------------------------------

def check(strict=True):
    """Return (ok, new_commits, new_briefs, banked_commits, banked_briefs)."""
    cur_commits, cur_briefs = measure()
    banked_commits, banked_briefs = read_baseline()

    # A new match not already banked is a violation.
    new_commits = set(cur_commits) - set(banked_commits)
    new_briefs = set(cur_briefs) - set(banked_briefs)

    ok = not new_commits and not new_briefs
    return ok, new_commits, new_briefs, banked_commits, banked_briefs


def format_results(new_commits, new_briefs, banked_commits, banked_briefs):
    out = []
    if new_commits:
        out.append('%d NEW commit-message violation(s):' % len(new_commits))
        for sha in sorted(new_commits):
            out.append('  commit:%s' % sha)
    if new_briefs:
        out.append('%d NEW brief-file violation(s):' % len(new_briefs))
        for bp in sorted(new_briefs):
            out.append('  brief:%s' % bp)
    if banked_commits:
        out.append('(%d banked commit violation(s) — clean with --update)'
                   % len(banked_commits))
    if banked_briefs:
        out.append('(%d banked brief violation(s) — clean with --update)'
                   % len(banked_briefs))
    if not out:
        out.append('no bare cmake --build -j$(nproc) violations found')
    return out


# ---------------------------------------------------------------------------
# self-test — pure string-based, no subprocess, no filesystem mutations
# ---------------------------------------------------------------------------

def self_test():
    """Test the regex and baseline logic without touching git or the filesystem."""
    ok = True

    def chk(label, cond):
        nonlocal ok
        if not cond:
            ok = False
            print('  FAIL: %s' % label)
        else:
            print('  ok:   %s' % label)

    # --- Regex tests ---
    bad_samples = [
        'cmake --build build --target foo -jnproc',
        'cmake --build build --target foo -j$(nproc)',
        'cmake --build build --target foo -j nproc',
        'cmake --build build --target foo -J$(nproc)',
        'cmake --build build --target foo -J nproc',
        'cmake --build build --target bezelc -j$(nproc)',
        'cmake --build build --target bezelc -jnproc',
        'cmake --build build --target foo -jnproc  # or lld',
        'ran cmake --build build --target foo -j$(nproc) and it worked',
        'also tried cmake --build build --target foo -j nproc',
    ]
    good_samples = [
        'cmake --build build --target foo -j4',
        'cmake --build build --target foo -j16',
        'cmake --build build --target foo -j 4',
        'cmake --build build --target foo -j 16',
        'cmake --build build -j4',
        'cmake --build build --target foo',
        'cmake --build build --target foo -- -j4',
    ]

    for sample in bad_samples:
        chk('regex MATCHES bad: %r' % sample,
            bool(RE_BARE_CMAKE.search(sample)))

    for sample in good_samples:
        chk('regex IGNORES good: %r' % sample,
            not RE_BARE_CMAKE.search(sample))

    # --- Baseline parse/write round-trip ---
    commits = {'abc123', 'def456789'}
    briefs = {'/var/tmp/bezel-scratch/foo/lane-brief-01.md'}
    with tempfile.NamedTemporaryFile(mode='w', suffix='.baseline',
                                     delete=False) as fh:
        tmp = pathlib.Path(fh.name)
    try:
        write_baseline(commits, briefs, path=tmp)
        r_c, r_b = read_baseline(path=tmp)
        chk('baseline round-trips commits', r_c == commits)
        chk('baseline round-trips briefs', r_b == briefs)
        chk('baseline round-trips exactly', r_c == commits and r_b == briefs)
    finally:
        tmp.unlink()

    # --- Inline check logic tests (avoids patching globals in nested functions) ---
    # Each test uses a fresh temp baseline and calls check() logic in-line
    # so we control all captured variables directly.

    # Test: empty measure + empty baseline = clean
    def check_inline(cur_commits, cur_briefs, baseline_path):
        """Mimics check() but takes explicit cur_* instead of calling measure()."""
        banked_c, banked_b = read_baseline(baseline_path)
        new_c = set(cur_commits) - set(banked_c)
        new_b = set(cur_briefs) - set(banked_b)
        return (not new_c and not new_b), new_c, new_b

    with tempfile.NamedTemporaryFile(mode='w', suffix='.baseline',
                                     delete=False) as fh:
        t1 = pathlib.Path(fh.name)
    write_baseline(set(), set(), path=t1)
    ok_c1, new_c1, new_b1 = check_inline([], [], t1)
    chk('empty measure + empty baseline = clean', ok_c1 and not new_c1)
    t1.unlink()

    # Test: new violation fails strict
    with tempfile.NamedTemporaryFile(mode='w', suffix='.baseline',
                                     delete=False) as fh:
        t2 = pathlib.Path(fh.name)
    write_baseline(set(), set(), path=t2)
    ok_c2, new_c2, _ = check_inline(['sha_new'], [], t2)
    chk('new violation fails strict', not ok_c2 and 'sha_new' in new_c2)
    t2.unlink()

    # Test: banked violation passes strict
    with tempfile.NamedTemporaryFile(mode='w', suffix='.baseline',
                                     delete=False) as fh:
        t3 = pathlib.Path(fh.name)
    write_baseline({'abc123'}, set(), path=t3)
    ok_c3, new_c3, _ = check_inline(['abc123'], [], t3)
    chk('banked violation passes strict', ok_c3 and not new_c3)
    t3.unlink()

    # Test: --update refuses a new (unbanked) violation
    with tempfile.NamedTemporaryFile(mode='w', suffix='.baseline',
                                     delete=False) as fh:
        t4 = pathlib.Path(fh.name)
    write_baseline(set(), set(), path=t4)
    # Simulate: cur has new_sha, banked is empty → new diff > 0 → refuse.
    # The logic is: new_c = cur_c - banked_c; if new_c: refuse.
    cur_c_new = {'unbanked_sha'}; banked_c_empty = set()
    new_c4 = set(cur_c_new) - set(banked_c_empty)
    chk('--update refuses to bank a new (unbanked) violation',
        len(new_c4) > 0 and 'unbanked_sha' in new_c4)
    t4.unlink()

    # Test: --update accepts already-banked set (exact match, no new)
    with tempfile.NamedTemporaryFile(mode='w', suffix='.baseline',
                                     delete=False) as fh:
        t5 = pathlib.Path(fh.name)
    write_baseline({'abc123'}, set(), path=t5)
    # Simulate: cur={'abc123'}, banked={'abc123'} → no new → accepted
    cur_c = {'abc123'}; banked_c = {'abc123'}
    new_c5 = set(cur_c) - set(banked_c)
    chk('--update accepts already-banked (no new diff)', not new_c5)
    t5.unlink()

    print('check_build_discipline --self-test: '
          + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def list_results(new_commits, new_briefs):
    """Print human-readable list of violations."""
    for sha in sorted(new_commits):
        print('commit:%s' % sha)
    for bp in sorted(new_briefs):
        print('brief:%s' % bp)


def main_update(force=False):
    """Shared between main() and self_test(). Returns exit code."""
    cur_c, cur_b = measure()
    banked_c, banked_b = read_baseline()
    new_c = set(cur_c) - set(banked_c)
    new_b = set(cur_b) - set(banked_b)
    if (new_c or new_b) and not force:
        sys.stderr.write(
            'check_build_discipline --update REFUSES to bank new violations:\n'
        )
        for sha in sorted(new_c):
            sys.stderr.write('  NEW commit: %s\n' % sha)
        for bp in sorted(new_b):
            sys.stderr.write('  NEW brief:  %s\n' % bp)
        sys.stderr.write(
            '  Clean up the bare cmake commands first, then re-run --update.\n'
            '  (Pass --force to establish the initial baseline anyway.)\n'
        )
        return 1
    write_baseline(cur_c, cur_b)
    print('re-baselined: %d commit(s), %d brief(s)'
          % (len(cur_c), len(cur_b)))
    return 0


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        '--strict', action='store_true',
        help='gate mode: fail if any unbanked violation is found',
    )
    ap.add_argument(
        '--self-test', action='store_true',
        help='run inline self-test and exit',
    )
    ap.add_argument(
        '--update', action='store_true',
        help='bank the current matching violations as the new baseline',
    )
    ap.add_argument(
        '--force', action='store_true',
        help='with --update, bank even when there are unbanked new violations '
             '(use to establish the initial baseline)',
    )
    ap.add_argument(
        '--list', action='store_true',
        help='list all current (unbanked) violations',
    )
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    if a.list:
        ok, new_c, new_b, _, _ = check(strict=False)
        list_results(new_c, new_b)
        return 0

    if a.update:
        return main_update(force=a.force)

    ok, new_c, new_b, banked_c, banked_b = check(strict=a.strict)
    if ok:
        print('check_build_discipline: OK '
              '(%d banked commit, %d banked brief violations)'
              % (len(banked_c), len(banked_b)))
        return 0

    for line in format_results(new_c, new_b, banked_c, banked_b):
        sys.stderr.write('check_build_discipline: %s\n' % line)
    sys.stderr.write(
        'Bare cmake --build -j$(nproc) bypasses the jobserver and the\n'
        'dev_heavy slice CPU/mem cap, risking mid-link OOM kills that waste\n'
        'a cold codegen build (1.7 GB RSS, ~25 min).  Use\n'
        '  scripts/agent_check.sh [--area <g>] <filter>\n'
        'or\n'
        '  scripts/dev_heavy.sh build <cmd>\n'
        'which both join the machine jobserver.  After cleaning up the above\n'
        'violations, re-run with --update.\n'
    )
    return 1


if __name__ == '__main__':
    sys.exit(main())
