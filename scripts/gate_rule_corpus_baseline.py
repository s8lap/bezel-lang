#!/usr/bin/env python3
r"""gate_rule_corpus_baseline.py - gate the rule corpus against its own
baseline ratchet (issue #4621).

WHY THIS EXISTS. tests/conformance/rules/ (the hand-authored rule corpus) has
had a real baseline ratchet since its founding: `runner.py` compares every
case's observed verdict against `baseline.tsv` and calls a banked-pass ->
observed-fail row a REGRESSION, and a banked-fail -> observed-pass row
UNBANKED (README.md, "The baseline ratchet"). Both are meant to fail a gate
cycle. But `gate_full_cycle.sh`'s existing "rule-corpus phase" only runs the
corpus HARNESS's own self-tests and `corpus.py check` (manifest currency) - it
never actually runs the corpus itself against the baseline. So a REGRESSION or
an UNBANKED row could sit red indefinitely and no gate cycle would ever see
it. That is exactly what happened: v0.307 (#4608) changed 3 case files and
added 35 more without re-banking baseline.tsv, leaving 3 REGRESSION + 35
UNCLASSIFIED rows that no cycle had any way to notice (#4621).

This script is the missing half. It is deliberately named `gate_*.py`, not
`check_*.py`: it is NOT build-free like the check_*.py ratchets (the corpus
needs a real `bezelc` to run, and a full pass takes well over a minute), so
it must NOT be swept up by `scripts/ratchet_sweep.sh`, which derives its
build-free pre-gate list by grepping `gate_full_cycle.sh` for
`scripts/check_[a-z_]+\.py` and is documented ("build-free; ~70s; safe to
run while lanes are building") on the assumption that every match is
build-free. It is wired into gate_full_cycle.sh AFTER a bezelc exists
(beside the "conformance phase", the spec-example sweep, which has the same
requirement), not in the build-free pre-gate ratchet sweep.

    gate_rule_corpus_baseline.py --self-test
    gate_rule_corpus_baseline.py --strict --bezelc PATH [--jobs N]

--self-test is build-free (mirrors the sibling convention exactly: it must run
before --strict and is not optional - a green from a matcher that matched
nothing is worth nothing). It proves two things, entirely synthetically:

  1. runner.py's own `compare(verdict, baseline_row)` classifies every one of
     the five (banked, observed) combinations exactly as README.md's ratchet
     table states - in particular that a banked-pass/observed-fail row really
     does come back REGRESSION and a banked-fail/observed-pass row really does
     come back UNBANKED, not silently folded into OK/KNOWN.
  2. This script's OWN summary-line parser and floor/cap logic (parse_summary,
     evaluate) correctly extracts REGRESSION/UNBANKED/total counts from a
     runner.py-shaped summary line and correctly turns a nonzero
     REGRESSION/UNBANKED, or a total below a given floor, into a failure - and
     correctly stays green on an all-clear line. A malformed or truncated
     summary line (the "0 cases: ..." vacuous-collection shape, or empty
     output) must also fail closed, never pass by default.

--strict actually runs `tests/conformance/rules/runner.py` against the
checked-in `baseline.tsv`, parses its summary line, and fails the cycle if:
  - REGRESSION > 0 or UNBANKED > 0 (the ratchet itself - runner.py's own exit
    code already encodes this, but this script parses independently rather
    than trusting only the exit code, for the same "don't trust a vacuous
    green" reason the floor exists), or
  - the total case count does not equal `manifest.tsv`'s row count (a
    collection failure, a bad --filter, or a missing cases/ directory prints a
    deceptively normal-looking but empty or partial summary otherwise).

See tests/conformance/rules/README.md, "Gate wiring", for the corpus's own
account of why the split between the harness phase and this phase exists.
"""

import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RULES_DIR = os.path.join(REPO, "tests", "conformance", "rules")
RUNNER = os.path.join(RULES_DIR, "runner.py")
BASELINE = os.path.join(RULES_DIR, "baseline.tsv")
MANIFEST = os.path.join(RULES_DIR, "manifest.tsv")

