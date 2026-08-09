"""arbitration.py — the flake arbitration (Phase 4 module, #1861 / #2100).

A faithful Python port of gate.sh:432-524 (`arbitrate` + `arb_run_suite`). The
sharded pass is the authoritative oracle (#1861): every test is process-isolated,
so a suspect that PASSES ALONE within 5 tries is a sharding flake (pollution /
timing) and the gate continues; a suspect that FAILS alone all 5 tries is a real
regression. This re-runs the suspect's SUITE across every per-area binary.

FIVE INVARIANTS this port preserves verbatim (each pinned by test_arbitration.py;
the gate-E false-green is what made each one load-bearing):

  1. ran-nothing is NOT a pass. A re-run suspect that exits 0 having run 0 tests
     validates NOTHING (a mapping bug / rename would otherwise exit 0 and be
     laundered into a flake-ACCEPT — the exact false-green this gate exists to
     prevent). The `total > 0` guard is the second half of "passes alone".

  2. [ FAIL ] suites are arbitrated IN FULL; only `died on:` (timeout-victim)
     suites are subject to the sample cap, and only AFTER every failing suite is
     checked. gate E sampled 8 of 47 alphabetically-first suspects and eleven
     deterministic ownership failures (own_branch_disposal.* etc.) were never
     re-run and the gate PASSED red. Never sample away a [ FAIL ]. (#2100)

  3. `died on: <none>` is STRIPPED. `<none>` is a shard killed before running
     ANY test (OOM-kill at startup); re-running the literal filter "<none>"
     matches nothing, "passes", and would launder a dropped whole slice into a
     flake-ACCEPT. Stripping it degrades the case to return 2 (marker-absent ->
     caller treats the result as untrustworthy). (#2100)

  4. The return contract (0 / 1 / 2) is never collapsed:
       0  every sampled suspect passed alone (flake, continue);
       1  a suspect fails all 5 tries (real regression);
       2  nonzero exit but NO crash/FAIL marker in the log (undercount/timeout —
          caller treats as untrustworthy, NEVER as a pass).
     gate_full_cycle distrusts rc=0+undercount precisely because collapsing 2
     into 0 is the #3001 shape.

  5. ARB_ENV forces ASan during arbitration in ASan modes, so an ASan-only
     failure reproduces rather than being falsely cleared as flaky. The runner
     merges arb_env into the subprocess environment alongside BEZEL_TEST_FILTER.

This is a FOUNDATION module (Wave 1): written + unit-tested, NOT yet wired into
gate.sh (Wave 2 replaces gate.sh:432-524 with a call here). gate.sh is untouched
on this lane. NOTE: porting arb_run_suite SUBSUMES the Phase-2c legacy-grep site
at gate.sh:451 (the `^[0-9]+ passed, [0-9]+ failed` harvest) — the grep moves
into _parse_summary here.

Stdlib only (no pytest). See test_arbitration.py for the load-bearing pins.
"""

import os
import re
import subprocess
from dataclasses import dataclass, field

# The two gtest summary shapes the harvest must read. Faithful to gate.sh:451
# `grep -oE '^[0-9]+ passed, [0-9]+ failed'`: anchored to the start of a line,
# captures the passed/failed counts. The bash takes the LAST match (tail -1).
_SUMMARY_RE = re.compile(r"^(\d+) passed, (\d+) failed", re.MULTILINE)

# The shard-log markers `arbitrate` keys off (gate.sh:459-460).
_DIED_ON_RE = re.compile(r"died on:\s*(.*)$")
_FAIL_LINE_RE = re.compile(r"^\[ FAIL \]\s*(\S+)")

DEFAULT_SAMPLE_CAP = 8       # per #2100: ARB_SAMPLE default in gate.sh:502
DEFAULT_RETRY = 5            # per #1861: 1..5 tries in gate.sh:509
DEFAULT_SUITE_TIMEOUT = 180  # per gate.sh:449 `timeout 180`

# Arbitration verdict codes — the return contract of arbitrate() (invariant 4).
ARB_FLAKY = 0          # every sampled suspect passed alone — continue
ARB_REAL = 1           # a suspect failed all 5 tries — real regression
ARB_UNTRUSTWORTHY = 2  # nonzero exit, no crash/FAIL marker — caller distrusts


