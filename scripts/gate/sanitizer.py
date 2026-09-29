"""gate.sanitizer — the sharded sanitizer-evidence channel (#4533).

WHY THIS MODULE EXISTS
======================
`scripts/gate.sh` treats a fired generated-code sanitizer report as
DETERMINISTIC evidence of a memory error and hard-fails on it, independent of
the REAL-vs-FLAKY arbitration. It used to look for one by grepping the log it
captured from `tests/run_parallel.sh`'s OWN STDOUT — a stream a shard's PROGRAM
output never reaches (every shard is redirected into its own file, and only
`^[ FAIL ]` lines were lifted back out). The channel was inert for the whole
life of the sharded gate.

Lifting the headlines is not enough on its own, because A REPORT IS NOT
UNIFORMLY A DEFECT:

  * 37 rows across 5 distinct helper/assertion shapes (`leaksAtLeast`,
    `expectLeakButNoMemoryError`, `expectLeakNotCrash`, an inline equivalent,
    and `diagnosticMessageContains(r, "LSan/ASan at exit")` — see
    scripts/gate/sanitizer_leak_exempt.txt) ASSERT a leak and re-emit a real
    LeakSanitizer report while PASSING; a naive grep for `leaksAtLeast` alone
    finds only 11 of them (fix #4537's own investigation);
  * rows that exercise §28.2 use-after-dispose in UNCHECKED mode are running a
    program whose behaviour the spec leaves UNDEFINED — generated-code ASan
    reports it, and the row passes because it deliberately asserts nothing
    about the outcome.

So the gate needs to know WHICH TEST produced a report. That cannot be read off
the `[ RUN  ]` lines: those go to block-buffered `std::cout` while the report
goes to unbuffered fd 2, so their interleaving in a shard log has nothing to do
with execution order — an attribution attempt on the real logs named a
plausible, wrong test. `tests/test_main.cpp` therefore brackets each test body
with `[san-scope] enter/leave` markers ON fd 2 under the sweep, which puts the
markers and the reports on one ordered stream. This module reads that stream.

THE RULE IT IMPLEMENTS
======================
  * corruption (`ERROR: AddressSanitizer` — UAF / OOB / double-free / SEGV)
    in a test that FAILED        -> FATAL. Deterministic evidence; it must
                                    pre-empt the flake arbitration, which is
                                    the whole reason the phase exists.
  * corruption that is UNATTRIBUTED (no enclosing scope — e.g. the markers are
    absent, or a worker thread reported after the body returned)
                                 -> FATAL. Fail closed: an unprovable report is
                                    not an excused one.
  * corruption in a test that PASSED
                                 -> FATAL **unless** the test is declared in
                                    scripts/gate/sanitizer_exempt.txt with a
                                    reason. A row whose program's crash is the
                                    SPECIFIED outcome says so out loud, in a
                                    tracked file, and every other one reds the
                                    cycle by name.
  * a LEAK headline (`ERROR: LeakSanitizer`) in a test that FAILED, or that is
    UNATTRIBUTED, or in a test that PASSED but is not declared in
    scripts/gate/sanitizer_leak_exempt.txt
                                 -> FATAL, by the identical rule as corruption
                                    above (#4537). The 37 rows (5 distinct
                                    assertion shapes) that deliberately assert
                                    a leak declare themselves there, exactly as
                                    sanitizer_exempt.txt's corruption row
                                    declares itself; the file is separate
                                    because the rationale is different (a
                                    deliberate assertion, not a UB allowance).
                                    An undeclared leak — a real leak the row's
                                    own oracle does not see — now reds the
                                    cycle by name instead of only being
                                    counted.

`scan` runs inside tests/run_parallel.sh, over the per-shard logs, WHILE THEY
STILL EXIST (the runner reaps them on a clean exit). `verdict` runs inside
scripts/gate.sh, over the lines `scan` lifted into the gate's log. A missing
accounting line is exit 2 — "the channel is not wired", which must never read
like "the suite was clean".
"""
import argparse
import os
import re
import sys

# The report markers. This alternation is gate.sh's own, verbatim: the channel
# is being CONNECTED, not widened.
REPORT_RE = re.compile(
    r"ERROR: (?:Address|Leak)Sanitizer|aot-asan #1822\] sanitizer fired")
