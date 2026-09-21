"""#4533 — the sharded ASan-report hard-fail must be able to FIRE.

THE DEFECT. `scripts/gate.sh` treats a fired generated-code sanitizer report as
DETERMINISTIC evidence of a memory error and hard-fails the cycle on it,
independent of the REAL-vs-FLAKY arbitration. It looked for one by grepping
`$S/gate_shard.log` — which is `tests/run_parallel.sh`'s OWN STDOUT. A shard's
PROGRAM output never reaches that stream: every shard is redirected into a
per-shard file under run_parallel's mktemp logdir, and the only thing lifted
back into the parent stream was the `^[ FAIL ]` block. The grep was therefore
over a file that STRUCTURALLY CANNOT hold the pattern, and a grep that returns 0
for that reason is indistinguishable from a clean suite.

MEASURED, on the failed 2026-09-21 `--full` cycle at e89577e4:

    grep -cE 'ERROR: (Address|Leak)Sanitizer' \
        /var/tmp/bezel-scratch/gate-cycle/e89577e4_full.log          -> 0
    grep -achE 'ERROR: (Address|Leak)Sanitizer' \
        /var/tmp/bezel-scratch/shard-logs/20260921-153416-rc1/shard.*.log
                                       -> 306 lines across 12 shard logs
    grep -achE 'ERROR: AddressSanitizer' (same logs)  -> 7   (4 double-free,
                                                              3 SEGV)

and the cycle recorded `shard_asan  0.01s  rc=0 (no ASan report)`.

WHAT THESE TESTS PIN. A guard that cannot fire is exactly the defect being
fixed, so the wiring is pinned from BOTH ends and then end-to-end:

  1. LiftTest drives the REAL tests/run_parallel.sh with a stub binary that
     honours the same shard contract test_main.cpp does, and asserts the
     evidence survives — on a CLEAN run (where the logdir is reaped) as well as
     on a failing one (where it is kept).
  2. GateBlockTest extracts gate.sh's REAL ASan phase and runs it over the
     shapes it must decide: no accounting line (the pre-fix state — now a hard
     failure, not a silent pass), 0 markers, a corruption headline, a leak-only
     report, 0 logs scanned, an unparseable line.
  3. EndToEndTest feeds run_parallel's OWN accounting line, produced by the real
     script, into gate.sh's REAL block — so a drift in that literal fails here
     rather than silently re-inerting the channel.
  4. DeliberateReportCaptureTest pins the rows that deliberately print a
     sanitizer headline and redirect fd 2 so it cannot reach a shard log.
  5. TautologyGuardTest pins that gate.sh echoes the evidence line only under
     ASAN=1, so gate_full_cycle.sh's `grep -qi 'asan\\|sanitize'`
     silent-degradation detector is not turned into a tautology.
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
REPO = SCRIPTS.parent
GATE_SH = SCRIPTS / "gate.sh"
CYCLE_SH = SCRIPTS / "gate_full_cycle.sh"
RP_SH = REPO / "tests" / "run_parallel.sh"
DRS_TEST = REPO / "tests" / "support" / "driver_run_support_test.cpp"

# The LOAD-BEARING literal: run_parallel.sh prints it, gate.sh parses it.
EVIDENCE_PREFIX = "SANITIZER EVIDENCE (all shards):"
EVIDENCE_RE = re.compile(
    r"^SANITIZER EVIDENCE \(all shards\): (?P<hits>\d+) report marker\(s\) in "
    r"(?P<files>\d+) of (?P<scanned>\d+) shard log\(s\) scanned, "
    r"(?P<corrupt>\d+) corruption headline\(s\)$")

# A REAL clang/ASan report, captured verbatim from an instrumented program
# (`int *p = malloc(16); free(p); p[1] = 7;` built with -fsanitize=address),
# wrapped in the banner aot_support.hpp's emitSanitizerReportToStderr writes to
# fd 2 (#1822). Recorded rather than generated so the fixture is deterministic
# and needs no toolchain — the genuine end-to-end run is what LiftTest exercises
# through the real script.
ASAN_CORRUPTION_REPORT = """
===== [aot-asan #1822] sanitizer fired in an AOT test program =====
=================================================================
==3871325==ERROR: AddressSanitizer: heap-use-after-free on address 0x7b68c07e0014 at pc 0x4e9e9f bp 0x7fffe805cfb0 sp 0x7fffe805cfa8
WRITE of size 4 at 0x7b68c07e0014 thread T0
    #0 0x4e9e9e in main uaf.c:2:66
    #1 0x7f48c15f35b4 in __libc_start_call_main (/lib64/libc.so.6+0x35b4)
SUMMARY: AddressSanitizer: heap-use-after-free uaf.c:2:66 in main
===== [aot-asan #1822] end sanitizer report =====
"""

# A leak report has a DIFFERENT headline (`ERROR: LeakSanitizer`); its only
# `AddressSanitizer` mention is the `SUMMARY:` line, which is why the two
# classes never collide on the corruption pattern. 11 rows in 7 files assert a
# leak deliberately (`leaksAtLeast`) and re-emit exactly this shape while
# PASSING, so it must NOT fail a cycle.
LSAN_LEAK_REPORT = """
===== [aot-asan #1822] sanitizer fired in an AOT test program =====
=================================================================
==3414184==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 8248 byte(s) in 103 object(s) allocated from:
    #0 0x4c1d3d in malloc
SUMMARY: AddressSanitizer: 8248 byte(s) leaked in 103 allocation(s).
===== [aot-asan #1822] end sanitizer report =====
"""

# The stub stands in for bezel_tests_<area>: the same shard contract
# test_main.cpp honours (BEZEL_SHARD_COUNT / BEZEL_SHARD_INDEX, the structured
# BEZEL_SHARD_SUMMARY JSON, and the legacy "N passed, M failed ... T total"
# line the dual-read agreement check pairs it with). STUB_REPORT names a file
# whose contents test #1 writes to fd 2 — the harness's own re-emit path.
# STUB_FAIL makes test #1 FAIL, so the run exits nonzero and _rp_keep KEEPS the
# per-shard logs instead of reaping them.
STUB = r"""#!/usr/bin/env bash
set -u
c="${BEZEL_SHARD_COUNT:-1}"; i="${BEZEL_SHARD_INDEX:-0}"
total=2; passed=0; failed=0
for (( t = 0; t < total; t++ )); do
  (( t % c == i )) || continue
  echo "[ RUN  ] stub.t$t"
  if [[ "$t" -eq 1 && -n "${STUB_REPORT:-}" ]]; then cat "$STUB_REPORT" >&2; fi
  if [[ "$t" -eq 1 && -n "${STUB_FAIL:-}" ]]; then
    failed=$(( failed + 1 )); echo "[ FAIL ] stub.t$t"
  else
    passed=$(( passed + 1 )); echo "[  OK  ] stub.t$t"
  fi
done
if [[ -n "${BEZEL_SHARD_SUMMARY:-}" ]]; then
  printf '{"schema":"bezel.shard-summary.v1","passed":%d,"failed":%d,"total":%d,"selected":%d,"native_sanitizer_unavailable_skips":0,"checked_toolchain_skips":0}\n' \
    "$passed" "$failed" "$total" "$(( passed + failed ))" > "$BEZEL_SHARD_SUMMARY"
fi
echo
echo "$passed passed, $failed failed (shard $i/$c), $total total"
exit 0
"""

# gate.sh's ASan phase, extracted and run against stubbed gate primitives.
GATE_HARNESS = r"""
set -u
now() { echo 0; }
rec_ran() { echo "REC_RAN $1 $2 ${3:-}"; }
fail() { FAILED_PHASE="$1"; echo "GATE_FAIL: $1"; exit 1; }
S="$PIN_TMP"
ASAN=1
SHARD_COUNT=7
ASAN_ERRS=0; ASAN_SHARDS=0
PHASE_T0=0
rc=${PIN_RC:-0}
@@HELPER@@
@@BLOCK@@
echo "AFTER_ASAN_BLOCK ASAN_ERRS=$ASAN_ERRS ASAN_SHARDS=$ASAN_SHARDS"
"""

# The canary tier launches run_parallel too, so it carries the same channel —
# with the FAIL-OPEN posture its own design mandates.
CANARY_HARNESS = r"""
set -u
fail() { echo "GATE_FAIL: $1"; exit 1; }
_canary_dual() { echo "CANARY_DUAL $1"; }
S="$PIN_TMP"
ASAN=1
_c_asan=0
@@HELPER@@
@@BLOCK@@
echo "AFTER_CANARY_BLOCK _c_asan=$_c_asan"
"""

_HELPER_START = "gate_sanitizer_evidence() { # $1 = sharded-run log"
_HELPER_END = "\n}\n"

_ASAN_BLOCK_START = ('if [ "$ASAN" -eq 1 ]; then\n'
                     '  PHASE_T0=$(now)  # shard_asan is only the harvest')
_ASAN_BLOCK_END = ('    rec_ran shard_asan "0" "no ASan report '
                   '($asan_scanned shard log(s) scanned)"\n  fi\nfi\n')

_CANARY_BLOCK_START = ('      if [ "$ASAN" -eq 1 ]; then\n'
                       '        if _c_ev=$(gate_sanitizer_evidence')
_CANARY_BLOCK_END = "          _c_asan=0\n        fi\n      fi\n"


def _slice(text, start, end, what):
    idx = text.find(start)
    if idx < 0 or text.find(start, idx + 1) >= 0:
        raise AssertionError(
            "%s: start sentinel %r not found exactly once" % (what, start))
    e = text.find(end, idx + len(start))
    if e < 0:
        raise AssertionError("%s: end sentinel not found after start" % what)
    return text[idx:e + len(end)]


def _env(tmp):
    env = dict(os.environ)
    env["TMPDIR"] = tmp
    env["BEZEL_SHARD_LOG_KEEP"] = os.path.join(tmp, "keep")
    # The gate sets these; a runner's exported value must not leak in.
    for k in ("BEZEL_TEST_FILTER", "BEZEL_TEST_LIST", "BEZEL_RUN_SUMMARY",
              "BEZEL_SHARD_TIMEOUT", "BEZEL_AOT_FORCE_SANITIZE"):
        env.pop(k, None)
    return env


def _run_parallel(tmp, report=None, fail=False, conc=2):
    """Run the REAL tests/run_parallel.sh over `conc` slices of one stub
    binary. Returns the CompletedProcess."""
    stub = pathlib.Path(tmp) / "stub_bin.sh"
    stub.write_text(STUB)
    stub.chmod(0o755)
    env = _env(tmp)
    if report is not None:
        rp = pathlib.Path(tmp) / "report.txt"
        rp.write_text(report)
        env["STUB_REPORT"] = str(rp)
    else:
        env["STUB_REPORT"] = ""
    env["STUB_FAIL"] = "1" if fail else ""
    return subprocess.run(
        ["bash", str(RP_SH), str(conc), str(stub)],
        cwd=tmp, env=env, capture_output=True, text=True, timeout=180)


def _evidence(text):
    for ln in text.splitlines():
        m = EVIDENCE_RE.match(ln.strip())
        if m:
            return {k: int(v) for k, v in m.groupdict().items()}
    return None


class LiftTest(unittest.TestCase):
    """The producer half: run_parallel must lift the evidence out of the
    per-shard logs, on every run, before the logs are reaped."""

    def test_clean_run_prints_the_accounting_line_with_a_nonzero_scan(self):
        """The case the old grep could not be told apart from an unwired
        channel. `0 markers` is only meaningful next to `N logs SCANNED`."""
        with tempfile.TemporaryDirectory() as tmp:
            r = _run_parallel(tmp)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            ev = _evidence(r.stdout)
            self.assertIsNotNone(
                ev, "no %r line on a clean run:\n%s" % (EVIDENCE_PREFIX, r.stdout))
            self.assertEqual(ev["hits"], 0)
            self.assertEqual(ev["corrupt"], 0)
            self.assertEqual(ev["scanned"], 2,
                             "the scan must report how many shard logs it read")
            self.assertNotIn("--- sanitizer reports ---", r.stdout)

    def test_a_report_in_a_PASSING_test_of_a_CLEAN_run_is_lifted(self):
        """#4533's central case: the run exits 0 and every test passes, so
        neither the exit-code path nor the flake arbitration sees anything —
        and _rp_keep reaps the logdir, so nothing survives for a later reader.
        The lift is what makes the report visible at all."""
        with tempfile.TemporaryDirectory() as tmp:
            r = _run_parallel(tmp, report=ASAN_CORRUPTION_REPORT)
            self.assertEqual(r.returncode, 0,
                             "guard: the stub run is green — the report is the "
                             "ONLY signal:\n" + r.stdout + r.stderr)
            ev = _evidence(r.stdout)
            self.assertIsNotNone(ev, r.stdout)
            self.assertEqual(ev["files"], 1)
            self.assertEqual(ev["scanned"], 2)
            self.assertGreater(ev["hits"], 0)
            self.assertGreater(ev["corrupt"], 0,
                               "a heap-use-after-free headline is corruption")
            self.assertIn("--- sanitizer reports ---", r.stdout)
            self.assertIn("heap-use-after-free", r.stdout,
                          "the report text itself must reach the gate log")
            self.assertIn("shard.stub_bin.sh.1.log", r.stdout,
                          "the excerpt must name the shard log it came from")
            # The logdir was reaped (clean run) — the lift is the only copy.
            self.assertFalse(
                any(pathlib.Path(tmp).glob("bezel_shards.*")),
                "guard: a clean run reaps its logdir, so a later grep over it "
                "would find nothing — which is why the lift runs inline")

    def test_a_leak_report_is_counted_but_carries_no_corruption_headline(self):
        """11 rows in 7 files assert a leak deliberately (`leaksAtLeast`) and
        re-emit a real LSan report while PASSING. They must be counted and
        shown, and must NOT look like memory corruption."""
        with tempfile.TemporaryDirectory() as tmp:
            r = _run_parallel(tmp, report=LSAN_LEAK_REPORT)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            ev = _evidence(r.stdout)
            self.assertIsNotNone(ev, r.stdout)
            self.assertGreater(ev["hits"], 0)
            self.assertEqual(
                ev["corrupt"], 0,
                "`SUMMARY: AddressSanitizer: N byte(s) leaked` must not read "
                "as an `ERROR: AddressSanitizer` corruption headline")

    def test_a_corruption_report_leads_the_bounded_excerpt(self):
        """The excerpt is bounded (3 logs), and a --full run's leak reports
        outnumber its corruption ones by two orders of magnitude on real data
        (601 markers vs 7 corruption headlines in the kept logs of the failed
        e89577e4 cycle). The double-free must still be what the operator
        reads."""
        with tempfile.TemporaryDirectory() as tmp:
            r = _run_parallel(tmp, report=LSAN_LEAK_REPORT + ASAN_CORRUPTION_REPORT)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            section = r.stdout[r.stdout.index("--- sanitizer reports ---"):]
            self.assertIn("heap-use-after-free", section)
            self.assertLess(
                section.index("ERROR: AddressSanitizer"),
                section.index("Direct leak of") if "Direct leak of" in section
                else len(section),
                "a leak banner earlier in the same shard log must not displace "
                "the corruption report in the excerpt")

    def test_a_failing_run_still_lifts_and_still_keeps_its_logs(self):
        """The other direction: on rc!=0 _rp_keep MOVES the logdir aside. The
        lift must already have run, and must not disturb the keep."""
        with tempfile.TemporaryDirectory() as tmp:
            r = _run_parallel(tmp, report=ASAN_CORRUPTION_REPORT, fail=True)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            ev = _evidence(r.stdout)
            self.assertIsNotNone(ev, r.stdout)
            self.assertGreater(ev["corrupt"], 0)
            self.assertIn("--- failing tests ---", r.stdout,
                          "guard: the pre-existing FAIL lift is untouched")
            kept = list((pathlib.Path(tmp) / "keep").glob("*-rc*"))
            self.assertEqual(len(kept), 1,
                             "the #3010 keep must still happen: %r" % kept)


class GateBlockTest(unittest.TestCase):
    """The consumer half: gate.sh's REAL ASan phase, over every shape."""

    def _run(self, tmp, shard_log, rc=0):
        (pathlib.Path(tmp) / "gate_shard.log").write_text(shard_log)
        text = GATE_SH.read_text()
        helper = _slice(text, _HELPER_START, _HELPER_END,
                        "gate_sanitizer_evidence")
        block = _slice(text, _ASAN_BLOCK_START, _ASAN_BLOCK_END,
                       "gate.sh ASan phase")
        self.assertIn("gate_sanitizer_evidence", block,
                      "guard: the extracted block does not read the evidence line")
        env = dict(os.environ)
        env["PIN_TMP"] = tmp
        env["PIN_RC"] = str(rc)
        script = (GATE_HARNESS.replace("@@HELPER@@", helper)
                              .replace("@@BLOCK@@", block))
        return subprocess.run(["bash", "-c", script], cwd=tmp, env=env,
                              capture_output=True, text=True, timeout=60)

    @staticmethod
    def _line(hits, files, scanned, corrupt):
        return ("%s %d report marker(s) in %d of %d shard log(s) scanned, "
                "%d corruption headline(s)"
                % (EVIDENCE_PREFIX, hits, files, scanned, corrupt))

    def test_a_missing_accounting_line_FAILS_instead_of_reading_as_clean(self):
        """THE #4533 REGRESSION PIN. This is the pre-fix world exactly: a shard
        log with no sanitizer evidence in it at all, because the evidence never
        reached that file. It must now fail the phase, not record
        `rc=0 (no ASan report)`."""
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, "TOTAL: 16482 passed, 0 failed  (across 96 shards, 2230s)\n")
            self.assertEqual(r.returncode, 1,
                             "a phase that cannot see the channel must not "
                             "report clean:\n" + r.stdout + r.stderr)
            self.assertIn("GATE_FAIL:", r.stdout)
            self.assertIn("#4533", r.stdout)
            self.assertIn('REC_RAN shard_asan 1', r.stdout)
            self.assertNotIn("AFTER_ASAN_BLOCK", r.stdout)

    def test_zero_markers_over_a_real_scan_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, self._line(0, 0, 96, 0) + "\n")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('REC_RAN shard_asan 0 no ASan report (96 shard log(s) scanned)',
                          r.stdout)
            self.assertIn("AFTER_ASAN_BLOCK ASAN_ERRS=0 ASAN_SHARDS=7", r.stdout)

    def test_a_corruption_headline_FAILS_the_cycle(self):
        """Deterministic evidence: `ERROR: AddressSanitizer` is never
        deliberate in this tree, and the fail is independent of rc — so the
        flake arbitration can never clear it."""
        for rc in (0, 1):
            with self.subTest(rc=rc), tempfile.TemporaryDirectory() as tmp:
                log = (self._line(2, 1, 96, 1) + "\n"
                       + "--- sanitizer reports ---\n"
                       + ASAN_CORRUPTION_REPORT)
                r = self._run(tmp, log, rc=rc)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("GATE_FAIL: sharded ASan — memory error", r.stdout)
                self.assertIn('REC_RAN shard_asan 1 1 corruption report(s)', r.stdout)
                self.assertIn("heap-use-after-free", r.stdout,
                              "the operator must be shown the report")

    def test_a_leak_only_report_is_recorded_loudly_but_does_not_fail(self):
        """`leaksAtLeast` rows re-emit a real LSan report on EVERY --full run
        and PASS. Failing on them would red every cycle, and on a
        ceiling-killed run would convert the #3667 RETRY into a FAIL."""
        with tempfile.TemporaryDirectory() as tmp:
            log = (self._line(22, 7, 96, 0) + "\n"
                   + "--- sanitizer reports ---\n" + LSAN_LEAK_REPORT)
            r = self._run(tmp, log)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("leak-only sanitizer report marker(s)", r.stdout)
            self.assertIn('REC_RAN shard_asan 0 22 leak-only marker(s), no corruption',
                          r.stdout)
            self.assertIn("AFTER_ASAN_BLOCK ASAN_ERRS=22", r.stdout,
                          "the count must reach telemetry even though it does "
                          "not fail — a drift is then visible in the trend")

    def test_a_scan_that_read_no_shard_log_FAILS(self):
        """`0 markers in 0 of 0 logs` is the same blind spot wearing a hat."""
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, self._line(0, 0, 0, 0) + "\n")
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("scan read 0 shard logs", r.stdout)

    def test_an_unparseable_accounting_line_FAILS(self):
        """A line the gate cannot parse must not be read as zero."""
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, EVIDENCE_PREFIX + " lots of reports, honest\n")
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("unparseable sanitizer-evidence accounting line",
                          r.stdout)