sys.path.insert(0, RULES_DIR)

SUMMARY_RE = re.compile(
    r'(\d+) cases: (\d+) ok, (\d+) known-divergent, (\d+) unclassified, '
    r'(\d+) REGRESSION, (\d+) UNBANKED')


def parse_summary(output):
    """Extract the trailing 'N cases: ... REGRESSION, ... UNBANKED' line from
    runner.py's stdout. Returns a dict of ints, or None if no such line is
    present anywhere in the output - the fail-closed case (empty output,
    a crash before the summary prints, or a reworded summary line)."""
    m = None
    for line in output.splitlines():
        found = SUMMARY_RE.search(line)
        if found:
            m = found  # keep the LAST match, in case case titles echo the shape
    if m is None:
        return None
    total, ok, known, unclassified, regression, unbanked = (
        int(x) for x in m.groups())
    return {
        "total": total, "ok": ok, "known": known,
        "unclassified": unclassified, "regression": regression,
        "unbanked": unbanked,
    }


def evaluate(summary, floor):
    """Given a parsed summary and a required floor on total cases run,
    return (ok: bool, reason: str). Pure - the self-test exercises this
    directly with synthetic summaries, no subprocess involved."""
    if summary is None:
        return False, ("no 'N cases: ...' summary line found in runner.py's "
                        "output - the run crashed, produced nothing, or the "
                        "summary format changed underneath this parser")
    if summary["total"] < floor:
        return False, (
            "only {} case(s) ran, below the floor of {} (manifest.tsv's row "
            "count) - a collection failure or an over-narrow filter would "
            "print an all-clear-looking summary over almost nothing"
            .format(summary["total"], floor))
    if summary["regression"] or summary["unbanked"]:
        return False, (
            "{} REGRESSION, {} UNBANKED row(s) - a banked `pass` case now "
            "fails, or a banked `fail` case now passes and needs banking "
            "(tests/conformance/rules/runner.py --update --filter <id>)"
            .format(summary["regression"], summary["unbanked"]))
    return True, "{} cases, {} ok, {} known-divergent, {} unclassified".format(
        summary["total"], summary["ok"], summary["known"],
        summary["unclassified"])


# --------------------------------------------------------------------------
# --self-test: build-free, no bezelc, no subprocess against a real compiler.
# --------------------------------------------------------------------------

