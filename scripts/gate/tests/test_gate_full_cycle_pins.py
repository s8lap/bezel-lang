"""fix #3666 — clang-tidy once per cycle, no canary on the ASan oracle, and the
attempt/oracle exports from gate_full_cycle.sh into gate.sh.

WHAT WAS MEASURED (gate-timings.jsonl, 46 gate.sh runs since 2026-08-28):

  clang-tidy   277 s median per gate.sh run -> ~547 s per two-oracle cycle,
               24% of a ~2230 s cycle. Advisory end to end (check_tidy.sh
               always exits 0; gate.sh never called `fail` on it), uncached,
               identical inputs on both passes (HEAD is pinned for the cycle),
               and NO reader: `grep tidy scripts/gate/*.py
               scripts/gate/tests/*.py scripts/gate_trend.sh` is empty. The
               second run was a pure duplicate.
  canary       0 catches in 46 runs (every real failure — 4 plain, 1 ASan —
               was found by the shard phase); on --full a median 184 s (up to
               286 s) whenever it ran. EV(keep it on ASan) ~ -46 s/cycle.

WHAT CHANGED, and which class here pins it:

  1. gate.sh no longer invokes check_tidy.sh, and says how to run it by hand
                                                  -> GateShNoTidyTest
  2. gate_full_cycle.sh runs it exactly once, after the conformance sweep is
     GREEN and before the HEAD-moved check, and the block contains no
     exit/return — advisory cannot become blocking by accident. A functional
     run against a stub tool that FAILS proves the cycle carries on.
                                                  -> CycleShTidyOnceTest
  3. run_until_trustworthy() exports BEZEL_GATE_ATTEMPT=<i>,
     BEZEL_GATE_ORACLE=plain|asan and GATE_CANARY (0 on --full; an operator's
     own GATE_CANARY=0 honoured on both passes) to every gate.sh attempt.
                                                  -> RunUntilTrustworthyExportsTest
  4. gate.sh honours GATE_CANARY=0 with the exact log line the cycle log is
     grepped for, and its shard-ceiling line names the attempt.
                                                  -> GateShCanaryKnobTest

HOW THE FUNCTIONAL PINS WORK. None of them runs the real gate.sh (that is a
build plus a 40-minute suite). RunUntilTrustworthyExportsTest extracts
`_legacy_rc_decision()` and `run_until_trustworthy()` VERBATIM from
gate_full_cycle.sh by sentinel, sources them in a temp cwd with `say`/`sleep`
stubbed, and points `scripts/gate.sh` at a stub that records the three
variables and writes a REAL verdict through gate.verdict (rc 3 = coverage
short -> retry on the first call, rc 0 = pass after). So the loop's own
retry/accept path, its verdict read and its classify / legacy-decision /
cross-check calls into scripts/gate/*.py all run — only the gate is a stub.
GateShCanaryKnobTest and CycleShTidyOnceTest do the same with the canary block
of gate.sh and the tidy block of gate_full_cycle.sh.

This module reads script TEXT and drives extracted bodies against stubs. It
never spawns the real gate_and_push.sh — it is registered as "source-only" in
test_gate_markers.SelfTestSuiteIsolationTest.KNOWN.

SHARPEN (review of the #3666 landing). The tidy block's two command
substitutions (`_tidy_n="$(sed … | tail -1)"`, `_tidy_why="$(grep … |
tail -1)"`) were its only unguarded statements; the reviewer measured
that under `set -e -o pipefail` they abort a GREEN cycle (grep rc 1 on the
ordinary tool-absent path, sed rc 2 on an unreadable log). Both now carry
`|| true`, and the functional harness runs the block under
`set -eu -o pipefail` with an EMPTY log. Also: when run-clang-tidy itself
fails, check_tidy.sh prints `check-tidy: 0 finding(s)` BEFORE its
`… possible tool/config failure` line, so the say line preferred a bogus
0; it now prefers the failure line. Both pinned below.

FIX #3718 (section 2b below). The tidy phase resolved its changed-file base
with `git merge-base HEAD main` — the LOCAL `main` branch, which nothing in
the run workflow ever fast-forwards: integration branches track `origin/main`
and gate_and_push.sh lands through that upstream, never through a local ref.
Measured 2026-09-03 on the R12 integration head with local `main` one cycle
behind: 16 changed `src/**/*.cpp` against the stale `main` versus 1 against
`origin/main` — an advisory phase spending >=15 min a cycle on files the cycle
never touched, and it re-rots the moment origin/main advances, so the one-off
`git branch -f main origin/main` workaround does not hold. The base is now the
branch's `@{upstream}`, falling back to `origin/main` and then (as before) to
`HEAD~1`; check_tidy.sh also prints the base's short SHA, so a cycle log
records WHICH commit it diffed against rather than a ref name that moves.
                                                  -> CycleShTidyBaseIsUpstreamTest

Stdlib only (subprocess/tempfile/unittest) — the house style for this
directory. Every test here FAILED at the parent commit (af4f73b) except the
two marked GUARD in their docstrings, which pin a contract the change now
leans on (the GATE_CANARY=0 knob itself dates from #2100) rather than the
change itself; the sharpen pins failed at a5772aa.

FIX #3667 / FIX #3674 (section 5 below). A shard killed at BEZEL_SHARD_TIMEOUT
writes no summary; run_parallel exited before its banner; gate_arbitrate found
no marker in a log that was never allowed to carry one; the verdict was FAIL
and the cycle BAILed — although the kill left the AOT bincache ~95% warm and a
retry costs ~310 s plain / ~500 s ASan against a ~2230 s cycle plus a relaunch.
gate.summary (narrow #3667 / sharpen #3668) classifies that shape; the pins in
section 5 cover the bash that maps its exit 0 onto the EXISTING rc=3 RETRY
contract (and nothing else onto anything), bounds the ceiling-only retries at
three in run_until_trustworthy, swaps the shard_times harvest that has
matched nothing since d0d2390 (2026-08-18) for the `shard-times` CLI, and puts `actual FAIL` into the condensing grep so
the culprit a completed shard names reaches the reader (#3674). The gate.sh
pins drive the EXTRACTED rc!=0 block and gate_shard_logdir() against the REAL
gate.summary over a fixture logdir, so the 0/1/2 contract is exercised, not
just spelled. Every section-5 test FAILED at e9a10c7 except the two marked
GUARD.

  6. fix #3690: gate.sh stores without the #3009 fsync pair
     (SHARD_ENV+=(BEZEL_AOT_STORE_FSYNC=0), #3669) AFTER the ASan branch
     reassigns SHARD_ENV and BEFORE both run sites, and logs it beside
     the ceiling line.                             -> GateShStoreFsyncOffTest

FIX #3725 (section 2c, sibling of #3718, found during its review). The same
`git merge-base HEAD main` defect existed a second, independent time in
`scripts/agent_check.sh`'s `--select` default base. Measured 2026-09-03 in a
lane worktree: 10628 tests selected against the stale local `main` versus
6022 against `origin/main` for the same HEAD (~1.8x over-selection), and
`scripts/select_tests.py`'s own default base already resolved against
`origin/main`, so the two scripts disagreed with each other. The scan
`_STALE_MAIN_RE` runs over is extended to `agent_check.sh` so a fourth
instance cannot reappear anywhere in scripts/.
                                                  -> AgentCheckSelectBaseIsUpstreamTest
"""

import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPTS = HERE.parent.parent          # scripts/
GATE_SH = SCRIPTS / "gate.sh"
CYCLE_SH = SCRIPTS / "gate_full_cycle.sh"
CHECK_TIDY_SH = SCRIPTS / "check_tidy.sh"
AGENT_CHECK_SH = SCRIPTS / "agent_check.sh"
GATE_PKG = SCRIPTS / "gate"
RP_SH = SCRIPTS.parent / "tests" / "run_parallel.sh"

# Section 5 builds run-summary fixtures for the REAL classifier; the schema
# string comes from the module so the fixture cannot drift from it.
sys.path.insert(0, str(SCRIPTS))
from gate import summary as _summary  # noqa: E402

# Variables the cycle now exports; a test runner's environment must not leak
# them into a harness (an operator's exported GATE_CANARY=0 would otherwise
# make the plain-pass expectation wrong for the wrong reason).
_SCRUB = ("GATE_CANARY", "BEZEL_GATE_ATTEMPT", "BEZEL_GATE_ORACLE",
          "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE")


def _code_lines(text):
    """(lineno, line) for every line that is not blank or a full-line comment.

    Prose in comments mentions `exit`, `check_tidy` and friends (that is how
    the scripts document themselves), so every pin below looks at CODE only.
    Inline ` # ...` comments are deliberately NOT stripped: gate.sh has
    strings like 'aot-asan #1822]' that a naive strip would truncate, and a
    pin that ignores what follows a `#` inside a string could miss an `exit`.
    """
    out = []
    for n, ln in enumerate(text.splitlines(), 1):
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        out.append((n, ln))
    return out


def _slice(text, start, end, what, search_from=0):
    """text[start .. end] by sentinel. `start` must occur exactly once at or
    after `search_from`; `end` is the FIRST occurrence after `start`. Returns
    (block, start_index). A miss is a loud failure, never an empty block."""
    idx = text.find(start, search_from)
    if idx < 0 or text.find(start, idx + 1) >= 0:
        raise AssertionError("%s: start sentinel %r not found exactly once" % (what, start))
    e = text.find(end, idx + len(start))
    if e < 0:
        raise AssertionError("%s: end sentinel %r not found after start" % (what, end))
    return text[idx:e + len(end)], idx


def _clean_env(tmp):
    env = dict(os.environ)
    for k in _SCRUB:
        env.pop(k, None)
    # A harness cwd is a bare temp dir; make sure git cannot walk up into some
    # enclosing repository and turn `merge-base` into a real answer.
    env["GIT_CEILING_DIRECTORIES"] = str(pathlib.Path(tmp).parent)
    return env


def _layout(tmp):
    """The temp cwd every functional harness runs in: `scripts/gate` is a
    symlink to the REAL package (the extracted bash calls
    `PYTHONPATH=scripts python3 -m gate.<mod>` relative to cwd), and
    `scripts/gate.sh` is whatever stub the test installs."""
    sd = pathlib.Path(tmp) / "scripts"
    sd.mkdir()
    (sd / "gate").symlink_to(GATE_PKG, target_is_directory=True)
    return sd


def _bash(script, tmp, env, timeout=120):
    return subprocess.run(["bash", "-c", script], cwd=tmp, env=env,
                          capture_output=True, text=True, timeout=timeout)