class EndToEndTest(unittest.TestCase):
    """Producer and consumer wired together: the line the REAL run_parallel
    prints must drive the REAL gate block. A drift in that literal fails here
    instead of silently re-inerting the channel."""

    def _gate_over(self, tmp, shard_log_text, rc=0):
        return GateBlockTest._run(GateBlockTest("test_zero_markers_over_a_real_scan_passes"),
                                  tmp, shard_log_text, rc=rc)

    def test_real_lift_output_fails_the_real_gate_block_on_corruption(self):
        with tempfile.TemporaryDirectory() as tmp:
            rp = _run_parallel(tmp, report=ASAN_CORRUPTION_REPORT)
            self.assertEqual(rp.returncode, 0, rp.stdout + rp.stderr)
            g = self._gate_over(tmp, rp.stdout)
            self.assertEqual(g.returncode, 1,
                             "the gate must fail on run_parallel's own output:\n"
                             + rp.stdout + "\n--- gate ---\n" + g.stdout + g.stderr)
            self.assertIn("GATE_FAIL: sharded ASan — memory error", g.stdout)

    def test_real_clean_lift_output_passes_the_real_gate_block(self):
        with tempfile.TemporaryDirectory() as tmp:
            rp = _run_parallel(tmp)
            self.assertEqual(rp.returncode, 0, rp.stdout + rp.stderr)
            g = self._gate_over(tmp, rp.stdout)
            self.assertEqual(g.returncode, 0, g.stdout + g.stderr)
            self.assertIn("REC_RAN shard_asan 0 no ASan report", g.stdout)