def self_test():
    ok = True

    def chk(desc, cond):
        nonlocal ok
        status = "ok" if cond else "FAIL"
        print("  [{}] {}".format(status, desc))
        if not cond:
            ok = False

    # Part 1: runner.py's own compare() must match README.md's ratchet table
    # exactly. Import directly rather than shelling out - compare() is pure
    # and this is the classification logic the whole ratchet rests on.
    import runner  # noqa: E402  (sys.path rigged above)

    chk("banked pass, observed pass -> OK",
        runner.compare("pass", {"verdict": "pass", "note": ""}) == runner.OK)
    chk("banked pass, observed fail -> REGRESSION",
        runner.compare("fail", {"verdict": "pass", "note": ""})
        == runner.REGRESSION)
    chk("banked fail, observed fail -> KNOWN",
        runner.compare("fail", {"verdict": "fail", "note": "divergence"})
        == runner.KNOWN)
    chk("banked fail, observed pass -> UNBANKED",
        runner.compare("pass", {"verdict": "fail", "note": "divergence"})
        == runner.UNBANKED)
    chk("no baseline row -> UNCLASSIFIED",
        runner.compare("pass", None) == runner.UNCLASSIFIED)
    chk("no baseline row, observed fail -> still UNCLASSIFIED "
        "(never silently REGRESSION)",
        runner.compare("fail", None) == runner.UNCLASSIFIED)

    # Part 2: this script's own parser/evaluator must fail closed on the
    # shapes that would otherwise pass vacuously, and pass on a real green.
    clean = ("2184 cases: 1758 ok, 426 known-divergent, 0 unclassified, "
             "0 REGRESSION, 0 UNBANKED\n")
    regressed = ("2184 cases: 1757 ok, 426 known-divergent, 0 unclassified, "
                 "1 REGRESSION, 0 UNBANKED\n")
    unbanked = ("2184 cases: 1758 ok, 425 known-divergent, 0 unclassified, "
                "0 REGRESSION, 1 UNBANKED\n")
    short = ("10 cases: 10 ok, 0 known-divergent, 0 unclassified, "
             "0 REGRESSION, 0 UNBANKED\n")  # a narrow/broken filter

    chk("a clean summary at the real floor passes",
        evaluate(parse_summary(clean), floor=2184)[0] is True)
    chk("a REGRESSION row fails, even with everything else clean",
        evaluate(parse_summary(regressed), floor=2184)[0] is False)
    chk("an UNBANKED row fails, even with everything else clean",
        evaluate(parse_summary(unbanked), floor=2184)[0] is False)
    chk("a summary below the floor fails (the vacuous-collection shape "
        "issue #4621 also names - a narrow filter or missing cases/ prints "
        "a normal-looking all-clear line over almost nothing)",
        evaluate(parse_summary(short), floor=2184)[0] is False)
    chk("empty output (a crash before any summary prints) fails closed",
        evaluate(parse_summary(""), floor=2184)[0] is False)
    chk("output with no summary line at all fails closed "
        "(a reworded summary format must not silently read as clean)",
        evaluate(parse_summary("some unrelated stack trace\n"), floor=2184)[0]
        is False)

    print("gate_rule_corpus_baseline --self-test: "
          + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# --------------------------------------------------------------------------
# --strict: the real thing. Needs a built bezelc.
# --------------------------------------------------------------------------

def manifest_row_count(manifest_path):
    n = 0
    with open(manifest_path, encoding="utf-8") as f:
        header = True
        for line in f:
            if header:
                header = False
                continue
            if line.strip():
                n += 1
    return n


def strict(bezelc, jobs):
    # Resolve to an absolute path before handing it to the subprocess: the
    # subprocess runs with cwd=RULES_DIR (runner.py expects to be invoked
    # from there for its own relative case-file lookups), so a relative
    # bezelc path here would resolve against the WRONG directory and fail
    # inside runner.py with a confusing "not executable" for a path that is
    # perfectly executable from this script's own cwd.
    bezelc = os.path.abspath(bezelc)
    if not os.access(bezelc, os.X_OK):
        print("gate_rule_corpus_baseline: ERROR: bezelc not executable: "
              + bezelc, file=sys.stderr)
        return 2
    if not os.path.exists(MANIFEST):
        print("gate_rule_corpus_baseline: ERROR: " + MANIFEST
              + " missing - run corpus.py build", file=sys.stderr)
        return 2
    floor = manifest_row_count(MANIFEST)

    proc = subprocess.run(
        [sys.executable, RUNNER, bezelc, "--baseline", BASELINE,
         "--jobs", str(jobs)],
        cwd=RULES_DIR, capture_output=True, text=True)
    output = proc.stdout + "\n" + proc.stderr
    summary = parse_summary(output)
    good, reason = evaluate(summary, floor)

    if not good:
        print("gate_rule_corpus_baseline: FAIL - " + reason)
        print()
        # Surface the interesting lines (REGRESSION/UNBANKED rows, or the
        # tail if the summary itself is what's missing) rather than the
        # whole corpus's KNOWN-divergence noise.
        interesting = [l for l in output.splitlines()
                       if l.startswith(("REGRESSION", "UNBANKED", "ERROR"))]
        for line in (interesting or output.splitlines()[-20:]):
            print("  | " + line)
        return 1

    print("gate_rule_corpus_baseline: OK (" + reason + ")")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--self-test", action="store_true",
                     help="prove the detector still detects (build-free)")
    ap.add_argument("--strict", action="store_true",
                     help="gate mode: run the corpus for real and fail on "
                          "any REGRESSION/UNBANKED row or a low case count")
    ap.add_argument("--bezelc", default=os.path.join(REPO, "build", "bezelc"))
    ap.add_argument("--jobs", type=int, default=12)
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.strict:
        return strict(args.bezelc, args.jobs)

    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