@dataclass
class ArbResult:
    """The arbitration decision plus the evidence a caller/banner needs.

    `verdict` is the 0/1/2 contract (invariant 4). `messages` are the FLAKY /
    REAL / cap lines gate.sh echoes (reproduced verbatim for the Wave-2
    equivalence proof). `last_suspects` mirrors ARB_LAST_SUSPECTS — the count of
    UNIQUE test-level suspects (crashed + failed, minus <none>) for the timing
    record. The parsed suite lists are exposed so a test can assert exactly
    which class was arbitrated and which was capped.
    """
    verdict: int
    messages: list = field(default_factory=list)
    last_suspects: int = 0
    fail_suites: list = field(default_factory=list)   # arbitrated IN FULL
    crash_suites: list = field(default_factory=list)  # subject to the cap
    sampled: list = field(default_factory=list)       # suites actually re-run


def _suite_name(test_name):
    """Everything before the first `.` — the gtest suite. Matches `sed 's/\\..*//'`."""
    return test_name.split(".", 1)[0]


def parse_crashed(log_text):
    """Extract the `died on: <victim>` test names from a shard log.

    Faithful to gate.sh:459 `grep 'died on:' | sed -E 's/.*died on: //'`: each
    matching line contributes the text AFTER `died on: `. A victim may be the
    literal `<none>` (a shard killed at startup) — kept here; the caller strips
    it (invariant 3).
    """
    out = []
    for line in log_text.splitlines():
        m = _DIED_ON_RE.search(line)
        if m:
            out.append(m.group(1).strip())
    return out


def parse_failed(log_text):
    """Extract the `[ FAIL ] <test>` first-token test names from a shard log.

    Faithful to gate.sh:460 `grep -E '^\\[ FAIL' | sed '...//' | awk '{print $1}'`:
    a line starting with `[ FAIL` contributes the first whitespace-delimited
    token after the `] `. These are tests that RAN and failed (real evidence),
    as opposed to `died on:` victims (innocent bystanders at the timeout).
    """
    out = []
    for line in log_text.splitlines():
        m = _FAIL_LINE_RE.match(line)
        if m:
            out.append(m.group(1))
    return out


def _parse_summary_last(stdout):
    """Take the LAST `N passed, M failed` line and return passed+failed.

    Faithful to gate.sh:451 `grep -oE ... | tail -1 | awk '{print $1+$3}'`: the
    last summary line wins (a stray earlier match must not), and its
    passed+failed is the count of tests that executed. Returns 0 when no line
    matches — the ran-nothing guard (invariant 1) turns that into a non-pass.
    """
    matches = _SUMMARY_RE.findall(stdout)
    if not matches:
        return 0
    passed, failed = matches[-1]
    return int(passed) + int(failed)