class DeliberateReportCaptureTest(unittest.TestCase):
    """Rows that deliberately PRINT a sanitizer headline must keep it out of
    the evidence channel. tests/support/driver_run_support_test.cpp's report
    row impersonates `ERROR: AddressSanitizer: heap-use-after-free` — exactly
    the corruption pattern the gate now hard-fails on — and redirects fd 2 for
    the duration of the call so it cannot reach a shard log. Inert before this
    fix, load-bearing after it."""

    def test_the_impersonated_report_row_still_redirects_fd_2(self):
        text = DRS_TEST.read_text()
        start = text.find("BEZEL_TEST(driver_run_support, "
                          "runProgramForTest_fails_the_test_on_a_sanitizer_report)")
        self.assertGreater(start, 0, "the report row has been renamed or removed")
        body = text[start:text.find("\n}\n", start)]
        self.assertIn("ERROR: AddressSanitizer", body,
                      "guard: this row is the one that prints a corruption headline")
        for needle in ("::dup(2)", "::dup2(captureFd, 2)", "::dup2(savedFd, 2)"):
            self.assertIn(
                needle, body,
                "the row prints a headline the gate hard-fails on (#4533); "
                "dropping the fd-2 capture (%s) would plant a false positive "
                "in a shard log and fail every --full cycle" % needle)


