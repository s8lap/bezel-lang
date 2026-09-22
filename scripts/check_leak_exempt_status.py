#!/usr/bin/env python3
"""check_leak_exempt_status.py - ratchet the status: field on every
scripts/gate/sanitizer_leak_exempt.txt row (#4557).

WHY THIS EXISTS. sanitizer_leak_exempt.txt declares tests whose generated-code
sanitizer LEAK report is a deliberate assertion (#4537) - the gate hard-fails
an UNDECLARED leak, so every deliberate one has to live here. Measured
2026-09-21: 38 rows, 19 citing no issue at all, and of the 15 distinct issues
cited, 13 CLOSED - rows whose fix landed on the CENTRAL case but left a
residual axis leaking, where the residual axis's own tracker was lost the
moment the issue closed (#4557). The file was load-bearing documentation with
no owner and no structure separating "this is a deliberate, permanent
leak-over-crash/UAF trade" from "this is a fix nobody has scoped out yet".

THE FIX (#4557's own proposal): every row's reason must start with a required
`status:` token -

    status:trade    a DELIBERATE, permanent leak-vs-crash/leak-vs-UAF boundary
                     or spec-mandated exclusion. No issue: required - the
                     row's own prose is the record (it will typically still
                     MENTION an issue number for context, e.g. the fix whose
                     control this row is, but that is not a claim the leak
                     itself is tracked there).
    status:residue   scoped OUT of an already-landed fix; a fix is still owed
                     and tracked by its OWN open issue. Requires issue:NNNN.
    status:defect    a leak nobody has scoped out on purpose yet. Requires
                     issue:NNNN.

WHAT THIS SCRIPT CHECKS, IN TWO HALVES, DELIBERATELY SEPARATE.

  1. STRUCTURE (always, offline, gate-wired): every row has a status: token
     with a valid value; every defect/residue row has a well-formed issue:
     token. This alone stops the file regressing to "no owner" - a new row
     with no status fails the very next gate, by name.

  2. ISSUE STATE (only against the checked-in snapshot
     scripts/gate/leak_exempt_issue_state.tsv, still offline): a defect/
     residue row's issue: must appear in the snapshot and be OPEN there. This
     is what actually catches "the tracking issue closed and nobody noticed"
     - #4557's own headline finding - but it can only ever be as fresh as the
     snapshot. The snapshot is refreshed by a SEPARATE, explicitly-manual
     companion script, scripts/check_leak_exempt_issue_state.sh (gh-based,
     same non-gated-audit shape as the pre-existing scripts/check_issue_refs.sh)
     - NOT this script and NOT anything gate_full_cycle.sh runs, because the
     gate is deliberately hermetic (no network call anywhere in it; verified
     2026-09-22) and a live `gh` call on every cycle would make the gate
     flaky on network/auth, which is a worse failure mode than a snapshot
     that is a few days stale. Re-run the companion periodically (or whenever
     #4557-style staleness is suspected) and commit the refreshed snapshot in
     the same commit, exactly like every other baseline file in this repo.

--self-test proves both halves detect what they claim to, with a synthetic
exempt file + snapshot, before trusting a green on the real tree - the same
"a green from a matcher that matched nothing is worth nothing" precedent
every sibling ratchet in gate_full_cycle.sh follows.

Usage: scripts/check_leak_exempt_status.py [--strict] [--self-test] [--list]
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXEMPT_PATH = ROOT / 'scripts' / 'gate' / 'sanitizer_leak_exempt.txt'
SNAPSHOT_PATH = ROOT / 'scripts' / 'gate' / 'leak_exempt_issue_state.tsv'

VALID_STATUSES = ('defect', 'trade', 'residue')
NEEDS_ISSUE = ('defect', 'residue')

# The reason string (everything after the row's first `#`) must OPEN with
# `status:<word>`, optionally followed by `issue:<digits>` - both anchored at
# the start so a row cannot bury the field mid-prose where a reader (or a
# looser regex) would miss it.
_LEAD_RE = re.compile(
    r'^\s*status:(?P<status>\S+)(?:\s+issue:(?P<issue>\d+))?\b')


def parse_rows(path):
    """[(name, status_or_None, issue_or_None, reason, lineno), ...].

    A MISSING file yields an empty list - matching gate.sanitizer.read_exempt's
    own "missing is an empty set, never an error" rule (losing the file makes
    the gate stricter, never looser); it is this script's job to fail loudly
    when the file legitimately needs structure it does not have, not to
    invent a requirement that the file must exist at all.
    """
    rows = []
    try:
        fh = open(path, 'r', errors='replace')
    except OSError:
        return rows
    with fh:
        for lineno, line in enumerate(fh, 1):
            s = line.strip()
            if not s or s.startswith('#'):
                continue
            name, _, reason = line.rstrip('\n').partition('#')
            name = name.strip()
            if not name:
                continue
            m = _LEAD_RE.match(reason)
            status = m.group('status') if m else None
            issue = int(m.group('issue')) if (m and m.group('issue')) else None
            rows.append((name, status, issue, reason.strip(), lineno))
    return rows


def read_snapshot(path):
    """{issue_number: 'OPEN'|'CLOSED'}. Raises SystemExit(2) if unreadable -
    a caller that needs the snapshot must fail closed, not silently skip the
    issue-state half (that is indistinguishable from "everything is fine")."""
    try:
        text = path.read_text(errors='replace')
    except OSError:
        sys.stderr.write('missing snapshot: %s\n' % path)
        raise SystemExit(2)
    out = {}
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        parts = s.split('\t')
        if len(parts) != 2:
            parts = s.split()
        if len(parts) != 2:
            sys.stderr.write('%s: cannot parse row %r\n' % (path, s))
            raise SystemExit(2)
        num_s, state = parts
        try:
            num = int(num_s)
        except ValueError:
            sys.stderr.write('%s: bad issue number %r\n' % (path, num_s))
            raise SystemExit(2)
        out[num] = state.strip().upper()
    return out


def validate(rows, snapshot_path=SNAPSHOT_PATH):
    """(ok, messages, counts). counts = {'total': N, 'defect': .., ...}."""
    counts = {'total': len(rows), 'defect': 0, 'trade': 0, 'residue': 0}
    bad = []
    needs_snapshot = False

    for name, status, issue, reason, lineno in rows:
        if status is None:
            bad.append(
                'line %d: %s has no status: token as the first word of its '
                'reason (got %r) - every row must open with '
                'status:{defect,trade,residue}' % (lineno, name, reason[:60]))
            continue
        if status not in VALID_STATUSES:
            bad.append(
                'line %d: %s has status:%s, not one of %s'
                % (lineno, name, status, '/'.join(VALID_STATUSES)))
            continue
        counts[status] += 1
        if status in NEEDS_ISSUE:
            if issue is None:
                bad.append(
                    'line %d: %s is status:%s, which requires an issue:NNNN '
                    'token naming its OPEN tracker (a %s leak with no '
                    'tracker is exactly the #4557 gap) - reason: %r'
                    % (lineno, name, status, status, reason[:80]))
            else:
                needs_snapshot = True

    if needs_snapshot:
        snapshot = read_snapshot(snapshot_path)
        for name, status, issue, reason, lineno in rows:
            if status not in NEEDS_ISSUE or issue is None:
                continue
            state = snapshot.get(issue)
            if state is None:
                bad.append(
                    'line %d: %s cites issue:%d, which is not in %s - '
                    'refresh the snapshot (scripts/check_leak_exempt_issue_'
                    'state.sh) and re-commit it'
                    % (lineno, name, issue, snapshot_path.name))
            elif state != 'OPEN':
                bad.append(
                    'line %d: %s is status:%s citing issue:%d, which the '
                    'snapshot marks %s. Either the leak is actually fixed '
                    '(verify and remove the row WITH EVIDENCE, never '
                    'silently), or it needs a fresh OPEN tracker (re-file '
                    'and update issue:), or it is really a deliberate '
                    'boundary (reclassify status:trade with reasoning) - '
                    'see #4557' % (lineno, name, status, issue, state))

    return (not bad), bad, counts


def format_counts(counts):
    return ('%d row(s): %d defect, %d trade, %d residue'
            % (counts['total'], counts['defect'], counts['trade'],
               counts['residue']))


def _list(rows):
    for name, status, issue, reason, lineno in rows:
        tag = status or 'MISSING'
        isu = ('#%d' % issue) if issue else '-'
        print('%-9s %-6s %s' % (tag, isu, name))


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _mk_exempt(td, rows_text):
    p = pathlib.Path(td) / 'exempt.txt'
    p.write_text(rows_text)
    return p


def _mk_snapshot(td, entries):
    p = pathlib.Path(td) / 'snapshot.tsv'
    p.write_text('\n'.join('%d\t%s' % (n, s) for n, s in entries) + '\n')
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
        # A row with no status: token at all - the exact #4557 shape.
        p = _mk_exempt(td, 'suite.no_status  # #1234: some old-style row\n')
        rows = parse_rows(p)
        good, msgs, counts = validate(rows, _mk_snapshot(td, []))
        chk('a row with no status: FAILS', not good)
        chk('the message names the missing-status row',
            any('no status:' in m for m in msgs))

        # An invalid status value.
        p = _mk_exempt(td, 'suite.bad_status  # status:whatever reason\n')
        good, msgs, counts = validate(parse_rows(p), _mk_snapshot(td, []))
        chk('an invalid status: value FAILS', not good)

        # status:trade needs no issue and no snapshot at all.
        p = _mk_exempt(
            td, 'suite.trade_row  # status:trade spec \xc2\xa728.1: '
                'deliberately does not free the caller copy\n')
        good, msgs, counts = validate(parse_rows(p), _mk_snapshot(td, []))
        chk('a bare status:trade row (no issue:, no snapshot needed) PASSES',
            good and not msgs)
        chk('counts a trade row', counts == {'total': 1, 'defect': 0,
                                              'trade': 1, 'residue': 0})

        # status:defect with NO issue: - the required-field violation.
        p = _mk_exempt(td, 'suite.defect_no_issue  # status:defect a leak\n')
        good, msgs, _ = validate(parse_rows(p), _mk_snapshot(td, []))
        chk('status:defect with no issue: FAILS', not good)
        chk('the message says a tracker is required',
            any('requires an issue' in m for m in msgs))

        # status:residue citing an issue NOT in the snapshot.
        p = _mk_exempt(
            td, 'suite.residue_unknown  # status:residue issue:9999 x\n')
        good, msgs, _ = validate(parse_rows(p), _mk_snapshot(td, []))
        chk('status:residue citing an issue absent from the snapshot FAILS',
            not good)
        chk('the message says to refresh the snapshot',
            any('refresh the snapshot' in m for m in msgs))

        # status:defect citing a CLOSED issue - the #4557 headline defect.
        p = _mk_exempt(
            td, 'suite.defect_closed  # status:defect issue:4253 leaks\n')
        good, msgs, _ = validate(parse_rows(p),
                                  _mk_snapshot(td, [(4253, 'CLOSED')]))
        chk('status:defect citing a CLOSED issue FAILS (#4557 headline shape)',
            not good)
        chk('the message names the CLOSED state',
            any('CLOSED' in m for m in msgs))

        # status:residue citing an OPEN issue - the correct shape.
        p = _mk_exempt(
            td, 'suite.residue_open  # status:residue issue:3541 leak\n')
        good, msgs, counts = validate(parse_rows(p),
                                       _mk_snapshot(td, [(3541, 'OPEN')]))
        chk('status:residue citing an OPEN issue PASSES', good and not msgs)
        chk('counts a residue row', counts['residue'] == 1)

        # Mixed file: one clean defect row + one violation - counts must
        # still total correctly and the ok bit must be the AND of all rows.
        p = _mk_exempt(
            td,
            'suite.a  # status:defect issue:100 x\n'
            'suite.b  # status:trade y\n'
            'suite.c  # status:residue issue:200 z\n')
        good, msgs, counts = validate(
            parse_rows(p),
            _mk_snapshot(td, [(100, 'OPEN'), (200, 'CLOSED')]))
        chk('a mixed file fails when ANY row fails', not good)
        chk('a mixed file still counts every row',
            counts == {'total': 3, 'defect': 1, 'trade': 1, 'residue': 1})
        chk('only the CLOSED-citing row is named',
            any('suite.c' in m for m in msgs)
            and not any('suite.a' in m for m in msgs))

        # A file needing NO snapshot lookups (all trade) must not require the
        # snapshot to exist at all - a missing snapshot must fail closed only
        # when something actually needs it.
        p = _mk_exempt(td, 'suite.only_trade  # status:trade fine\n')
        missing_snapshot = pathlib.Path(td) / 'does-not-exist.tsv'
        good, msgs, _ = validate(parse_rows(p), missing_snapshot)
        chk('an all-trade file needs no snapshot and PASSES',
            good and not msgs)

        # But a defect/residue row DOES need the snapshot to exist - fail
        # closed, never silently skip the issue-state half.
        p = _mk_exempt(td, 'suite.needs_it  # status:defect issue:1 x\n')
        try:
            validate(parse_rows(p), missing_snapshot)
            chk('a missing-but-needed snapshot fails closed', False)
        except SystemExit as e:
            chk('a missing-but-needed snapshot fails closed', e.code == 2)

        # A missing exempt file is an empty set (matches
        # gate.sanitizer.read_exempt's own rule) - not an error.
        rows = parse_rows(pathlib.Path(td) / 'nope.txt')
        chk('a missing exempt file parses as zero rows (never an error)',
            rows == [])

    print('check_leak_exempt_status --self-test: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--strict', action='store_true',
                     help='gate mode: any violation is a nonzero exit')
    ap.add_argument('--self-test', action='store_true',
                     help='prove the detector still detects')
    ap.add_argument('--list', action='store_true',
                     help='list every row with its status/issue')
    ap.add_argument('--exempt', default=str(EXEMPT_PATH),
                     help='path to sanitizer_leak_exempt.txt (default: repo)')
    ap.add_argument('--snapshot', default=str(SNAPSHOT_PATH),
                     help='path to the issue-state snapshot (default: repo)')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    rows = parse_rows(pathlib.Path(a.exempt))

    if a.list:
        _list(rows)
        return 0

    good, msgs, counts = validate(rows, pathlib.Path(a.snapshot))

    if good:
        print('check_leak_exempt_status: OK (%s)' % format_counts(counts))
        return 0

    print('check_leak_exempt_status: %d violation(s) in %s (%s)\n'
          % (len(msgs), EXEMPT_PATH.name, format_counts(counts)))
    for m in msgs:
        print('  ' + m)
    print('\nSee scripts/gate/sanitizer_leak_exempt.txt\'s own header (#4557) '
          'for the status: field contract.')
    return 1 if a.strict else 0


if __name__ == '__main__':
    sys.exit(main())