# ---------------------------------------------------------------------------
# 1. gate.sh no longer runs clang-tidy.
# ---------------------------------------------------------------------------

class GateShNoTidyTest(unittest.TestCase):
    """gate.sh ran `check_tidy.sh --changed` in BOTH oracles of a cycle (277 s
    median each) with no reader of the result. It must not invoke it at all;
    the cycle runs it once (CycleShTidyOnceTest)."""

    def test_gate_sh_has_no_check_tidy_invocation(self):
        text = GATE_SH.read_text()
        hits = [(n, ln) for n, ln in _code_lines(text) if "check_tidy" in ln]
        self.assertEqual(
            hits, [],
            "gate.sh still invokes check_tidy.sh — that phase ran once per "
            "ORACLE (~547 s per cycle, advisory, no consumer) and was moved to "
            "gate_full_cycle.sh, once per cycle (fix #3666):\n%s"
            % "\n".join("  %d: %s" % h for h in hits))

    def test_gate_sh_header_says_how_to_run_tidy_by_hand(self):
        """A standalone gate.sh run used to include static analysis; a
        reader who notices it is gone must find the by-hand recipe in the
        header, not by archaeology."""
        text = GATE_SH.read_text()
        comment = "\n".join(ln for ln in text.splitlines()
                            if ln.lstrip().startswith("#"))
        self.assertIn("check_tidy.sh build --changed", comment,
                      "gate.sh's header must carry the by-hand recipe "
                      "`scripts/check_tidy.sh build --changed <base>` now "
                      "that the phase is gone from it (fix #3666)")


# ---------------------------------------------------------------------------
# 2. gate_full_cycle.sh runs it exactly once, advisory, in the right place.
# ---------------------------------------------------------------------------

_TIDY_START = 'say "tidy phase: clang-tidy over changed'
_CONFORMANCE_GREEN = 'say "conformance phase: GREEN"'
_HEAD_MOVED = "HEAD MOVED DURING THE CYCLE"

_TIDY_HARNESS = r'''
%(setopts)s
cd "$PIN_TMP"
export PATH="$PIN_TMP/bin:$PATH"
say() { echo "SAY $*"; }
LOGDIR="$PIN_TMP/logs"; mkdir -p "$LOGDIR"
TAG=pin
CYCLE_LOG="$LOGDIR/pin_cycle.log"
%(block)s
echo "AFTER_TIDY_BLOCK rc=$?"
'''