class CanaryBlockTest(unittest.TestCase):
    """The canary tier launches tests/run_parallel.sh too (gate.sh's canary
    block), so its ASan grep had the SAME defect — the issue's claim that it
    "reads a direct binary run and is unaffected" does not hold. It must now
    read the lifted line, and it must stay FAIL-OPEN: the canary is never the
    acceptance authority, and a diff that selects a deliberate `leaksAtLeast`
    row must not brick a healthy gate."""

    def _run(self, tmp, canary_log):
        (pathlib.Path(tmp) / "gate_canary.log").write_text(canary_log)
        text = GATE_SH.read_text()
        helper = _slice(text, _HELPER_START, _HELPER_END,
                        "gate_sanitizer_evidence")
        block = _slice(text, _CANARY_BLOCK_START, _CANARY_BLOCK_END,
                       "gate.sh canary ASan leg")
        env = dict(os.environ)
        env["PIN_TMP"] = tmp
        script = (CANARY_HARNESS.replace("@@HELPER@@", helper)
                                .replace("@@BLOCK@@", block))
        return subprocess.run(["bash", "-c", script], cwd=tmp, env=env,
                              capture_output=True, text=True, timeout=60)

    def test_corruption_in_the_canary_fails_hard(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = (GateBlockTest._line(2, 1, 8, 1) + "\n"
                   + "--- sanitizer reports ---\n" + ASAN_CORRUPTION_REPORT)
            r = self._run(tmp, log)
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("CANARY_DUAL hard", r.stdout,
                          "the dual-consume marker must still be recorded "
                          "BEFORE the fail")
            self.assertIn("GATE_FAIL: canary ASan", r.stdout)

    def test_a_leak_only_canary_does_not_brick_the_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = (GateBlockTest._line(4, 2, 8, 0) + "\n"
                   + "--- sanitizer reports ---\n" + LSAN_LEAK_REPORT)
            r = self._run(tmp, log)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("AFTER_CANARY_BLOCK _c_asan=0", r.stdout,
                          "a leak must not be fed to gate.canary.decide as "
                          "deterministic evidence — it would FAIL_HARD")
            self.assertNotIn("CANARY_DUAL", r.stdout)

    def test_an_unreadable_canary_channel_degrades_open_and_says_so(self):
        """FAIL-OPEN here, fail-closed in the authoritative pass. A canary that
        cannot read the channel must not brick the gate on its own infra, and
        must not pretend it checked."""
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, "TOTAL: 120 passed, 0 failed\n")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("not evaluated here", r.stdout)
            self.assertIn("#4533", r.stdout)
            self.assertIn("AFTER_CANARY_BLOCK _c_asan=0", r.stdout)