# An `ERROR: AddressSanitizer` headline is memory CORRUPTION. A leak report's
# headline is `ERROR: LeakSanitizer` and its only AddressSanitizer mention is
# `SUMMARY: AddressSanitizer: N byte(s) leaked`, so the two never collide here.
CORRUPTION_RE = re.compile(r"ERROR: AddressSanitizer")
# fix #4537: the LEAK-specific headline, symmetric with CORRUPTION_RE. NOT the
# same set as "REPORT_RE minus CORRUPTION_RE" — the `aot-asan #1822] sanitizer
# fired` banner line matches REPORT_RE's second alternative on EVERY report
# (leak or corruption alike) but is neither headline itself, so deriving
# "leak" as markers-corruption double-counts the banner as a leak on a
# corruption-only report. Measured: a bare CORRUPTION fixture scans as
# markers=2 (banner + headline), corruption=1 — markers-corruption=1 would
# read as one undeclared leak on a report that has none.
LEAK_RE = re.compile(r"ERROR: LeakSanitizer")

ENTER_RE = re.compile(r"^\[san-scope\] enter (\S+)$")
LEAVE_RE = re.compile(r"^\[san-scope\] leave (\S+) (pass|fail)$")

# LOAD-BEARING LITERALS: scripts/gate.sh and tests/run_parallel.sh both key on
# these, and scripts/gate/tests/test_sanitizer_evidence.py pins them.
EVIDENCE_PREFIX = "SANITIZER EVIDENCE (all shards):"
SCOPE_PREFIX = "SANITIZER SCOPE:"
EXCERPT_HEADER = "--- sanitizer reports ---"

EVIDENCE_RE = re.compile(
    r"^" + re.escape(EVIDENCE_PREFIX) +
    r" (?P<hits>\d+) report marker\(s\) in (?P<files>\d+) of "
    r"(?P<scanned>\d+) shard log\(s\) scanned, (?P<corrupt>\d+) corruption "
    r"headline\(s\); attributed (?P<failing>\d+) failing, (?P<passing>\d+) "
    r"passing, (?P<unattributed>\d+) unattributed; "
    r"(?P<leak_unattributed>\d+) leak report\(s\) unattributed$")
SCOPE_LINE_RE = re.compile(
    r"^" + re.escape(SCOPE_PREFIX) +
    r" (?P<test>\S+) (?P<outcome>pass|fail) markers=(?P<markers>\d+) "
    r"corruption=(?P<corruption>\d+) leak=(?P<leak>\d+)$")


class Scan(object):
    """What one sweep over a run's per-shard logs found."""

    def __init__(self):
        self.scanned = 0          # shard logs read
        self.files = 0            # shard logs holding >= 1 report marker
        self.hits = 0             # report-marker lines
        self.corrupt = 0          # corruption headline lines
        self.leak = 0             # leak headline lines (parallels self.corrupt)
        self.by_test = {}         # "suite.name" -> {outcome, markers, corruption, leak}
        self.unattributed = 0     # corruption lines with no enclosing scope
        # fix #4537: LEAK_RE headline lines with no enclosing scope. A
        # SEPARATE counter from `unattributed`, not folded into it — the
        # ScannerTest invariant `sum(attributed()) == corrupt` would otherwise
        # break the moment a leak line landed here too.
        self.unattributed_leaks = 0
        self.excerpts = []        # (basename, [lines]) — bounded, for the reader

    # Counts of CORRUPTION by attribution; the three sum to self.corrupt.
    def attributed(self):
        failing = sum(r["corruption"] for r in self.by_test.values()
                      if r["outcome"] == "fail")
        passing = sum(r["corruption"] for r in self.by_test.values()
                      if r["outcome"] == "pass")
        return failing, passing, self.unattributed


def _record(scan, test, outcome, markers, corruption, leak):
    if not markers and not corruption and not leak:
        return
    row = scan.by_test.setdefault(
        test, {"outcome": outcome, "markers": 0, "corruption": 0, "leak": 0})
    # A test that failed anywhere is reported as failed: the FATAL rule must not
    # be dodged by a second, passing occurrence of the same name in another shard.
    if outcome == "fail":
        row["outcome"] = "fail"
    row["markers"] += markers
    row["corruption"] += corruption
    row["leak"] += leak


