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

Stdlib only (subprocess/tempfile/unittest) — the house style for this
directory. Every test here FAILED at the parent commit (af4f73b) except the
two marked GUARD in their docstrings, which pin a contract the change now
leans on (the GATE_CANARY=0 knob itself dates from #2100) rather than the
change itself.
"""

import json
import os
import pathlib
import re
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPTS = HERE.parent.parent          # scripts/
GATE_SH = SCRIPTS / "gate.sh"
CYCLE_SH = SCRIPTS / "gate_full_cycle.sh"
GATE_PKG = SCRIPTS / "gate"

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
set -u
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

    def _run_block(self, dev_heavy_stub, check_tidy_stub):
        """Source the tidy block in a temp cwd with stub `clang-tidy` /
        `run-clang-tidy` on PATH (so the main branch runs) and stub
        scripts/dev_heavy.sh + scripts/check_tidy.sh."""
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
            r = _bash(_TIDY_HARNESS % {"block": self._extract_block()}, tmp, env)
            cycle_log = pathlib.Path(tmp) / "logs" / "pin_cycle.log"
            cycle_text = cycle_log.read_text() if cycle_log.exists() else ""
            return r, cycle_text

    # dev_heavy stub that just runs its command (kind + cmd...), like the real one.
    _DH_PASSTHROUGH = "#!/usr/bin/env bash\nshift\nexec \"$@\"\n"

    def test_tidy_block_reports_elapsed_and_finding_count(self):
        r, cycle_text = self._run_block(
            self._DH_PASSTHROUGH,
            "#!/usr/bin/env bash\n"
            "echo \"check-tidy: changed-files mode vs $4 — 2 file(s)\"\n"
            "echo 'check-tidy: 3 finding(s) (ADVISORY — does not fail the build)'\n"
            "exit 0\n")
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
        r, _ = self._run_block(
            "#!/usr/bin/env bash\necho 'dev_heavy: refusing (stub)' >&2\nexit 5\n",
            "#!/usr/bin/env bash\nexit 0\n")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("AFTER_TIDY_BLOCK rc=0", r.stdout,
                      "a failing tidy tool ended the cycle — the phase is "
                      "ADVISORY (fix #3666):\n" + r.stdout + r.stderr)
        done = [ln for ln in r.stdout.splitlines() if "tidy phase: done in" in ln]
        self.assertEqual(len(done), 1, r.stdout)
        self.assertIn("no findings count", done[0],
                      "a run with no `check-tidy:` findings line must say so, "
                      "not report 0: " + done[0])


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


if __name__ == "__main__":
    unittest.main()