class TautologyGuardTest(unittest.TestCase):
    """gate_full_cycle.sh's silent-degradation detector is
    `grep -qi 'asan\\|sanitize'` over gate.sh's output for the --full pass. The
    evidence line carries `SANITIZER`, so emitting it on a PLAIN run would
    satisfy that grep and turn the detector into a tautology — the same trap
    gate.sh's repo-identity banner documents. Every producer of that text must
    therefore sit behind `if [ "$ASAN" -eq 1 ]`."""

    def test_the_detector_still_greps_for_asan_or_sanitize(self):
        self.assertIn("grep -qi 'asan\\|sanitize'", CYCLE_SH.read_text(),
                      "guard: the detector this test protects has moved")

    def test_every_evidence_emitter_sits_behind_the_asan_guard(self):
        text = GATE_SH.read_text()
        regions = []
        for start, end, what in (
                (_HELPER_START, _HELPER_END, "helper"),
                (_ASAN_BLOCK_START, _ASAN_BLOCK_END, "shard ASan block"),
                (_CANARY_BLOCK_START, _CANARY_BLOCK_END, "canary ASan block")):
            block = _slice(text, start, end, what)
            first = text[:text.index(start)].count("\n") + 1
            regions.append(range(first, first + block.count("\n") + 1))
        # The helper is only ever REACHED from the two ASan-guarded blocks, so
        # pin that too: a third call site could print the line on a plain run.
        calls = [n for n, ln in enumerate(text.splitlines(), 1)
                 if "gate_sanitizer_evidence" in ln
                 and not ln.strip().startswith("#")
                 and "gate_sanitizer_evidence() {" not in ln]
        self.assertEqual(
            len(calls), 2,
            "gate_sanitizer_evidence must have exactly two call sites (the "
            "authoritative pass and the canary), found lines %r" % calls)
        offenders = [n for n, ln in enumerate(text.splitlines(), 1)
                     if (EVIDENCE_PREFIX in ln or "gate_sanitizer_evidence" in ln)
                     and not ln.strip().startswith("#")
                     and not any(n in r for r in regions)]
        self.assertEqual(
            offenders, [],
            "every non-comment mention of %r must live in the helper or in an "
            "`if [ \"$ASAN\" -eq 1 ]` block; emitting it on the plain oracle "
            "would make gate_full_cycle.sh's asan-ran detector a tautology. "
            "Offending lines: %r" % (EVIDENCE_PREFIX, offenders))