def _flush_unattributed(scan, pending):
    """`pending` is [markers, corruption, leak] for a scope that never reached
    a matching `leave` (crashed shard, or a `leave` with a mismatched name).
    Both the corruption remainder AND the leak remainder become unattributed
    — losing either is the one outcome not allowed (#4533, #4537)."""
    if pending[1]:
        scan.unattributed += pending[1]
    if pending[2]:
        scan.unattributed_leaks += pending[2]


def scan_log(scan, path):
    """Read one shard log, folding its findings into `scan`.

    Streamed line by line: a shard log is megabytes and there are dozens of
    them. `errors="replace"` because a test program's stderr is arbitrary
    bytes. Returns True if the log held at least one report marker.
    """
    cur = None             # the test currently open, or None
    pending = [0, 0, 0]    # [markers, corruption, leak] seen since `enter`
    hits_here = 0
    try:
        fh = open(path, "r", errors="replace")
    except OSError:
        return False
    with fh:
        for line in fh:
            line = line.rstrip("\n")
            m = ENTER_RE.match(line)
            if m:
                # An UNCLOSED scope (the previous test never reached `leave` —
                # it crashed the shard) keeps its reports rather than silently
                # dropping them: they become unattributed, which is FATAL.
                if cur is not None:
                    _flush_unattributed(scan, pending)
                cur, pending = m.group(1), [0, 0, 0]
                continue
            m = LEAVE_RE.match(line)
            if m:
                name, outcome = m.group(1), m.group(2)
                if cur is not None and name == cur:
                    _record(scan, name, outcome, pending[0], pending[1], pending[2])
                elif cur is not None:
                    _flush_unattributed(scan, pending)
                cur, pending = None, [0, 0, 0]
                continue
            if not REPORT_RE.search(line):
                continue
            hits_here += 1
            scan.hits += 1
            is_corrupt = bool(CORRUPTION_RE.search(line))
            is_leak = bool(LEAK_RE.search(line))
            if is_corrupt:
                scan.corrupt += 1
            if is_leak:
                scan.leak += 1
            if cur is None:
                # No enclosing scope: the markers are off (the plain oracle), or
                # a worker thread reported after the body returned. The banner
                # line alone (neither corrupt nor leak) carries no evidence to
                # lose, so only a real headline needs to become unattributed.
                if is_corrupt:
                    scan.unattributed += 1
                elif is_leak:
                    scan.unattributed_leaks += 1
            else:
                pending[0] += 1
                pending[1] += 1 if is_corrupt else 0
                pending[2] += 1 if is_leak else 0
        if cur is not None:
            _flush_unattributed(scan, pending)
    return hits_here > 0