class CycleShTidyOnceTest(unittest.TestCase):
    """The tidy phase lives in the cycle: once, after the last verdict-bearing
    phase (conformance GREEN), before the HEAD-moved check, and it cannot end
    the cycle whatever its tool does."""

    def _invocations(self, lines):
        return [(n, ln) for n, ln in lines
                if "check_tidy.sh" in ln and "--changed" in ln]

    def test_exactly_one_changed_invocation_after_conformance_green(self):
        text = CYCLE_SH.read_text()
        lines = _code_lines(text)
        inv = self._invocations(lines)
        self.assertEqual(
            len(inv), 1,
            "gate_full_cycle.sh must invoke `check_tidy.sh … --changed` "
            "exactly once (fix #3666); found %d:\n%s"
            % (len(inv), "\n".join("  %d: %s" % h for h in inv)))
        inv_line = inv[0][0]
        green = [n for n, ln in lines if _CONFORMANCE_GREEN in ln]
        moved = [n for n, ln in lines if _HEAD_MOVED in ln]
        self.assertEqual(len(green), 1, "guard: conformance-GREEN line not unique")
        self.assertTrue(moved, "guard: HEAD-moved check not found")
        self.assertGreater(
            inv_line, green[0],
            "the tidy invocation (line %d) must come AFTER the conformance "
            "sweep reports GREEN (line %d): a red cycle and every retry "
            "attempt must pay nothing for an advisory phase" % (inv_line, green[0]))
        self.assertLess(
            inv_line, min(moved),
            "the tidy invocation (line %d) must come BEFORE the HEAD-moved "
            "check (line %d), which is what pins the inputs for the whole "
            "cycle" % (inv_line, min(moved)))

    def test_tidy_block_has_no_exit_or_return(self):
        """From the column-0 `if` enclosing the invocation to its column-0
        `fi`, no code line may exit or return: an advisory phase that can end
        the cycle is a blocking phase with a misleading name."""
        text = CYCLE_SH.read_text()
        raw = text.splitlines()
        lines = _code_lines(text)
        inv = self._invocations(lines)
        self.assertEqual(len(inv), 1, "guard: one invocation (pinned above)")
        i = inv[0][0] - 1                       # 0-based index into raw
        top = i
        while top >= 0 and not raw[top].startswith("if "):
            top -= 1
        self.assertGreaterEqual(top, 0, "no column-0 `if` encloses the tidy invocation")
        bot = i
        while bot < len(raw) and raw[bot] != "fi":
            bot += 1
        self.assertLess(bot, len(raw), "no column-0 `fi` closes the tidy block")
        block = [(n, ln) for n, ln in lines if top + 1 <= n <= bot + 1]
        bad = [(n, ln) for n, ln in block if re.search(r"\b(exit|return)\b", ln)]
        self.assertEqual(
            bad, [],
            "the tidy block (lines %d-%d) contains exit/return — it is "
            "ADVISORY and must never end the cycle (fix #3666):\n%s"
            % (top + 1, bot + 1, "\n".join("  %d: %s" % h for h in bad)))
        # The invocation itself must be shielded so a future `set -e` cannot
        # turn a tool's non-zero exit into a cycle abort.
        self.assertIn("|| true", raw[i] + raw[i + 1],
                      "the check_tidy invocation must be `|| true`-guarded")

    def _extract_block(self):
        text = CYCLE_SH.read_text()
        block, _ = _slice(text, _TIDY_START, "\nfi\n", "tidy block")
        self.assertIn("check_tidy.sh", block, "guard: extracted block lacks the invocation")
        return block

    def _run_block(self, dev_heavy_stub, check_tidy_stub, strict=False):
        """Source the tidy block in a temp cwd with stub `clang-tidy` /
        `run-clang-tidy` on PATH (so the main branch runs) and stub
        scripts/dev_heavy.sh + scripts/check_tidy.sh. `strict` runs it
        under `set -eu -o pipefail` — the block must be advisory under
        ANY future set option, not just today's `set -u`."""
        with tempfile.TemporaryDirectory() as tmp:
            sd = _layout(tmp)
            bind = pathlib.Path(tmp) / "bin"
            bind.mkdir()
            for tool in ("clang-tidy", "run-clang-tidy"):
                p = bind / tool
                p.write_text("#!/usr/bin/env bash\nexit 0\n")
                p.chmod(0o755)
            dh = sd / "dev_heavy.sh"
            dh.write_text(dev_heavy_stub)
            dh.chmod(0o755)
            ct = sd / "check_tidy.sh"
            ct.write_text(check_tidy_stub)
            ct.chmod(0o755)
            env = _clean_env(tmp)
            env["PIN_TMP"] = tmp
            r = _bash(_TIDY_HARNESS % {
                "block": self._extract_block(),
                "setopts": "set -eu -o pipefail" if strict else "set -u"}, tmp, env)
            cycle_log = pathlib.Path(tmp) / "logs" / "pin_cycle.log"
            cycle_text = cycle_log.read_text() if cycle_log.exists() else ""
            return r, cycle_text

    # dev_heavy stub that just runs its command (kind + cmd...), like the real one.
    _DH_PASSTHROUGH = "#!/usr/bin/env bash\nshift\nexec \"$@\"\n"

    def test_tidy_block_reports_elapsed_and_finding_count(self):
        for strict in (False, True):
            with self.subTest(strict=strict):
                r, cycle_text = self._run_block(
                    self._DH_PASSTHROUGH,
                    "#!/usr/bin/env bash\n"
                    "echo \"check-tidy: changed-files mode vs $4 — 2 file(s)\"\n"
                    "echo 'check-tidy: 3 finding(s) (ADVISORY — does not fail the build)'\n"
                    "exit 0\n", strict=strict)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("AFTER_TIDY_BLOCK rc=0", r.stdout,
                              "the harness must run past the block:\n" + r.stdout + r.stderr)
                done = [ln for ln in r.stdout.splitlines() if "tidy phase: done in" in ln]
                self.assertEqual(len(done), 1, "one `done` say line expected:\n" + r.stdout)
                self.assertRegex(done[0], r"done in \d+s", "elapsed seconds missing")
                self.assertIn("3 finding(s)", done[0], "finding count missing: " + done[0])
                self.assertIn("check-tidy: 3 finding(s)", cycle_text,
                              "the check-tidy summary lines must be teed into the cycle log")

    def test_tidy_block_survives_a_failing_tool(self):
        """The whole point of 'advisory': dev_heavy refusing (exit 5, no
        output at all) must leave the cycle running and say so."""
        for strict in (False, True):
            with self.subTest(strict=strict):
                r, _ = self._run_block(
                    "#!/usr/bin/env bash\necho 'dev_heavy: refusing (stub)' >&2\nexit 5\n",
                    "#!/usr/bin/env bash\nexit 0\n", strict=strict)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("AFTER_TIDY_BLOCK rc=0", r.stdout,
                              "a failing tidy tool ended the cycle — the phase is "
                              "ADVISORY (fix #3666):\n" + r.stdout + r.stderr)
                done = [ln for ln in r.stdout.splitlines() if "tidy phase: done in" in ln]
                self.assertEqual(len(done), 1, r.stdout)
                self.assertIn("no findings count", done[0],
                              "a run with no `check-tidy:` findings line must say so, "
                              "not report 0: " + done[0])

    # ---- sharpen (review of a5772aa): errexit/pipefail + tool-failure ----

    def test_tidy_assignments_are_guarded_for_errexit(self):
        """Source pin: the two command substitutions carry `|| true`."""
        lines = _code_lines(CYCLE_SH.read_text())
        for var in ('_tidy_n="$(', '_tidy_why="$('):
            hits = [(n, ln) for n, ln in lines if ln.lstrip().startswith(var)]
            self.assertEqual(len(hits), 1, "guard: one %s assignment" % var)
            n, ln = hits[0]
            self.assertTrue(
                ln.rstrip().endswith("|| true"),
                "gate_full_cycle.sh:%d: %s must end with `|| true` — under "
                "set -e -o pipefail a no-match grep / unreadable log aborts "
                "a GREEN cycle from an advisory block: %s" % (n, var, ln))

    def test_tidy_block_survives_errexit_pipefail_with_empty_log(self):
        """The measured shape: tool present, log EMPTY (no `check-tidy:`
        line at all), `set -eu -o pipefail`. grep exits 1, pipefail makes
        the substitution fail, and without `|| true` errexit ends the
        cycle right there."""
        r, _ = self._run_block(self._DH_PASSTHROUGH,
                               "#!/usr/bin/env bash\nexit 0\n", strict=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("AFTER_TIDY_BLOCK rc=0", r.stdout,
                      "an empty tidy log ended the cycle under errexit/pipefail "
                      "(sharpen of #3666):\n" + r.stdout + r.stderr)
        done = [ln for ln in r.stdout.splitlines() if "tidy phase: done in" in ln]
        self.assertEqual(len(done), 1, r.stdout)
        self.assertIn("no check-tidy output", done[0], done[0])

    def test_tidy_block_prefers_tool_failure_over_zero_count(self):
        """check_tidy.sh prints `check-tidy: 0 finding(s)` BEFORE its
        `… exited N with no parsed findings — possible tool/config failure`
        line when run-clang-tidy itself failed. A cycle log saying
        `0 finding(s)` for a tool that did not run is a false clean."""
        stub = ("#!/usr/bin/env bash\n"
                "echo 'check-tidy: sanitized database has 1760 TU(s) (from 1760 raw entries)'\n"
                "echo 'check-tidy: 0 finding(s) (ADVISORY — does not fail the build)'\n"
                "echo 'check-tidy: run-clang-tidy exited 1 with no parsed findings — "
                "possible tool/config failure, see /x/tidy/run.log'\n"
                "exit 0\n")
        for strict in (False, True):
            with self.subTest(strict=strict):
                r, _ = self._run_block(self._DH_PASSTHROUGH, stub, strict=strict)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("AFTER_TIDY_BLOCK rc=0", r.stdout, r.stdout + r.stderr)
                done = [ln for ln in r.stdout.splitlines() if "tidy phase: done in" in ln]
                self.assertEqual(len(done), 1, r.stdout)
                self.assertIn("possible tool/config failure", done[0],
                              "the say line must surface the tool failure: " + done[0])
                self.assertNotIn("0 finding(s)", done[0],
                                 "a 0 from a tool that did not run is not a count: " + done[0])


# ---------------------------------------------------------------------------
# 2b. fix #3718 — the advisory tidy base is the branch's UPSTREAM, never the
#     bare local `main`.
# ---------------------------------------------------------------------------

_TIDY_BASE_START = '_tidy_up="$(git rev-parse'
_TIDY_BASE_END = "_tidy_t0="

_BASE_HARNESS = r'''
%(setopts)s
cd "$PIN_REPO"
%(snippet)s
echo "TIDY_UP=$_tidy_up"
echo "TIDY_BASE=$_tidy_base"
'''

# `merge-base HEAD main` in any quoting. `main` must be a word: `main`,
# "main", 'main' — but NOT origin/main, which is the whole point of the fix.
_STALE_MAIN_RE = re.compile(r"""merge-base\s+HEAD\s+["']?main\b""")


class CycleShTidyBaseIsUpstreamTest(unittest.TestCase):
    """#3718. `git merge-base HEAD main` resolved against the LOCAL `main`
    branch, which nothing in the run workflow ever fast-forwards: integration
    branches track `origin/main` and gate_and_push.sh lands through that
    upstream. Measured 2026-09-03 on the R12 integration head: 16 changed
    `src/**/*.cpp` against a stale local `main` versus 1 against `origin/main`
    — an advisory phase costing >=15 min a cycle over files the cycle never
    touched, re-rotting every time origin/main advances."""

    # -- source -------------------------------------------------------------

    def test_no_script_resolves_a_base_against_the_bare_local_main(self):
        # fix #3725: agent_check.sh's `--select` default base had the SAME
        # defect independently (`git merge-base HEAD main`) — added to this
        # scan so a fourth instance cannot reappear anywhere in scripts/.
        for path in (CYCLE_SH, GATE_SH, AGENT_CHECK_SH):
            with self.subTest(script=path.name):
                bad = [(n, ln) for n, ln in _code_lines(path.read_text())
                       if _STALE_MAIN_RE.search(ln)]
                self.assertEqual(
                    bad, [],
                    "%s resolves a base against the bare local `main` "
                    "(#3718/#3725). Local `main` is whatever it was when "
                    "someone last checked it out; use the branch's "
                    "`@{upstream}`, falling back to `origin/main`:\n%s"
                    % (path.name, "\n".join("  %d: %s" % h for h in bad)))

    def test_tidy_base_resolution_names_the_upstream_and_the_fallback(self):
        block = _slice(CYCLE_SH.read_text(), _TIDY_START, "\nfi\n", "tidy block")[0]
        # CODE only. The block's comment recounts the defect verbatim, so a
        # whole-text search would stay green over a reverted resolution.
        code = "\n".join(ln for _n, ln in _code_lines(block))
        self.assertIn("@{upstream}", code,
                      "the tidy base must be resolved from the branch's own "
                      "upstream (#3718)")
        self.assertIn("origin/main", code,
                      "the tidy base needs an `origin/main` fallback for a "
                      "detached HEAD or a lane branch with no upstream (#3718)")
        self.assertIn("merge-base HEAD", code,
                      "guard: the base is still a merge-base against that ref")

    def test_check_tidy_reports_the_base_sha_not_only_the_ref_name(self):
        """A log line that only echoes the ref name back cannot show that the
        base was stale — which is how #3718 survived unnoticed."""
        text = CHECK_TIDY_SH.read_text()
        self.assertIn('base_sha="$(git rev-parse --short "$base"', text,
                      "check_tidy.sh --changed must resolve the base to a sha "
                      "(#3718)")
        said = [ln for n, ln in _code_lines(text)
                if "check-tidy:" in ln and "$base_sha" in ln]
        self.assertGreaterEqual(
            len(said), 1,
            "check_tidy.sh must print the resolved base sha on its "
            "changed-files line (#3718)")

    # -- functional ---------------------------------------------------------

    def _snippet(self):
        block, _ = _slice(CYCLE_SH.read_text(), _TIDY_BASE_START,
                          _TIDY_BASE_END, "tidy base resolution")
        snippet = block[:-len(_TIDY_BASE_END)]
        self.assertIn("merge-base", snippet, "guard: snippet lacks the merge-base")
        self.assertIn("@{upstream}", snippet, "guard: snippet lacks the upstream read")
        return snippet

    def _git(self, repo, *args):
        env = _clean_env(repo)
        env.pop("GIT_CEILING_DIRECTORIES", None)   # the repo IS at `repo`
        env.update({"GIT_AUTHOR_NAME": "pin", "GIT_AUTHOR_EMAIL": "pin@invalid",
                    "GIT_COMMITTER_NAME": "pin", "GIT_COMMITTER_EMAIL": "pin@invalid",
                    "GIT_CONFIG_GLOBAL": os.devnull,
                    "GIT_CONFIG_SYSTEM": os.devnull})
        r = subprocess.run(("git",) + args, cwd=repo, env=env,
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0,
                         "git %s failed: %s%s" % (" ".join(args), r.stdout, r.stderr))
        return r.stdout.strip()

    def _repo(self, tmp):
        """A repo shaped exactly like the measured defect: local `main` LEFT
        BEHIND at A, `origin/main` at B, HEAD on a branch at C that tracks
        origin/main. Returns (A, B, C)."""
        repo = pathlib.Path(tmp) / "r"
        repo.mkdir()
        self._git(repo, "init", "-q", "-b", "main", ".")
        shas = []
        for msg in ("a", "b", "c"):
            self._git(repo, "commit", "-q", "--allow-empty", "-m", msg)
            shas.append(self._git(repo, "rev-parse", "HEAD"))
        a, b, c = shas
        self._git(repo, "update-ref", "refs/remotes/origin/main", b)
        # `topic` first: git refuses to force a branch a worktree has checked out.
        self._git(repo, "checkout", "-q", "-B", "topic", c)
        self._git(repo, "branch", "-f", "main", a)
        self._git(repo, "config", "branch.topic.remote", "origin")
        self._git(repo, "config", "branch.topic.merge", "refs/heads/main")
        # Non-vacuity: the two bases really do differ in this repo, so a pin
        # that passes here cannot be passing because they coincide.
        self.assertEqual(self._git(repo, "merge-base", "HEAD", "main"), a,
                         "guard: stale local `main` does not give the old answer")
        self.assertEqual(self._git(repo, "merge-base", "HEAD", "origin/main"), b,
                         "guard: origin/main does not give the new answer")
        self.assertNotEqual(a, b, "guard: the two bases coincide")
        return repo, a, b, c

    def _resolve(self, repo, strict=False):
        env = _clean_env(str(repo))
        env.pop("GIT_CEILING_DIRECTORIES", None)
        env["PIN_REPO"] = str(repo)
        env["GIT_CONFIG_GLOBAL"] = os.devnull
        env["GIT_CONFIG_SYSTEM"] = os.devnull
        r = _bash(_BASE_HARNESS % {
            "snippet": self._snippet(),
            "setopts": "set -eu -o pipefail" if strict else "set -u"},
            str(repo), env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        out = dict(ln.split("=", 1) for ln in r.stdout.splitlines() if "=" in ln)
        return out, r

    def test_base_is_the_upstream_tip_not_the_stale_local_main(self):
        for strict in (False, True):
            with self.subTest(strict=strict):
                with tempfile.TemporaryDirectory() as tmp:
                    repo, a, b, _c = self._repo(tmp)
                    out, r = self._resolve(repo, strict=strict)
                    self.assertEqual(out.get("TIDY_UP"), "origin/main", r.stdout)
                    self.assertEqual(
                        out.get("TIDY_BASE"), b,
                        "the advisory tidy base must be the merge-base with the "
                        "branch's upstream (%s), not with the stale local `main` "
                        "(%s) — #3718:\n%s" % (b[:7], a[:7], r.stdout))

    def test_base_falls_back_to_origin_main_without_an_upstream(self):
        """A lane branch created with `git branch <n> <sha>` has no upstream at
        all; `origin/main` is still the right base, and `main` still is not."""
        with tempfile.TemporaryDirectory() as tmp:
            repo, a, b, _c = self._repo(tmp)
            self._git(repo, "config", "--unset", "branch.topic.remote")
            self._git(repo, "config", "--unset", "branch.topic.merge")
            out, r = self._resolve(repo)
            self.assertEqual(out.get("TIDY_UP"), "origin/main", r.stdout)
            self.assertEqual(out.get("TIDY_BASE"), b,
                             "no upstream must fall back to origin/main, not to "
                             "the stale local `main` (%s):\n%s" % (a[:7], r.stdout))

    def test_base_falls_back_when_the_upstream_has_no_tracking_ref(self):
        """`branch.<b>.merge` can name a remote branch this clone has never
        fetched: git then fails the `@{upstream}` read outright. That must not
        leave the base empty, and must not fall through to `main`."""
        with tempfile.TemporaryDirectory() as tmp:
            repo, a, b, _c = self._repo(tmp)
            self._git(repo, "config", "branch.topic.merge", "refs/heads/nope")
            out, r = self._resolve(repo)
            self.assertEqual(out.get("TIDY_UP"), "origin/main", r.stdout)
            self.assertEqual(out.get("TIDY_BASE"), b,
                             "an unfetched upstream must fall back to origin/main "
                             "(not %s):\n%s" % (a[:7], r.stdout))

    def test_base_falls_back_to_head_1_with_no_upstream_and_no_origin_main(self):
        """The pre-existing last-resort, unchanged: a repo with no remote at
        all still gets a usable base rather than an empty one."""
        with tempfile.TemporaryDirectory() as tmp:
            repo, _a, _b, _c = self._repo(tmp)
            self._git(repo, "config", "--unset", "branch.topic.remote")
            self._git(repo, "config", "--unset", "branch.topic.merge")
            self._git(repo, "update-ref", "-d", "refs/remotes/origin/main")
            out, r = self._resolve(repo)
            self.assertEqual(out.get("TIDY_BASE"), "HEAD~1", r.stdout)

    def test_a_detached_head_resolves_without_erroring(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo, _a, b, c = self._repo(tmp)
            self._git(repo, "checkout", "-q", "--detach", c)
            out, r = self._resolve(repo, strict=True)
            self.assertEqual(out.get("TIDY_UP"), "origin/main", r.stdout)
            self.assertEqual(out.get("TIDY_BASE"), b, r.stdout)


# ---------------------------------------------------------------------------
# 2c. fix #3725 (sibling of #3718, found during its review) — agent_check.sh's
#     `--select` default base independently spelled the SAME
#     `git merge-base HEAD main` defect: the bare local `main`, which nothing
#     in the run workflow fast-forwards.
# ---------------------------------------------------------------------------

_SELECT_BASE_START = '_up="$(git -C "$ROOT" rev-parse'
# The resolution lives inside an `if [ -z "$base" ]; then … fi` guard (an
# explicit `--select <ref>` must skip it — shape requirement of #3725), so
# the end sentinel is that guard's OWN closing `fi` (2-space indent, distinct
# from the resolution's own inner `if … fi` pairs at 4-space indent) rather
# than the next statement, which would leave the extracted snippet with an
# unbalanced trailing `fi`.
_SELECT_BASE_END = "\n  fi\n"

_SELECT_BASE_HARNESS = r'''
%(setopts)s
ROOT="$PIN_REPO"
%(snippet)s
echo "SELECT_UP=$_up"
echo "SELECT_BASE=$base"
'''


class AgentCheckSelectBaseIsUpstreamTest(unittest.TestCase):
    """#3725. `scripts/agent_check.sh --select`'s default base resolved
    `git merge-base HEAD main` against the LOCAL `main` branch — the same
    defect fix #3718 removed from gate_full_cycle.sh, spelled a second,
    independent time. `scripts/select_tests.py` (~377, not owned by this
    fix) already resolves ITS OWN default base against `origin/main`, so the
    two scripts silently disagreed with each other on every `--select` run.
    Measured 2026-09-03 in a lane worktree with no upstream configured for
    the lane branch (so both bases fall through their own fallback chains):
    10628 tests selected against the stale local `main` (ed5f8148b) versus
    6022 against `origin/main` (98931b5e6) for the SAME HEAD — a lane check
    running ~1.8x the intended subset (the issue measured 9424 vs 3225 at
    61b146c; the ratio drifts as the branch advances since local `main` was
    last moved, the defect itself does not)."""

    # -- source -------------------------------------------------------------

    def test_select_base_resolution_names_the_upstream_and_the_fallback(self):
        block, _ = _slice(AGENT_CHECK_SH.read_text(), _SELECT_BASE_START,
                          _SELECT_BASE_END, "--select base resolution")
        # CODE only: the block's own comment recounts the defect verbatim
        # (including the string `origin/main`), so a whole-text search would
        # stay green even over a reverted resolution.
        code = "\n".join(ln for _n, ln in _code_lines(block))
        self.assertIn("@{upstream}", code,
                      "the --select base must be resolved from the branch's "
                      "own upstream (#3725)")
        self.assertIn("origin/main", code,
                      "the --select base needs an `origin/main` fallback for "
                      "a detached HEAD or a lane branch with no upstream "
                      "(#3725)")
        self.assertIn("merge-base HEAD", code,
                      "guard: the base is still a merge-base against that ref")

    def test_explicit_base_argument_skips_the_upstream_resolution(self):
        """Shape requirement: `agent_check.sh --select <ref>` must still use
        exactly that ref — the resolution only fires when no ref was passed."""
        text = AGENT_CHECK_SH.read_text()
        block, _ = _slice(text, 'base="${1:-}"', _SELECT_BASE_END,
                          "--select argument handling")
        self.assertIn('if [ -z "$base" ]; then', block,
                      "the upstream resolution must be gated on an EMPTY "
                      "explicit base (#3725), so `--select <ref>` is unaffected")

    # -- functional -----------------------------------------------------
    # Reuses #3718's repo/git fixture verbatim (same shape: local `main` LEFT
    # BEHIND at A, `origin/main` at B, HEAD on a branch at C) rather than
    # duplicating it: the two harnesses differ only in the shell variable
    # names each snippet happens to use (`_tidy_up`/`_tidy_base` there,
    # `_up`/`base` here), which the harness template already isolates.

    _git = CycleShTidyBaseIsUpstreamTest._git
    _repo = CycleShTidyBaseIsUpstreamTest._repo

    def _snippet(self):
        block, _ = _slice(AGENT_CHECK_SH.read_text(), _SELECT_BASE_START,
                          _SELECT_BASE_END, "--select base resolution")
        snippet = block[:-len(_SELECT_BASE_END)]
        self.assertIn("merge-base", snippet, "guard: snippet lacks the merge-base")
        self.assertIn("@{upstream}", snippet, "guard: snippet lacks the upstream read")
        return snippet

    def _resolve(self, repo, strict=False):
        env = _clean_env(str(repo))
        env.pop("GIT_CEILING_DIRECTORIES", None)
        env["PIN_REPO"] = str(repo)
        env["GIT_CONFIG_GLOBAL"] = os.devnull
        env["GIT_CONFIG_SYSTEM"] = os.devnull
        r = _bash(_SELECT_BASE_HARNESS % {
            "snippet": self._snippet(),
            "setopts": "set -eu -o pipefail" if strict else "set -u"},
            str(repo), env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        out = dict(ln.split("=", 1) for ln in r.stdout.splitlines() if "=" in ln)
        return out, r

    def test_base_is_the_upstream_tip_not_the_stale_local_main(self):
        for strict in (False, True):
            with self.subTest(strict=strict):
                with tempfile.TemporaryDirectory() as tmp:
                    repo, a, b, _c = self._repo(tmp)
                    out, r = self._resolve(repo, strict=strict)
                    self.assertEqual(out.get("SELECT_UP"), "origin/main", r.stdout)
                    self.assertEqual(
                        out.get("SELECT_BASE"), b,
                        "the --select base must be the merge-base with the "
                        "branch's upstream (%s), not with the stale local "
                        "`main` (%s) — #3725:\n%s" % (b[:7], a[:7], r.stdout))

    def test_base_falls_back_to_origin_main_without_an_upstream(self):
        """The common lane shape: `git branch <n> <sha>` sets no upstream at
        all; `origin/main` is still the right base, and `main` still is not."""
        with tempfile.TemporaryDirectory() as tmp:
            repo, a, b, _c = self._repo(tmp)
            self._git(repo, "config", "--unset", "branch.topic.remote")
            self._git(repo, "config", "--unset", "branch.topic.merge")
            out, r = self._resolve(repo)
            self.assertEqual(out.get("SELECT_UP"), "origin/main", r.stdout)
            self.assertEqual(out.get("SELECT_BASE"), b,
                             "no upstream must fall back to origin/main, not "
                             "to the stale local `main` (%s):\n%s" % (a[:7], r.stdout))

    def test_base_falls_back_when_the_upstream_has_no_tracking_ref(self):
        """`branch.<b>.merge` can name a remote branch this clone has never
        fetched: git then fails the `@{upstream}` read outright. That must
        not leave the base empty, and must not fall through to `main`."""
        with tempfile.TemporaryDirectory() as tmp:
            repo, a, b, _c = self._repo(tmp)
            self._git(repo, "config", "branch.topic.merge", "refs/heads/nope")
            out, r = self._resolve(repo)
            self.assertEqual(out.get("SELECT_UP"), "origin/main", r.stdout)
            self.assertEqual(out.get("SELECT_BASE"), b,
                             "an unfetched upstream must fall back to "
                             "origin/main (not %s):\n%s" % (a[:7], r.stdout))

    def test_base_falls_back_to_head_1_with_no_upstream_and_no_origin_main(self):
        """The pre-existing last-resort, unchanged: a repo with no remote at
        all still gets a usable base rather than an empty one."""
        with tempfile.TemporaryDirectory() as tmp:
            repo, _a, _b, _c = self._repo(tmp)
            self._git(repo, "config", "--unset", "branch.topic.remote")
            self._git(repo, "config", "--unset", "branch.topic.merge")
            self._git(repo, "update-ref", "-d", "refs/remotes/origin/main")
            out, r = self._resolve(repo)
            self.assertEqual(out.get("SELECT_BASE"), "HEAD~1", r.stdout)

    def test_a_detached_head_resolves_without_erroring(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo, _a, b, c = self._repo(tmp)
            self._git(repo, "checkout", "-q", "--detach", c)
            out, r = self._resolve(repo, strict=True)
            self.assertEqual(out.get("SELECT_UP"), "origin/main", r.stdout)
            self.assertEqual(out.get("SELECT_BASE"), b, r.stdout)


# ---------------------------------------------------------------------------
# 3. run_until_trustworthy exports attempt / oracle / canary to gate.sh.
# ---------------------------------------------------------------------------

_STUB_GATE_SH = r'''#!/usr/bin/env bash
# Stub gate.sh for test_gate_full_cycle_pins (#3666): record the three
# variables the cycle exports, then write a REAL verdict via gate.verdict —
# rc 3 (coverage short -> retry) on the first call, rc 0 (pass) after. The
# call number comes from the record file, NOT from BEZEL_GATE_ATTEMPT, so the
# loop terminates the same way on a tree that does not export it.
printf '%s %s %s\n' "${BEZEL_GATE_ATTEMPT:-}" "${BEZEL_GATE_ORACLE:-}" "${GATE_CANARY:-}" >> "$PIN_RECORD"
n=$(wc -l < "$PIN_RECORD")
rc=0; [ "$n" -eq 1 ] && rc=3
PYTHONPATH=scripts python3 -m gate.verdict write --path "$GATE_VERDICT_FILE" --rc "$rc" || exit 1
echo "stub gate.sh: call $n args=[$*] rc=$rc"
exit "$rc"
'''

_LOOP_HARNESS = r'''
set -u
cd "$PIN_TMP"
%(operator_env)s
say() { :; }
sleep() { :; }
CYCLE_LOG=/dev/null
MAX_ATTEMPTS=3
%(legacy)s
%(loop)s
run_until_trustworthy "$PIN_TMP/pin.log" "%(mode)s" %(args)s
echo "RUN_UNTIL_TRUSTWORTHY_RC=$?"
'''


class RunUntilTrustworthyExportsTest(unittest.TestCase):
    """Drive the REAL run_until_trustworthy() (extracted verbatim) against a
    stub gate.sh and read back what each attempt saw."""

    def _functions(self):
        text = CYCLE_SH.read_text()
        legacy, _ = _slice(text, "_legacy_rc_decision() {", "\n}\n", "_legacy_rc_decision")
        loop, _ = _slice(text, "run_until_trustworthy() {", "\n  return 99\n}\n",
                         "run_until_trustworthy")
        # Structural controls: we got the real bodies, not fragments.
        self.assertIn('scripts/gate.sh "$@"', loop)
        self.assertIn("gate.cycle classify", loop)
        self.assertIn("accept)", loop)
        self.assertIn("echo bail", legacy)
        return legacy, loop

    def _run(self, mode, args, operator_env=""):
        legacy, loop = self._functions()
        with tempfile.TemporaryDirectory() as tmp:
            sd = _layout(tmp)
            stub = sd / "gate.sh"
            stub.write_text(_STUB_GATE_SH)
            stub.chmod(0o755)
            rec = pathlib.Path(tmp) / "record.txt"
            env = _clean_env(tmp)
            env["PIN_TMP"] = tmp
            env["PIN_RECORD"] = str(rec)
            script = _LOOP_HARNESS % {
                "operator_env": operator_env, "legacy": legacy, "loop": loop,
                "mode": mode, "args": args}
            r = _bash(script, tmp, env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("RUN_UNTIL_TRUSTWORTHY_RC=0", r.stdout,
                          "the loop must ACCEPT on the stub's second (rc 0) "
                          "attempt:\n" + r.stdout + r.stderr)
            records = rec.read_text().splitlines() if rec.exists() else []
            # Evidence the real loop ran its real paths: the retry branch
            # copied attempt 1's log aside (#3001), and both attempts wrote a
            # verdict through gate.verdict that reads back as retry / pass.
            self.assertTrue((pathlib.Path(tmp) / "pin.attempt1.log").exists(),
                            "attempt 1 must have taken the RETRY branch (log kept aside)")
            v1 = json.loads((pathlib.Path(tmp) / "pin.attempt1.verdict.json").read_text())
            v2 = json.loads((pathlib.Path(tmp) / "pin.attempt2.verdict.json").read_text())
            self.assertEqual(v1["verdict"], "retry")
            self.assertEqual(v2["verdict"], "pass")
            return records

    def test_plain_pass_exports_attempt_oracle_and_canary_on(self):
        self.assertEqual(
            self._run("plain", ""), ["1 plain 1", "2 plain 1"],
            "each PLAIN attempt must see BEZEL_GATE_ATTEMPT=<i> "
            "BEZEL_GATE_ORACLE=plain GATE_CANARY=1 (fix #3666)")

    def test_full_pass_exports_asan_and_canary_off(self):
        self.assertEqual(
            self._run("full(ASan)", "--full"), ["1 asan 0", "2 asan 0"],
            "each --full attempt must see BEZEL_GATE_ORACLE=asan and "
            "GATE_CANARY=0 — the canary caught nothing in 46 runs and costs "
            "a median 184 s on the ASan oracle (fix #3666)")

    def test_operator_canary_opt_out_is_honoured_on_plain(self):
        """GATE_CANARY=0 is a documented gate.sh knob. The cycle must pass an
        operator's opt-out through, not clobber it with its own 1."""
        self.assertEqual(
            self._run("plain", "", operator_env="export GATE_CANARY=0"),
            ["1 plain 0", "2 plain 0"])

    def test_operator_canary_on_cannot_reenable_it_under_asan(self):
        self.assertEqual(
            self._run("full(ASan)", "--full", operator_env="export GATE_CANARY=1"),
            ["1 asan 0", "2 asan 0"],
            "the ASan-oracle canary is off by decision (#3666), not by default")


# ---------------------------------------------------------------------------
# 4. gate.sh's side: the GATE_CANARY=0 knob and the attempt on the ceiling line.
# ---------------------------------------------------------------------------

_CANARY_HARNESS = r'''
set -u
cd "$PIN_TMP"
S="$PIN_TMP"
DH="$PIN_TMP/dh_stub"
rec_skip() { echo "REC_SKIP $*"; }
rec_ran() { echo "REC_RAN $*"; }
now() { echo 0; }
fail() { echo "FAIL $*"; exit 1; }
gate_arbitrate() { echo "ARB $*"; return 0; }
ASAN=0
GATE_CANARY=%(canary)s
%(block)s
echo "CANARY_BLOCK_DONE"
'''


class GateShCanaryKnobTest(unittest.TestCase):
    """The cycle now leans on gate.sh's GATE_CANARY=0 path for every --full
    attempt, and greps the cycle log for its exact line."""

    def _block(self):
        text = GATE_SH.read_text()
        start = 'CANARY_LIST="$S/gate_canary.list"\n'
        knob = 'if [ "${GATE_CANARY:-1}" = "0" ]; then\n'
        i = text.find(start)
        self.assertGreaterEqual(i, 0, "canary block start not found in gate.sh")
        k = text.find(knob, i)
        self.assertGreaterEqual(k, 0, "the GATE_CANARY=0 knob is gone from gate.sh")
        e = text.find("\nfi\n", k)
        self.assertGreaterEqual(e, 0)
        block = text[i:e + len("\nfi\n")]
        # Structural controls: the real dispatch, not a fragment.
        for needle in ("run_parallel.sh", "select_tests.py", "_canary_dual", "canary_skip()"):
            self.assertIn(needle, block, "guard: canary block lacks %s" % needle)
        return block

    def _run(self, canary):
        with tempfile.TemporaryDirectory() as tmp:
            _layout(tmp)
            dh = pathlib.Path(tmp) / "dh_stub"
            dh.write_text("#!/usr/bin/env bash\ntouch \"$PIN_TMP/DH_WAS_CALLED\"\nexit 0\n")
            dh.chmod(0o755)
            env = _clean_env(tmp)
            env["PIN_TMP"] = tmp
            r = _bash(_CANARY_HARNESS % {"canary": canary, "block": self._block()}, tmp, env)
            called = (pathlib.Path(tmp) / "DH_WAS_CALLED").exists()
            return r, called

    def test_gate_canary_zero_skips_with_the_exact_line(self):
        """GUARD: this knob pre-dates #3666 (#2100); the pin is the exact
        wording the cycle log is now read for, and that nothing runs."""
        r, called = self._run("0")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("CANARY_BLOCK_DONE", r.stdout, r.stdout + r.stderr)
        self.assertIn("(canary skipped: GATE_CANARY=0)", r.stdout,
                      "gate.sh must log exactly `canary skipped: GATE_CANARY=0` "
                      "on the --full oracle:\n" + r.stdout)
        self.assertIn("REC_SKIP canary GATE_CANARY=0", r.stdout,
                      "telemetry must record the canary phase as skipped with "
                      "that reason (rec_skip), so the 29-key record is unchanged")
        self.assertNotIn("REC_RAN", r.stdout, "nothing may RUN on the skip path")
        self.assertFalse(called, "the sharded runner must not be launched on the skip path")

    def test_gate_canary_one_takes_a_different_path(self):
        """Negative control for the harness: with the knob ON, the same block
        goes elsewhere (here: no base ref in a bare temp dir -> selector
        fail-open skip, through the real gate.canary decide)."""
        r, _ = self._run("1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("CANARY_BLOCK_DONE", r.stdout, r.stdout + r.stderr)
        self.assertNotIn("GATE_CANARY=0", r.stdout)
        self.assertIn("canary skipped: no base", r.stdout, r.stdout)

    def test_ceiling_line_names_the_attempt_and_keeps_asan_evidence(self):
        text = GATE_SH.read_text()
        lines = _code_lines(text)
        ceiling = [(n, ln) for n, ln in lines if "shard ceiling:" in ln and ln.lstrip().startswith("echo")]
        self.assertEqual(len(ceiling), 1, "guard: one shard-ceiling echo expected")
        n, ln = ceiling[0]
        self.assertIn("attempt=${BEZEL_GATE_ATTEMPT:-standalone}", ln,
                      "gate.sh:%d must append attempt=${BEZEL_GATE_ATTEMPT:-standalone} "
                      "to the shard-ceiling line (fix #3666): %s" % (n, ln))
        # GUARD: on --full this line is the `asan|sanitize` evidence
        # gate.cycle assert-asan-ran greps the log for. With the tidy block
        # gone it must still carry that word through $_sto_label.
        self.assertIn("$_sto_label shard ceiling", ln)
        self.assertTrue(any('_sto_label="ASan"' in l for _, l in lines),
                        "gate.sh must set _sto_label=\"ASan\" on the ASan pass — "
                        "the ceiling line is the observed-evidence carrier")


# ---------------------------------------------------------------------------
# 5. fix #3667 / fix #3674: the ceiling-only RETRY and its bound, the
#    shard_times harvest, and `actual FAIL` in the condensed shard log.
# ---------------------------------------------------------------------------

# The phrase gate.sh prints on the ceiling-only path and gate_full_cycle.sh
# greps to count those retries. One spelling, pinned on both sides.
_CEIL_PHRASE = "ceiling-killed shard(s) only"
_RC_BLOCK_START = 'if [ "$rc" -ne 0 ]; then\n'
_ARB_CALL = 'gate_arbitrate "$S/gate_shard.log"'


def _cycle_functions(tc):
    """The REAL _legacy_rc_decision() and run_until_trustworthy() bodies,
    extracted verbatim from gate_full_cycle.sh by sentinel, with the same
    structural controls RunUntilTrustworthyExportsTest applies."""
    text = CYCLE_SH.read_text()
    legacy, _ = _slice(text, "_legacy_rc_decision() {", "\n}\n", "_legacy_rc_decision")
    loop, _ = _slice(text, "run_until_trustworthy() {", "\n  return 99\n}\n",
                     "run_until_trustworthy")
    tc.assertIn('scripts/gate.sh "$@"', loop)
    tc.assertIn("gate.cycle classify", loop)
    tc.assertIn("accept)", loop)
    tc.assertIn("echo bail", legacy)
    return legacy, loop


_STUB_GATE_SH_CEILING = r"""#!/usr/bin/env bash
# Stub gate.sh for the #3667 retry-bound pins: every call is a ceiling-only
# retry — print the line gate.sh prints on that path, write a REAL rc=3
# verdict through gate.verdict, exit 3. PIN_GREEN_AFTER=<n> makes every call
# after the n-th a clean pass instead (the negative control). The call number
# comes from the record file, not from BEZEL_GATE_ATTEMPT.
printf '%s\n' "${BEZEL_GATE_ATTEMPT:-}" >> "$PIN_RECORD"
n=$(wc -l < "$PIN_RECORD")
rc=3
if [ -n "${PIN_GREEN_AFTER:-}" ] && [ "$n" -gt "$PIN_GREEN_AFTER" ]; then rc=0; fi
if [ "$rc" -eq 3 ]; then
  echo "   shard_arbitration rc=3 (ceiling-killed only)"
  echo "== gate: RETRY — sharded suite: ceiling-killed shard(s) only, no FAIL/crash (see $PIN_TMP/gate_shard.log) =="
fi
PYTHONPATH=scripts python3 -m gate.verdict write --path "$GATE_VERDICT_FILE" --rc "$rc" || exit 1
exit "$rc"
"""

_CEIL_LOOP_HARNESS = r"""
set -u
cd "$PIN_TMP"
say() { echo "SAY $*"; }
sleep() { :; }
CYCLE_LOG=/dev/null
MAX_ATTEMPTS=%(max_attempts)d
%(legacy)s
%(loop)s
run_until_trustworthy "$PIN_TMP/pin.log" "plain"
echo "RUN_UNTIL_TRUSTWORTHY_RC=$?"
"""


class CeilingOnlyRetryBoundTest(unittest.TestCase):
    """gate_full_cycle.sh run_until_trustworthy: a ceiling-only retry (gate.sh
    exit 3 carrying the `ceiling-killed shard(s) only` line) is retried twice
    and BAILed (return 1) on the third. MAX_ATTEMPTS is 40: a machine that
    cannot finish a shard inside the ceiling must not grind through 40
    warm-cache re-runs of the whole suite before anyone hears about it."""

    def _run(self, green_after=None, max_attempts=5):
        legacy, loop = _cycle_functions(self)
        with tempfile.TemporaryDirectory() as tmp:
            sd = _layout(tmp)
            stub = sd / "gate.sh"
            stub.write_text(_STUB_GATE_SH_CEILING)
            stub.chmod(0o755)
            rec = pathlib.Path(tmp) / "record.txt"
            env = _clean_env(tmp)
            env["PIN_TMP"] = tmp
            env["PIN_RECORD"] = str(rec)
            if green_after is not None:
                env["PIN_GREEN_AFTER"] = str(green_after)
            r = _bash(_CEIL_LOOP_HARNESS % {
                "legacy": legacy, "loop": loop, "max_attempts": max_attempts},
                tmp, env)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            records = rec.read_text().splitlines() if rec.exists() else []
            verdicts = {}
            for i in range(1, max_attempts + 1):
                vf = pathlib.Path(tmp) / ("pin.attempt%d.verdict.json" % i)
                if vf.exists():
                    verdicts[i] = json.loads(vf.read_text())["verdict"]
            kept = sorted(p.name for p in pathlib.Path(tmp).glob("pin.attempt*.log"))
            return r.stdout, records, verdicts, kept

    def test_third_ceiling_only_retry_bails_with_rc_1(self):
        out, records, verdicts, kept = self._run()
        self.assertIn(
            "RUN_UNTIL_TRUSTWORTHY_RC=1", out,
            "the third ceiling-only retry must BAIL with return 1 (fix #3667); "
            "got:\n" + out)
        self.assertEqual(
            records, ["1", "2", "3"],
            "exactly three attempts — two retries and the bail — not "
            "MAX_ATTEMPTS (5 in this harness, 40 in production)")
        self.assertEqual(
            verdicts, {1: "retry", 2: "retry", 3: "retry"},
            "every attempt's verdict is the EXISTING rc=3 RETRY — never pass")
        self.assertEqual(
            kept, ["pin.attempt1.log", "pin.attempt2.log", "pin.attempt3.log"],
            "the evidence of every attempt is kept aside (#3001), the bailed "
            "one included")
        says = [ln for ln in out.splitlines() if ln.startswith("SAY ")]
        retries = [s for s in says if "CEILING-KILLED SHARD(S) ONLY" in s and "ceiling-only retry " in s]
        self.assertEqual(len(retries), 2,
                         "two ceiling-only retry lines expected:\n" + "\n".join(says))
        bail = [s for s in says if "CEILING-KILLED SHARD(S) ONLY" in s and "giving up" in s]
        self.assertEqual(len(bail), 1,
                         "one ceiling-only bail line expected:\n" + "\n".join(says))
        self.assertEqual(
            [s for s in says if "COVERAGE SHORT" in s], [],
            "a ceiling-only retry is not a coverage short and must not be "
            "labelled as one")
        self.assertEqual(
            [s for s in says if "no trustworthy run in" in s], [],
            "the ceiling-only bound, not MAX_ATTEMPTS, must end the loop")

    def test_two_ceiling_only_retries_then_green_is_accepted(self):
        """GUARD: the bound is three, not two — a run that clears on its
        third attempt is accepted exactly as any retry->pass sequence is.
        (Passes at e9a10c7 too: there the two retries are labelled COVERAGE
        SHORT, and the third attempt is still accepted.)"""
        out, records, verdicts, kept = self._run(green_after=2)
        self.assertIn("RUN_UNTIL_TRUSTWORTHY_RC=0", out, out)
        self.assertEqual(records, ["1", "2", "3"])
        self.assertEqual(verdicts, {1: "retry", 2: "retry", 3: "pass"})
        self.assertEqual(kept, ["pin.attempt1.log", "pin.attempt2.log"],
                         "only the distrusted attempts are copied aside")


class GateShCeilingOnlyWiringTest(unittest.TestCase):
    """Source pins on gate.sh's shard phase: the classifier is consulted
    before arbitration with BOTH log inputs, the only exit it can add is the
    existing rc=3, and the shard_times harvest is the CLI."""

    def _rc_block(self):
        text = GATE_SH.read_text()
        block, _ = _slice(text, _RC_BLOCK_START, "\nfi\n", "rc!=0 block")
        self.assertIn(_ARB_CALL, block, "guard: the rc!=0 block must still arbitrate")
        return block

    def test_ceiling_only_classifier_runs_before_arbitration_with_both_logs(self):
        block = self._rc_block()
        code = "\n".join(ln for _, ln in _code_lines(block))
        self.assertEqual(
            code.count("run-summary ceiling-only"), 1,
            "gate.sh's rc!=0 block must call `python3 -m gate.summary "
            "run-summary ceiling-only` exactly once (fix #3667):\n" + code)
        self.assertLess(
            code.find("run-summary ceiling-only"), code.find(_ARB_CALL),
            "the classifier must run BEFORE gate_arbitrate: arbitration finds "
            "no marker in a ceiling-killed run's log and returns 2 -> FAIL -> BAIL")
        self.assertIn('--shard-log "$S/gate_shard.log"', code,
                      "the gate log must be passed as --shard-log")
        self.assertIn("--shard-logs-dir", code,
                      "the kept per-shard logdir must be passed as --shard-logs-dir "
                      "(the gate log alone misses a [ FAIL ] a shard printed before "
                      "run_parallel's early exit — sharpen #3668 case (a))")
        self.assertIn("rec_ran shard_arbitration 3", code,
                      "telemetry must record the ceiling-only outcome as rc=3")
        self.assertIn(_CEIL_PHRASE, code,
                      "the retry line must carry the phrase gate_full_cycle.sh counts")
        self.assertIn("CANNOT CLASSIFY", code,
                      "the classifier's exit 2 must be said aloud — it is not "
                      "collapsed into 1 (reviewer guidance, c1-b)")

    def test_the_only_exit_the_block_can_take_is_3_after_setting_failed_phase(self):
        block = self._rc_block()
        lines = _code_lines(block)
        exits = [(n, ln) for n, ln in lines if re.search(r"\bexit\s+\S+", ln)]
        codes = sorted({re.search(r"\bexit\s+(\S+)", ln).group(1).rstrip(";")
                        for _, ln in exits})
        self.assertEqual(
            codes, ["3"],
            "the rc!=0 block may add ONE verdict, rc=3 = the existing RETRY "
            "contract (verdict.py verdict_from_rc); found exits: %r" % exits)
        phase = [n for n, ln in lines if ln.lstrip().startswith("FAILED_PHASE=") and _CEIL_PHRASE in ln]
        self.assertEqual(len(phase), 1, "FAILED_PHASE must name the ceiling-only retry")
        self.assertLess(phase[0], exits[0][0],
                        "FAILED_PHASE must be set BEFORE exit 3 — the EXIT trap "
                        "writes the verdict from it")

    def test_shard_times_harvest_uses_the_cli(self):
        lines = [(n, ln) for n, ln in _code_lines(GATE_SH.read_text())
                 if ln.startswith("SHARD_TIMES=$(")]
        self.assertEqual(len(lines), 1, "guard: one SHARD_TIMES harvest line")
        n, ln = lines[0]
        self.assertIn(
            "gate.summary shard-times", ln,
            "gate.sh:%d must harvest shard_times via `python3 -m gate.summary "
            "shard-times` — the grep it replaces was anchored `s$` and has "
            "matched nothing since d0d2390 (2026-08-18) added run_parallel's "
            "` (ceiling 1800s, 0.7%%)` suffix: 690 telemetry records carry "
            "shard_times data, the 188 written since are all [] "
            "(narrow #3667): %s" % (n, ln))
        self.assertNotIn("[0-9.]+s$", ln, "the anchored regex must be gone")

    def test_run_summary_is_removed_before_the_shard_launch(self):
        """sharpen (review of 6102402): GATE_RUN_SUMMARY is a FIXED path
        (/var/tmp/bezel-scratch/gate_run_summary.json) every gate overwrites.
        If run_parallel dies before `account` writes it (run_parallel.sh:233),
        the ceiling-only classifier — and the coverage harvest after it —
        would read the PREVIOUS gate's summary and classify a run that is not
        this one. The launch must be preceded by an rm, exactly as
        MEMPEAK_OUT is (`never source a PRIOR gate's numbers`)."""
        lines = _code_lines(GATE_SH.read_text())
        assign = [n for n, ln in lines
                  if ln.startswith('GATE_RUN_SUMMARY="$S/gate_run_summary.json"')]
        rm = [n for n, ln in lines if re.match(r'\s*rm -f "\$GATE_RUN_SUMMARY"', ln)]
        launch = [n for n, ln in lines
                  if "tests/run_parallel.sh" in ln and '>"$S/gate_shard.log"' in ln]
        self.assertEqual(len(assign), 1, "guard: one GATE_RUN_SUMMARY assignment")
        self.assertEqual(len(launch), 1, "guard: one authoritative run_parallel launch")
        self.assertEqual(
            len(rm), 1,
            "gate.sh must `rm -f \"$GATE_RUN_SUMMARY\"` once before the shard "
            "launch — never classify a PRIOR gate's run summary (sharpen of "
            "#3667); found %r" % rm)
        self.assertLess(assign[0], rm[0], "the rm must follow the assignment (set -u)")
        self.assertLess(rm[0], launch[0],
                        "the rm (line %d) must precede the run_parallel launch "
                        "(line %d)" % (rm[0], launch[0]))


_CEIL_BLOCK_HARNESS = r"""
set -u
cd "$PIN_TMP"
S="$PIN_TMP/S"
GATE_RUN_SUMMARY="$S/gate_run_summary.json"
GATE_SHARD_STAMP="$S/gate_shard.stamp"
rec_ran() { echo "REC_RAN $*"; }
now() { echo 0; }
fail() { echo "FAIL $*"; exit 1; }
gate_arbitrate() { echo "ARB $*"; ARB_LAST_SUSPECTS=0; return 0; }
ASAN=0
ARB_SUSPECTS=0
FAILED_PHASE=""
rc=1
@@HELPER@@
@@BLOCK@@
echo "AFTER_RC_BLOCK failed_phase=[${FAILED_PHASE:-}]"
"""


class GateShCeilingOnlyFunctionalTest(unittest.TestCase):
    """Drive gate.sh's EXTRACTED rc!=0 block plus gate_shard_logdir() against
    the REAL gate.summary classifier over a fixture logdir, so the 0/1/2
    contract is exercised end to end: 0 -> exit 3 with the phrase and no
    arbitration; 1 -> arbitrate as before; 2 -> say CANNOT CLASSIFY and
    arbitrate as before."""

    def _fixture(self, tmp, *, kept_line=True, kept_fail=False,
                 gate_fail=False, summary=True, stale_fail_dir=False):
        S = pathlib.Path(tmp) / "S"
        S.mkdir()
        root = pathlib.Path(tmp) / "shard-logs"
        root.mkdir()
        stamp = S / "gate_shard.stamp"
        stamp.touch()
        now = time.time()
        os.utime(stamp, (now - 60, now - 60))          # touched before the run
        if stale_fail_dir:
            # A PREVIOUS gate's kept logs, older than the stamp, carrying a
            # [ FAIL ]. The measured shape: /var/tmp/bezel-scratch/shard-logs
            # held 249 kept runs on 2026-09-03, 217 of them with a [ FAIL ].
            old = root / "20260809-011922-rc1"
            old.mkdir()
            (old / "shard.codegen.0.log").write_text(
                "[ RUN  ] old.t\n[ FAIL ] old.t (1 ms)\nSHARD_ELAPSED 3.00\n")
            os.utime(old, (now - 120, now - 120))
        kept, keep_msg = self._keep_via_run_parallel(tmp, root, {
            "shard.codegen.0.log":
                "[ RUN  ] a.b\n"
                + ("[ FAIL ] a.b (11 ms)\n" if kept_fail else "")
                + "SHARD_ELAPSED 1782.00\n",
            "shard.codegen.1.log":
                "[ RUN  ] c.d\n[  OK  ] c.d\n5 passed, 0 failed (shard 1/2), 10 total\n"
                "SHARD_ELAPSED 12.34\n",
        })                                               # mtime = now > stamp
        lines = [
            "Running 1 test binaries x 2 slices = 2 shards, 2 at a time (of 16 cores)...",
            "shard codegen/0 TIMED OUT at the per-shard ceiling (BEZEL_SHARD_TIMEOUT=#3222)",
        ]
        if gate_fail:
            lines += ["--- failing tests ---", "[ FAIL ] a.b (11 ms)"]
        lines += ["shard codegen.0: 1795.50s (ceiling 1800s, 99.8%)",
                  "shard codegen.1: 12.34s (ceiling 1800s, 0.7%)"]
        if kept_line:
            # The producer's own line (run_parallel.sh's _rp_keep, run for real
            # above): it names the ROOT only, never the timestamped subdir.
            lines.append(keep_msg)
        (S / "gate_shard.log").write_text("\n".join(lines) + "\n")
        if summary:
            (S / "gate_run_summary.json").write_text(json.dumps({
                "schema": _summary.RUN_SCHEMA,
                "timed_out_shards": ["codegen/0"],
                "crashed_shards": [],
                "subset_empty": False,
                "missing_summary_shards": ["codegen/0"],
            }))
        return kept

    def _keep_via_run_parallel(self, tmp, root, logs):
        """Produce the kept logdir and the `KEPT in` line with
        tests/run_parallel.sh's REAL _rp_keep trap (extracted by sentinel and
        run with rc=1 and BEZEL_SHARD_LOG_KEEP=root), so the wording
        gate_shard_logdir() parses is the producer's own, not a copy of it —
        a drift there must fail these pins, not silently downgrade the
        classifier to a gate-log-only scan. Returns (kept_dir, line)."""
        keep_fn, _ = _slice(RP_SH.read_text(), "_rp_keep() {", "\n}\n", "_rp_keep")
        self.assertIn("BEZEL_SHARD_LOG_KEEP", keep_fn, "guard: not the keep trap")
        logdir = pathlib.Path(tmp) / "bezel_shards.pin"
        logdir.mkdir()
        for name, content in logs.items():
            (logdir / name).write_text(content)
        env = _clean_env(tmp)
        env["BEZEL_SHARD_LOG_KEEP"] = str(root)
        r = _bash("set -u\nlogdir=%s\n%s\nfalse\n_rp_keep\n"
                  % (shlex.quote(str(logdir)), keep_fn), tmp, env)
        self.assertEqual(r.returncode, 1, "guard: _rp_keep returns the rc it saw:\n" + r.stdout + r.stderr)
        lines = [ln for ln in r.stderr.splitlines() if "KEPT" in ln]
        self.assertEqual(len(lines), 1, "guard: _rp_keep announces the kept root once:\n" + r.stderr)
        subdirs = [p for p in root.iterdir() if p.is_dir() and p.name.endswith("-rc1")
                   and p.stat().st_mtime > time.time() - 30]
        self.assertEqual(len(subdirs), 1, "guard: _rp_keep moved the logdir under root once: %r"
                         % sorted(p.name for p in root.iterdir()))
        self.assertFalse(logdir.exists(), "guard: the original logdir was moved, not copied")
        return subdirs[0], lines[0]

    def _run(self, tmp):
        text = GATE_SH.read_text()
        helper, _ = _slice(text, "gate_shard_logdir() {", "\n}\n", "gate_shard_logdir")
        block, _ = _slice(text, _RC_BLOCK_START, "\nfi\n", "rc!=0 block")
        self.assertIn(_ARB_CALL, block, "guard: extracted block lacks the arbitration call")
        _layout(tmp)
        env = _clean_env(tmp)
        env["PIN_TMP"] = tmp
        script = _CEIL_BLOCK_HARNESS.replace("@@HELPER@@", helper).replace("@@BLOCK@@", block)
        return _bash(script, tmp, env)

    def test_ceiling_only_run_exits_3_with_the_phrase_and_never_arbitrates(self):
        with tempfile.TemporaryDirectory() as tmp:
            kept = self._fixture(tmp)
            r = self._run(tmp)
            self.assertEqual(r.returncode, 3,
                             "a ceiling-only run must take the rc=3 RETRY exit:\n" + r.stdout + r.stderr)
            self.assertIn("REC_RAN shard_arbitration 3 ceiling-killed only", r.stdout)
            self.assertIn(_CEIL_PHRASE, r.stdout,
                          "the retry line gate_full_cycle.sh counts is missing:\n" + r.stdout)
            self.assertIn(str(kept), r.stdout,
                          "the retry line must name the per-shard logdir it consulted")
            self.assertNotIn("ARB ", r.stdout, "arbitration must not run on the retry path")
            self.assertNotIn("AFTER_RC_BLOCK", r.stdout, "exit 3 must leave the block")

    def test_a_fail_only_in_the_kept_shard_log_is_not_retried(self):
        """sharpen #3668 case (a): the gate log is clean (run_parallel exited
        before its failing-tests block) and the [ FAIL ] lives only in the
        kept per-shard log. Only a located --shard-logs-dir can see it."""
        with tempfile.TemporaryDirectory() as tmp:
            self._fixture(tmp, kept_fail=True)
            r = self._run(tmp)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("ARB ", r.stdout, "not ceiling-only -> arbitrate as before")
            self.assertNotIn("REC_RAN shard_arbitration 3", r.stdout)
            self.assertNotIn(_CEIL_PHRASE, r.stdout)
            self.assertIn("AFTER_RC_BLOCK", r.stdout)

    def test_a_fail_in_the_gate_log_is_not_retried(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._fixture(tmp, gate_fail=True)
            r = self._run(tmp)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("ARB ", r.stdout)
            self.assertNotIn(_CEIL_PHRASE, r.stdout)

    def test_missing_run_summary_cannot_classify_says_so_and_arbitrates(self):
        """The classifier's exit 2 is a third answer: not retried, not
        silently treated as 'no' — said aloud, then today's path."""
        with tempfile.TemporaryDirectory() as tmp:
            self._fixture(tmp, summary=False)
            r = self._run(tmp)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("CANNOT CLASSIFY", r.stdout,
                          "exit 2 must be logged, not collapsed into 1:\n" + r.stdout)
            self.assertIn("ARB ", r.stdout)
            self.assertNotIn(_CEIL_PHRASE, r.stdout)
            self.assertIn("AFTER_RC_BLOCK", r.stdout)

    def test_no_kept_line_classifies_on_the_gate_log_alone_and_says_so(self):
        """No `per-shard logs KEPT in` line (the mv failed, or the logs were
        reaped): the flag is omitted and the log says so; the summary and
        the gate log still decide (gate.summary: reaped logs are no evidence,
        not a defect)."""
        with tempfile.TemporaryDirectory() as tmp:
            self._fixture(tmp, kept_line=False)
            r = self._run(tmp)
            self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
            self.assertIn("per-shard logs not located", r.stdout,
                          "omitting --shard-logs-dir must be said in the log:\n" + r.stdout)
            self.assertIn(_CEIL_PHRASE, r.stdout)
            self.assertNotIn("ARB ", r.stdout)

    def test_a_previous_gates_kept_logs_are_not_this_runs_evidence(self):
        """run_parallel names only the ROOT (`…/shard-logs/`), which holds
        every non-clean run ever kept — 249 on this box, 217 with a
        [ FAIL ]. The block must hand the classifier THIS run's subdir (newer
        than the stamp), never the root or an older sibling: a stale FAIL
        would otherwise make every ceiling-only run 'not retryable'."""
        with tempfile.TemporaryDirectory() as tmp:
            kept = self._fixture(tmp, stale_fail_dir=True)
            r = self._run(tmp)
            self.assertEqual(r.returncode, 3,
                             "an OLDER kept run's [ FAIL ] must not veto this run's "
                             "retry:\n" + r.stdout + r.stderr)
            self.assertIn(str(kept), r.stdout)
            self.assertNotIn("20260809-011922-rc1", r.stdout)
            self.assertNotIn("ARB ", r.stdout)


class CondensedShardLogTest(unittest.TestCase):
    """fix #3674: gate.sh condenses the shard log for the reader with one
    grep alternation. Neither the #3018 `##   shard X actual FAIL: <test>`
    line nor the #3668 `… completed its slice; actual FAIL: <test>` line
    matched it, so a completed-shard failure showed the banner and the
    failing-tests header and named no test."""

    # Issue #3674's verify fixture plus the #3018 wording and a died-on line.
    FIXTURE = (
        "## GATE FAILURE: shard crash / undercount\n"
        "##   shard module/7 exited nonzero — completed its slice; actual FAIL: a.b\n"
        "##   shard sema/5 exited nonzero — died on: c.d\n"
        "##   shard sema/5 actual FAIL: old.t\n"
        "--- failing tests ---\n"
        "[ FAIL ] a.b (11 ms)\n"
        "TOTAL: 20148 passed\n")

    def _grep_line(self):
        lines = [(n, ln) for n, ln in _code_lines(GATE_SH.read_text())
                 if "grep -E" in ln and "## GATE FAILURE" in ln and "died on" in ln]
        self.assertEqual(len(lines), 1, "guard: one condensing grep expected in gate.sh")
        return lines[0]

    def test_alternation_carries_actual_fail(self):
        n, ln = self._grep_line()
        self.assertIn("actual FAIL", ln,
                      "gate.sh:%d must add `actual FAIL` to the condensing grep "
                      "(fix #3674): %s" % (n, ln))

    def test_the_3674_fixture_names_the_culprit(self):
        n, ln = self._grep_line()
        with tempfile.TemporaryDirectory() as tmp:
            (pathlib.Path(tmp) / "gate_shard.log").write_text(self.FIXTURE)
            r = _bash("S=%s\n%s\n" % (shlex.quote(tmp), ln.strip()), tmp, _clean_env(tmp))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("completed its slice; actual FAIL: a.b", r.stdout,
                          "the #3668 culprit line must survive condensation:\n" + r.stdout)
            self.assertIn("##   shard sema/5 actual FAIL: old.t", r.stdout,
                          "the #3018 culprit line must survive condensation")
            # GUARD: what already surfaced keeps surfacing.
            for keep in ("## GATE FAILURE: shard crash / undercount",
                         "died on: c.d", "--- failing tests ---", "TOTAL: 20148 passed"):
                self.assertIn(keep, r.stdout)


# ---------------------------------------------------------------------------
# 6. fix #3690: the gate stores without the #3009 fsync pair
#    (BEZEL_AOT_STORE_FSYNC=0, #3669) and logs that it does.
# ---------------------------------------------------------------------------

# The exact code line, the ASan branch's REASSIGNMENT it must follow, and the
# exact log line the next gate's log is read for.
_STORE_FSYNC_ADD = "SHARD_ENV+=(BEZEL_AOT_STORE_FSYNC=0)"
_ASAN_REASSIGN = "SHARD_ENV=(env BEZEL_AOT_FORCE_SANITIZE=address)"
_STORE_FSYNC_LOG = "store fsync: OFF (verify-on-read, #3669)"


class GateShStoreFsyncOffTest(unittest.TestCase):
    """Source pins on the flip (#3690), plus a bash REPLAY of the SHARD_ENV
    assignment lines in file order.

    The hazard the ordering pins exist for: gate.sh's ASan branch does not
    extend SHARD_ENV, it REASSIGNS it (`SHARD_ENV=(env BEZEL_AOT_FORCE_SANITIZE=
    address)`), so a `+=` placed in the block where SHARD_ENV is seeded would be
    silently dropped on every --full pass — the flag would look set in the
    source, the plain pass would skip its fsyncs, and the ASan pass (the one
    with ~27k stores) would keep paying them. The replay proves the flag
    survives BOTH oracles, and that `env` is still the array's first word
    (dev_heavy.sh ends in `exec systemd-run ... -- "$@"`, which execs the first
    word as a FILENAME).
    """

    def _lines(self):
        return _code_lines(GATE_SH.read_text())

    def _shard_env_lines(self):
        """Every CODE line that seeds or extends SHARD_ENV, in file order."""
        return [(n, ln) for n, ln in self._lines()
                if re.match(r"\s*SHARD_ENV\+?=\(", ln)]

    def _add_line(self):
        hits = [n for n, ln in self._shard_env_lines() if ln.strip() == _STORE_FSYNC_ADD]
        self.assertEqual(len(hits), 1,
                         "gate.sh must carry exactly one `%s` (fix #3690): %r"
                         % (_STORE_FSYNC_ADD, hits))
        return hits[0]

    def test_shard_env_carries_store_fsync_off_exactly_once(self):
        self._add_line()

    def test_store_fsync_off_follows_the_asan_reassignment(self):
        reassign = [n for n, ln in self._shard_env_lines()
                    if ln.strip().startswith(_ASAN_REASSIGN)]
        self.assertEqual(len(reassign), 1,
                         "guard: the ASan SHARD_ENV reassignment moved: %r" % reassign)
        self.assertGreater(self._add_line(), reassign[0],
                           "the += must come AFTER the ASan branch reassigns "
                           "SHARD_ENV, or --full silently drops it")

    def test_store_fsync_off_precedes_both_run_sites(self):
        # canary and main: every store the gate makes is in ONE mode, so
        # `store_fsyncs=0` on the [aot-cache] aggregate is a checkable criterion.
        sites = [n for n, ln in self._lines()
                 if '"${SHARD_ENV[@]}"' in ln and "tests/run_parallel.sh" in ln]
        self.assertEqual(len(sites), 2,
                         "guard: two SHARD_ENV run sites expected (canary, main): %r" % sites)
        add = self._add_line()
        for n in sites:
            self.assertLess(add, n, "gate.sh:%d runs the suite before the flag is added" % n)

    def test_replayed_shard_env_keeps_env_first_and_carries_the_flag_in_both_oracles(self):
        for asan in (0, 1):
            with self.subTest(asan=asan):
                lines = [ln for _, ln in self._shard_env_lines()
                         if asan or not ln.strip().startswith(_ASAN_REASSIGN)]
                script = ("set -u\nsto=1800\nGATE_RUN_SUMMARY=/dev/null\n"
                          + "\n".join(lines)
                          + '\nprintf "%s\\n" "${SHARD_ENV[@]}"\n')
                r = subprocess.run(["bash", "-c", script],
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)
                words = r.stdout.splitlines()
                self.assertEqual(words[:1], ["env"], words)
                self.assertEqual(words.count("BEZEL_AOT_STORE_FSYNC=0"), 1, words)
                self.assertEqual("BEZEL_AOT_FORCE_SANITIZE=address" in words,
                                 bool(asan), words)

    def test_ceiling_block_logs_store_fsync_off_after_the_flag_is_set(self):
        lines = self._lines()
        echo = [n for n, ln in lines
                if ln.lstrip().startswith("echo") and _STORE_FSYNC_LOG in ln]
        self.assertEqual(len(echo), 1,
                         "gate.sh must log exactly `%s` once" % _STORE_FSYNC_LOG)
        ceiling = [n for n, ln in lines
                   if "shard ceiling:" in ln and ln.lstrip().startswith("echo")]
        self.assertEqual(len(ceiling), 1)
        self.assertGreater(echo[0], ceiling[0], "the line belongs with the ceiling echo")
        self.assertGreater(echo[0], self._add_line(),
                           "the log must not claim a mode the array does not yet carry")
        # GUARD: the ASan-evidence grep is a case-insensitive `asan|sanitize`
        # (gate.cycle assert_asan_ran); this line prints on the PLAIN pass too
        # and must not manufacture that evidence.
        self.assertNotRegex(_STORE_FSYNC_LOG, r"(?i)asan|sanitize")


if __name__ == "__main__":
    unittest.main()