def _default_suite_runner(binary, suite, arb_env, timeout):
    """Run `BEZEL_TEST_FILTER=<suite> <binary>` and return (rc, stdout_text).

    Injected in tests; in production this is the re-run across one per-area
    binary. arb_env (invariant 5) forces BEZEL_AOT_FORCE_SANITIZE=address in
    ASan modes so an ASan-only failure reproduces. stderr is folded into stdout
    (bash: `2>&1`) so a summary the binary writes to stderr is still harvested.
    """
    env = dict(os.environ)
    if arb_env:
        env.update(arb_env)
    env["BEZEL_TEST_FILTER"] = suite
    try:
        proc = subprocess.run(
            [binary], env=env, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout + proc.stderr
    except subprocess.TimeoutExpired:
        return 124, ""
    except OSError:
        return 127, ""


def arb_run_suite(suite, binaries, *, runner=None, arb_env=None,
                  timeout=DEFAULT_SUITE_TIMEOUT):
    """Re-run ONE gtest suite by filter across the per-area binaries.

    A faithful port of gate.sh:445-455. The shard log does not say which binary
    a suspect came from, and it does not need to: a binary holding none of the
    suite walks its registry and exits in ~30ms, so asking all of them costs
    nothing and cannot pick the wrong one. "Passes alone" requires BOTH every
    binary exiting 0 AND at least one test actually having run (invariant 1):
    without the second half a suspect name that matches NOWHERE would exit 0
    having validated nothing and be laundered into a flake-ACCEPT.

    Returns True iff the suite passed alone (every binary rc==0 and total>0);
    returns False on the FIRST nonzero-exit binary (no point continuing) or when
    no test ran across any binary.
    """
    if runner is None:
        runner = _default_suite_runner
    total = 0
    for binpath in binaries:
        if not binpath:
            continue
        rc, stdout = runner(binpath, suite, arb_env, timeout)
        if rc != 0:
            # gate.sh:450 `[ "$rc" -ne 0 ] && return 1` — fail-fast on the first
            # binary that exits nonzero; do not keep re-running the others.
            return False
        total += _parse_summary_last(stdout)
    # gate.sh:454 `[ "$total" -gt 0 ]` — invariant 1: ran-nothing is NOT a pass.
    return total > 0


def arbitrate(log_text, run_suite, *, sample_cap=DEFAULT_SAMPLE_CAP):
    """Arbitrate the sharded-run suspects. Returns an ArbResult.

    A faithful port of gate.sh:457-524. `run_suite(suite) -> bool` is the
    injectable re-run (bound to arb_run_suite + the real binaries in
    production; a stub in tests). `sample_cap` is ARB_SAMPLE (default 8).

    The two suspect classes (gate.sh:471-486) get OPPOSITE treatment, and the
    order in which they are arbitrated is what enforces invariant 2:
      - fail_suites  — from `[ FAIL ]` lines, a test that RAN and failed. Real
        evidence. Arbitrated IN FULL, listed FIRST.
      - crash_suites — from `died on:` lines, the test a shard happened to be
        running when it was killed (usually at the timeout ceiling). Almost
        always innocent. Subject to the cap, listed AFTER every fail_suite, and
        only the ones not already covered by a [ FAIL ] (a suite that failed
        outright needs no second arbitration).
    """
    crashed = parse_crashed(log_text)
    failed = parse_failed(log_text)

    # gate.sh:466 — the UNIQUE test-level suspect set (crashed + failed, minus
    # blank, minus <none>). Used only for the empty-check and the suspect COUNT
    # (ARB_LAST_SUSPECTS); the loop below works on the SUITE-stripped sets.
    suspects_raw = sorted({s for s in crashed + failed if s and s != "<none>"})

    result = ArbResult(verdict=ARB_UNTRUSTWORTHY, last_suspects=0)
    if not suspects_raw:
        # gate.sh:468 `[ -z "$suspects" ] && return 2` — invariant 4: a nonzero
        # exit with NO crash/FAIL marker is an undercount/timeout shape. The
        # caller treats 2 as untrustworthy (gate_full_cycle distrusts it); it is
        # NEVER collapsed into 0 (a pass).
        return result

    result.last_suspects = len(suspects_raw)

    # gate.sh:488-489 — strip to SUITE name (before first '.'), dedupe, drop <none>.
    fail_suites = sorted(
        {_suite_name(s) for s in failed if s and s != "<none>"})
    crash_suites = sorted(
        {_suite_name(s) for s in crashed if s and s != "<none>"})

    # gate.sh:491-493 — a suite already covered by a [ FAIL ] needs no second
    # arbitration. `comm -13` keeps crash suites NOT in fail_suites.
    fail_set = set(fail_suites)
    crash_suites = [s for s in crash_suites if s not in fail_set]

    # gate.sh:495 — fail_suites FIRST, then crash_suites. THIS ORDER IS THE
    # INVARIANT: the cap (below) only ever truncates the trailing crash_suites,
    # so every [ FAIL ] suite is arbitrated before any cap can fire.
    ordered = fail_suites + crash_suites
    crash_set = set(crash_suites)
    ncrash = len(crash_suites)

    result.fail_suites = list(fail_suites)
    result.crash_suites = list(crash_suites)

    real = False
    sampled_crash = 0
    for t in ordered:
        if t in crash_set:
            # gate.sh:500-506 — the cap applies ONLY to crash suites, AFTER every
            # fail_suite has been arbitrated (invariant 2). sampled_crash counts
            # crash suites only; the cap fires when it EXCEEDS the limit, so up
            # to `sample_cap` crash suites are arbitrated and the next one breaks.
            sampled_crash += 1
            if sampled_crash > sample_cap:
                result.messages.append(
                    "   (capped arbitration at {} of {} timeout-victim suites; "
                    "every suite with a real [ FAIL ] was arbitrated in full)"
                    .format(sample_cap, ncrash))
                break

        suite = _suite_name(t)  # idempotent (t is already a suite name)
        result.sampled.append(t)

        passed_alone = False
        for _k in range(DEFAULT_RETRY):
            # gate.sh:509-514 — up to 5 tries; a flake passes within one of them.
            if run_suite(suite):
                passed_alone = True
                break

        if passed_alone:
            result.messages.append(
                "   FLAKY: {} — its suite PASSES alone within 5 tries (sharding "
                "pollution/timing, not a deterministic failure)".format(t))
        else:
            result.messages.append(
                "   REAL: {} — fails alone all 5 tries".format(t))
            real = True

    # gate.sh:522-523 — invariant 4: 1 if any suspect is REAL, else 0. Never 2
    # here (the empty-suspects return-2 was handled above).
    result.verdict = ARB_REAL if real else ARB_FLAKY
    return result