def _has_corruption(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return any(CORRUPTION_RE.search(line) for line in fh)
    except OSError:
        return False


def _excerpt(path, budget=20):
    """`budget` lines from the first CORRUPTION marker, else the first marker
    of any kind. Mirrors the old `grep -m1 -A18`: enough of the report to act
    on, bounded so a run with hundreds cannot bury the rest of the gate log."""
    pat = CORRUPTION_RE if _has_corruption(path) else REPORT_RE
    out, arming = [], False
    try:
        fh = open(path, "r", errors="replace")
    except OSError:
        return out
    with fh:
        for line in fh:
            line = line.rstrip("\n")
            if not arming and pat.search(line):
                arming = True
            if arming:
                out.append(line)
                if len(out) >= budget:
                    break
    return out


def scan_logdir(logdir, excerpt_logs=3):
    """Scan every `shard.*.log` in `logdir`. Never raises on a bad file."""
    scan = Scan()
    try:
        names = sorted(n for n in os.listdir(logdir)
                       if n.startswith("shard.") and n.endswith(".log"))
    except OSError:
        names = []
    hit_paths = []
    for name in names:
        path = os.path.join(logdir, name)
        scan.scanned += 1
        if scan_log(scan, path):
            scan.files += 1
            hit_paths.append(path)
    # Corruption-bearing logs LEAD the bounded excerpt: on real data leak
    # reports outnumber corruption ones by two orders of magnitude (601 markers
    # vs 7 corruption headlines in the kept logs of the failed e89577e4 cycle),
    # and the double-free must still be what the operator reads.
    hit_paths.sort(key=lambda p: (not _has_corruption(p), p))
    scan.excerpts = [(os.path.basename(p), _excerpt(p))
                     for p in hit_paths[:excerpt_logs]]
    return scan


def format_scan(scan):
    """The lines tests/run_parallel.sh prints. Line 1 is UNCONDITIONAL: `0
    reports` and `the channel is not wired` are indistinguishable to a grep
    that returns 0, which is the defect #4533 reports, so the count of shard
    logs actually SCANNED travels with the count of reports."""
    failing, passing, unattributed = scan.attributed()
    out = ["%s %d report marker(s) in %d of %d shard log(s) scanned, "
           "%d corruption headline(s); attributed %d failing, %d passing, "
           "%d unattributed; %d leak report(s) unattributed"
           % (EVIDENCE_PREFIX, scan.hits, scan.files, scan.scanned,
              scan.corrupt, failing, passing, unattributed,
              scan.unattributed_leaks)]
    for test in sorted(scan.by_test):
        row = scan.by_test[test]
        out.append("%s %s %s markers=%d corruption=%d leak=%d"
                   % (SCOPE_PREFIX, test, row["outcome"], row["markers"],
                      row["corruption"], row["leak"]))
    if scan.hits:
        out.append(EXCERPT_HEADER)
        for name, lines in scan.excerpts:
            out.append("==> %s <==" % name)
            out.extend(lines)
        extra = scan.files - len(scan.excerpts)
        if extra > 0:
            out.append("... and %d further shard log(s) with report markers"
                       % extra)
    return out


def read_exempt(path):
    """`suite.name  # reason` per line; blank lines and `#` comments ignored.

    Returns {name: reason}. A MISSING FILE IS AN EMPTY SET, never an error —
    an unreadable exemption list must make the gate stricter, not looser.
    """
    out = {}
    try:
        fh = open(path, "r", errors="replace")
    except OSError:
        return out
    with fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, _, reason = line.partition("#")
            name = name.strip()
            if name:
                out[name] = reason.strip()
    return out


def parse_lifted(text):
    """Read `scan`'s output back out of the gate's captured log.

    Returns (evidence_dict, scope_rows) or (None, []) when the accounting line
    is absent — the caller must treat that as "channel unwired", not "clean".
    """
    ev, rows = None, []
    for line in text.splitlines():
        line = line.rstrip()
        m = EVIDENCE_RE.match(line)
        if m and ev is None:
            ev = {k: int(v) for k, v in m.groupdict().items()}
            continue
        m = SCOPE_LINE_RE.match(line)
        if m:
            d = m.groupdict()
            rows.append({"test": d["test"], "outcome": d["outcome"],
                         "markers": int(d["markers"]),
                         "corruption": int(d["corruption"]),
                         "leak": int(d["leak"])})
    return ev, rows


UNWIRED = 2
FATAL = 1
OK = 0


def verdict(ev, rows, exempt, leak_exempt=None):
    """(rc, lines) — the gate's decision over one lifted scan.

    `exempt` declares CORRUPTION rows whose crash is the spec's SPECIFIED
    outcome (scripts/gate/sanitizer_exempt.txt). `leak_exempt` declares LEAK
    rows that deliberately ASSERT a leak (scripts/gate/sanitizer_leak_exempt.txt,
    #4537) — a different rationale, so a separate list; a test can appear in
    both, one, or neither. Both are applied by the identical rule: fatal in a
    failing test regardless of declaration, fatal in a passing test unless
    declared, allowed when declared.
    """
    leak_exempt = leak_exempt or {}
    if ev is None:
        return UNWIRED, ["no %s accounting line — the sanitizer-evidence "
                         "channel is not wired, and a zero from an unwired "
                         "channel is not a clean suite (#4533)"
                         % EVIDENCE_PREFIX]
    if ev["scanned"] == 0:
        return UNWIRED, ["the sanitizer-evidence scan read 0 shard logs, so "
                         "its zero means nothing (#4533)"]
    lines, fatal = [], False
    for row in sorted(rows, key=lambda r: r["test"]):
        if row["corruption"]:
            if row["outcome"] == "fail":
                fatal = True
                lines.append("FATAL  %s — %d corruption headline(s) in a "
                             "FAILING test: deterministic evidence, not "
                             "arbitrable" % (row["test"], row["corruption"]))
            elif row["test"] in exempt:
                lines.append("allowed %s — %d corruption headline(s) in a "
                             "passing test, declared in sanitizer_exempt.txt: %s"
                             % (row["test"], row["corruption"],
                                exempt[row["test"]] or "(no reason recorded)"))
            else:
                fatal = True
                lines.append("FATAL  %s — %d corruption headline(s) in a "
                             "PASSING test, not declared in "
                             "sanitizer_exempt.txt. Either it is a "
                             "memory-safety defect the row's oracle does not "
                             "see, or the row deliberately runs an undefined "
                             "program and must say so in that file."
                             % (row["test"], row["corruption"]))
        # fix #4537: a LEAK headline on this row (LEAK_RE, not merely
        # "markers minus corruption" — the banner line matches REPORT_RE on
        # every report and is neither headline) is fatal by the identical
        # rule, against the SEPARATE leak_exempt list.
        leaks_here = row["leak"]
        if leaks_here:
            if row["outcome"] == "fail":
                fatal = True
                lines.append("FATAL  %s — %d leak report(s) in a FAILING "
                             "test: deterministic evidence, not arbitrable "
                             "(#4537)" % (row["test"], leaks_here))
            elif row["test"] in leak_exempt:
                lines.append("allowed %s — %d leak report(s) in a passing "
                             "test, declared in sanitizer_leak_exempt.txt: %s"
                             % (row["test"], leaks_here,
                                leak_exempt[row["test"]] or
                                "(no reason recorded)"))
            else:
                fatal = True
                lines.append("FATAL  %s — %d leak report(s) in a PASSING "
                             "test, not declared in "
                             "sanitizer_leak_exempt.txt. Either it is a real "
                             "leak the row's oracle does not see, or the row "
                             "deliberately asserts one and must say so in "
                             "that file (#4537)." % (row["test"], leaks_here))
    if ev["unattributed"]:
        fatal = True
        lines.append("FATAL  %d corruption headline(s) could not be attributed "
                     "to any test (no enclosing [san-scope] marker). An "
                     "unprovable report is not an excused one."
                     % ev["unattributed"])
    if ev.get("leak_unattributed"):
        fatal = True
        lines.append("FATAL  %d leak report(s) could not be attributed to "
                     "any test (no enclosing [san-scope] marker). An "
                     "unprovable report is not an excused one (#4537)."
                     % ev["leak_unattributed"])
    return (FATAL if fatal else OK), lines


def main(argv=None):
    ap = argparse.ArgumentParser(prog="gate.sanitizer")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("scan", help="scan a run_parallel logdir")
    s.add_argument("logdir")

    v = sub.add_parser("verdict", help="decide over a lifted scan")
    v.add_argument("--shard-log", required=True)
    v.add_argument("--exempt", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "sanitizer_exempt.txt"))
    # fix #4537: a SEPARATE declared-exemption list for deliberate LEAK
    # assertions (leaksAtLeast rows) — a different rationale from the
    # UB-corruption list above, so a different file. gate.sh's
    # gate_sanitizer_verdict() never passes either --exempt flag, so both
    # defaults (computed here, next to this script) are what the gate
    # actually consults.
    v.add_argument("--leak-exempt", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "sanitizer_leak_exempt.txt"))

    args = ap.parse_args(argv)
    if args.cmd == "scan":
        for line in format_scan(scan_logdir(args.logdir)):
            print(line)
        return 0
    try:
        with open(args.shard_log, "r", errors="replace") as fh:
            text = fh.read()
    except OSError as exc:
        print("cannot read %s: %s" % (args.shard_log, exc), file=sys.stderr)
        return UNWIRED
    ev, rows = parse_lifted(text)
    rc, lines = verdict(ev, rows, read_exempt(args.exempt),
                        read_exempt(args.leak_exempt))
    for line in lines:
        print(line)
    return rc


if __name__ == "__main__":
    sys.exit(main())