class EvidenceContractTest(unittest.TestCase):
    """The literal is a contract between two files; pin both ends."""

    def test_run_parallel_emits_the_exact_literal_the_gate_parses(self):
        rp = RP_SH.read_text()
        self.assertIn('echo "%s $san_hits report marker(s) in $san_files of '
                      '$san_scanned shard log(s) scanned, $san_corrupt '
                      'corruption headline(s)"' % EVIDENCE_PREFIX, rp,
                      "the producer literal has drifted from the consumer parser")

    def test_the_lift_runs_before_the_logdir_can_be_reaped(self):
        """_rp_keep is an EXIT trap, so any reader outside this script sees the
        logs only on a FAILING run. The lift must be in the script body."""
        rp = RP_SH.read_text()
        self.assertLess(rp.index("sanitizer evidence lift"), rp.index("exit 0\n"),
                        "the lift must run inline, not from the exit trap")
        self.assertIn('for _sf in "$logdir"/shard.*.log; do', rp,
                      "the lift must read the PER-SHARD logs, not this script's "
                      "own stdout (#4533)")

    def test_json_fixture_is_the_schema_the_accounting_reads(self):
        """Guard on the stub, not on production: a stub whose JSON the real
        accounting rejects would make every LiftTest row vacuous."""
        import sys
        sys.path.insert(0, str(SCRIPTS))
        from gate import summary as _summary
        doc = json.loads(STUB[STUB.index("{\"schema\""):STUB.index("\\n'")]
                         .replace('%d', '1'))
        self.assertEqual(doc["schema"], _summary.SCHEMA)


if __name__ == "__main__":
    unittest.main()
