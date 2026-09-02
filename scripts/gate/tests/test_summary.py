"""Tests for gate.summary (#3012, Phase 2b).

The load-bearing pins:
  1. parse_legacy_summary is a faithful translation of run_parallel.sh:202-220
     (the legacy grep the gate still reads) across the real summary shapes.
  2. check_agreement is ZERO-mismatch when JSON and legacy describe the same
     shard, and reports a mismatch when they disagree (the equivalence data).
  3. read_shard_summary degrades gracefully on a malformed/missing JSON (the
     #3003 shape: garbage must not crash the agreement pass).
Fixtures are written with gate.io.atomic_json_write (the #3010 helper) so the
JSON side uses the same atomic primitive test_main.cpp mirrors.
"""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import io, summary  # noqa: E402


def _write_log(logdir, name, idx, text):
    with open(os.path.join(logdir, "shard.{}.{}.log".format(name, idx)),
              "w") as f:
        f.write(text)


def _write_json(logdir, name, idx, obj):
    io.atomic_json_write(
        os.path.join(logdir, "summary.{}.{}.json".format(name, idx)), obj)


class ParseLegacySummaryTest(unittest.TestCase):
    def test_full_suite_summary(self):
        # The plain shape: "N passed, M failed (shard i/n), T total".
        out = summary.parse_legacy_summary(
            "[ RUN ] x.y\n12 passed, 0 failed (shard 1/4), 40 total\n")
        self.assertEqual(out, {"passed": 12, "failed": 0,
                               "total": 40, "selected": None})

    def test_subset_mode_carries_selected(self):
        # BEZEL_TEST_LIST subset: ", N selected" is present (#2100).
        out = summary.parse_legacy_summary(
            "5 passed, 1 failed, 2 filtered, 8 selected, 40 total\n")
        self.assertEqual(out["selected"], 8)
        self.assertEqual(out["total"], 40)

    def test_filtered_field_is_ignored(self):
        # ", S filtered" is not harvested by run_parallel — only selected.
        out = summary.parse_legacy_summary(
            "3 passed, 0 failed, 7 filtered, 10 total\n")
        self.assertEqual(out["passed"], 3)
        self.assertIsNone(out["selected"])

    def test_takes_the_last_matching_line(self):
        # Mirrors `grep ... | tail -1`: a stray earlier match must not win.
        text = ("garbage 1 passed, 0 failed, 1 total\n"
                "[ RUN ] x.y\n9 passed, 2 failed (shard 0/2), 20 total\n")
        out = summary.parse_legacy_summary(text)
        self.assertEqual(out["passed"], 9)
        self.assertEqual(out["failed"], 2)

    def test_no_summary_returns_none(self):
        self.assertIsNone(summary.parse_legacy_summary("no summary here\n"))
        self.assertIsNone(summary.parse_legacy_summary(""))


class ReadShardSummaryTest(unittest.TestCase):
    def test_reads_valid_json(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "summary.codegen.0.json")
            io.atomic_json_write(p, {"schema": summary.SCHEMA, "passed": 5})
            obj = summary.read_shard_summary(p)
            self.assertEqual(obj["passed"], 5)

    def test_missing_file_returns_none(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(
                summary.read_shard_summary(os.path.join(d, "nope.json")))

    def test_malformed_json_returns_none_not_raise(self):
        # The #3003 graceful-degradation invariant: a half-written / corrupt
        # document must not crash the agreement pass.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "bad.json")
            with open(p, "w") as f:
                f.write('{"passed": 5,')  # truncated mid-document
            self.assertIsNone(summary.read_shard_summary(p))


class SplitShardNameTest(unittest.TestCase):
    def test_parses_log_and_json(self):
        self.assertEqual(
            summary._split_shard_name("shard.codegen.0.log", "shard", ".log"),
            ("codegen", 0))
        self.assertEqual(
            summary._split_shard_name("summary.codegen.3.json",
                                      "summary", ".json"),
            ("codegen", 3))

    def test_name_may_contain_dots(self):
        # The shard index is always the LAST dot-segment (rpartition).
        self.assertEqual(
            summary._split_shard_name("summary.bezel.tests.1.json",
                                      "summary", ".json"),
            ("bezel.tests", 1))

    def test_wrong_prefix_or_suffix(self):
        self.assertIsNone(
            summary._split_shard_name("other.codegen.0.log", "shard", ".log"))
        self.assertIsNone(
            summary._split_shard_name("shard.codegen.0.txt", "shard", ".log"))


class MergeTest(unittest.TestCase):
    def test_per_binary_and_grand_totals(self):
        summaries = [
            {"binary": "codegen", "passed": 5, "failed": 0},
            {"binary": "codegen", "passed": 4, "failed": 1},
            {"binary": "sema", "passed": 3, "failed": 0},
        ]
        m = summary.merge(summaries)
        self.assertEqual(m["binaries"]["codegen"],
                         {"passed": 9, "failed": 1, "shards": 2})
        self.assertEqual(m["binaries"]["sema"],
                         {"passed": 3, "failed": 0, "shards": 1})
        self.assertEqual(m["total"],
                         {"passed": 12, "failed": 1, "shards": 3})


class CheckAgreementTest(unittest.TestCase):
    def _full_log(self, passed, failed, total, idx=0, count=2):
        return ("[ RUN ] x.y\n{} passed, {} failed (shard {}/{}), "
                "{} total\n").format(passed, failed, idx, count, total)

    def test_zero_mismatch_when_json_matches_legacy(self):
        # The healthy case: every shard's JSON agrees with its legacy line.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, self._full_log(5, 0, 20, 0))
            _write_log(d, "codegen", 1, self._full_log(5, 0, 20, 1))
            _write_json(d, "codegen", 0, {
                "schema": summary.SCHEMA, "binary": "b", "shard_index": 0,
                "shard_count": 2, "passed": 5, "failed": 0, "total": 20,
                "selected": 20, "asan_mode": False, "rc": 0, "elapsed_s": 1.0})
            _write_json(d, "codegen", 1, {
                "schema": summary.SCHEMA, "binary": "b", "shard_index": 1,
                "shard_count": 2, "passed": 5, "failed": 0, "total": 20,
                "selected": 20, "asan_mode": False, "rc": 0, "elapsed_s": 1.0})
            self.assertEqual(summary.check_agreement(d), [])

    def test_reports_passed_mismatch(self):
        # A divergence: JSON says 5 passed, legacy says 4 — the #3012 shape
        # (the race corrupted the count the gate read).
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, self._full_log(4, 0, 20, 0))
            _write_json(d, "codegen", 0, {
                "schema": summary.SCHEMA, "binary": "b", "shard_index": 0,
                "shard_count": 2, "passed": 5, "failed": 0, "total": 20,
                "selected": 20, "asan_mode": False, "rc": 0, "elapsed_s": 1.0})
            mm = summary.check_agreement(d)
            self.assertEqual(len(mm), 1)
            self.assertIn("passed mismatch", mm[0])
            self.assertIn("JSON=5", mm[0])
            self.assertIn("legacy=4", mm[0])

    def test_reports_schema_mismatch(self):
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, self._full_log(5, 0, 20, 0))
            _write_json(d, "codegen", 0, {
                "schema": "bezel.shard-summary.v0", "binary": "b",
                "shard_index": 0, "shard_count": 2, "passed": 5, "failed": 0,
                "total": 20, "selected": 20, "asan_mode": False,
                "rc": 0, "elapsed_s": 1.0})
            mm = summary.check_agreement(d)
            self.assertTrue(any("schema mismatch" in m for m in mm))

    def test_reports_missing_json(self):
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, self._full_log(5, 0, 20, 0))
            # No summary.*.json written.
            mm = summary.check_agreement(d)
            self.assertEqual(len(mm), 1)
            self.assertIn("NO JSON summary", mm[0])

    def test_reports_missing_legacy(self):
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, "no summary line at all\n")
            _write_json(d, "codegen", 0, {
                "schema": summary.SCHEMA, "binary": "b", "shard_index": 0,
                "shard_count": 2, "passed": 5, "failed": 0, "total": 20,
                "selected": 20, "asan_mode": False, "rc": 0, "elapsed_s": 1.0})
            mm = summary.check_agreement(d)
            self.assertEqual(len(mm), 1)
            self.assertIn("NO legacy summary", mm[0])

    def test_subset_selected_compared_only_when_legacy_carries_it(self):
        # selected is compared when the legacy line has it (subset mode)...
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0,
                       "5 passed, 0 failed, 8 selected, 20 total\n")
            _write_json(d, "codegen", 0, {
                "schema": summary.SCHEMA, "binary": "b", "shard_index": 0,
                "shard_count": 2, "passed": 5, "failed": 0, "total": 20,
                "selected": 9, "asan_mode": False, "rc": 0, "elapsed_s": 1.0})
            mm = summary.check_agreement(d)
            self.assertTrue(any("selected mismatch" in m for m in mm))
        # ...and ignored (no false mismatch) when the legacy line omits it.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, self._full_log(5, 0, 20, 0))
            _write_json(d, "codegen", 0, {
                "schema": summary.SCHEMA, "binary": "b", "shard_index": 0,
                "shard_count": 2, "passed": 5, "failed": 0, "total": 20,
                "selected": 99, "asan_mode": False, "rc": 0, "elapsed_s": 1.0})
            self.assertEqual(summary.check_agreement(d), [])


class _Fixture(unittest.TestCase):
    """Shared helpers for the Phase 2c account() tests: build a logdir with rc
    files, summary JSONs, and legacy logs, and a reference implementation of
    run_parallel.sh's legacy grep accounting (built on the already-tested
    parse_legacy_summary) to pin account() against."""

    def _rc(self, logdir, name, idx, rc):
        with open(os.path.join(logdir, "rc.{}.{}".format(name, idx)), "w") as f:
            f.write(str(rc))

    def _shard(self, logdir, name, idx, passed, failed, total,
               selected=None, rc=0, schema=summary.SCHEMA, write_json=True,
               write_log=True, legacy_extra="", shard_elapsed=None):
        """Write one shard's rc, JSON, and legacy log line. `selected=None`
        means non-subset (no 'selected' in the legacy line); an int means
        subset mode (the line carries ', N selected').

        `shard_elapsed` (optional, narrow #3222) appends a `SHARD_ELAPSED <s>`
        marker to the legacy log — mirrors tests/run_parallel.sh:163-164.
        Used by the timeout-classification tests to build the "killed at
        ceiling" shape that distinguishes infra-defect from suite-defect.
        When `shard_elapsed` is set the log is written even if `write_log`
        is False — the marker is the only signal extract_shard_elapsed has,
        and the timeout tests need it even on dead shards."""
        self._rc(logdir, name, idx, rc)
        if write_json:
            obj = {"schema": schema, "binary": "bin/" + name,
                   "shard_index": idx, "shard_count": 4,
                   "passed": passed, "failed": failed, "total": total,
                   "selected": selected if selected is not None else total,
                   "asan_mode": False, "rc": (0 if failed == 0 else 1),
                   "elapsed_s": (shard_elapsed if shard_elapsed is not None
                                  else 1.0)}
            _write_json(logdir, name, idx, obj)
        if write_log:
            line = "{} passed, {} failed (shard {}/4), {} total".format(
                passed, failed, idx, total)
            if selected is not None:
                line = "{} passed, {} failed, {} selected, {} total".format(
                    passed, failed, selected, total)
            extra = legacy_extra
            if shard_elapsed is not None:
                extra = extra + "SHARD_ELAPSED {:.2f}\n".format(shard_elapsed)
            _write_log(logdir, name, idx, extra + line + "\n")
        elif shard_elapsed is not None:
            # No legacy line (dead shard) but the SHARD_ELAPSED marker must
            # still land — extract_shard_elapsed reads only the log. Write a
            # log carrying JUST the marker, no legacy summary line.
            _write_log(logdir, name, idx,
                       "SHARD_ELAPSED {:.2f}\n".format(shard_elapsed))

    def _legacy_account(self, logdir, conc, names, subset):
        """Reference impl of run_parallel.sh:185-270 (the grep path), via the
        already-pinned parse_legacy_summary. account() must equal this on every
        fixture — that equality IS the Phase 2c switch's equivalence proof."""
        tp = tf = 0
        shard_rc_ok = count_ok = summary_ok = 1
        global_selected = 0
        crashed = miss = under = []
        for name in names:
            bp = bf = breg = bsel = bsum = 0
            for i in range(conc):
                rcf = os.path.join(logdir, "rc.{}.{}".format(name, i))
                try:
                    with open(rcf) as f:
                        rc = int(f.read().strip())
                except (OSError, ValueError):
                    rc = 1
                if rc != 0:
                    shard_rc_ok = 0
                    crashed = crashed + ["%s/%d" % (name, i)]
                logp = os.path.join(logdir, "shard.{}.{}.log".format(name, i))
                try:
                    with open(logp, errors="replace") as f:
                        leg = summary.parse_legacy_summary(f.read())
                except OSError:
                    leg = None
                if leg is None:
                    miss = miss + ["%s/%d" % (name, i)]
                else:
                    bsum += 1
                    bp += leg["passed"]
                    bf += leg["failed"]
                    if leg["total"] is not None and leg["total"] > breg:
                        breg = leg["total"]
                    if subset and leg.get("selected") is not None:
                        if leg["selected"] > bsel:
                            bsel = leg["selected"]
            bexp = bsel if subset else breg
            what = "selected" if subset else "registered"
            bran = bp + bf
            if bexp > 0 and bran != bexp:
                count_ok = 0
                under = under + ["%s: ran %d of %d %s" % (name, bran, bexp, what)]
            if bsum != conc:
                summary_ok = 0
            if subset:
                global_selected += bsel
            tp += bp
            tf += bf
        return {
            "total_pass": tp, "total_fail": tf, "shard_rc_ok": shard_rc_ok,
            "count_ok": count_ok, "summary_ok": summary_ok,
            "global_selected": global_selected,
            "crashed_shards": crashed, "missing_summary_shards": miss,
            "undercount_bins": under,
        }


class AccountTest(_Fixture):
    def test_clean_run_two_binaries(self):
        # Full-suite shape: 2 binaries, 4 shards each, every shard clean.
        with tempfile.TemporaryDirectory() as d:
            for i in range(4):
                self._shard(d, "codegen", i, 5, 0, 20)
                self._shard(d, "sema", i, 3, 0, 12)
            acc = summary.account(d, 4, ["codegen", "sema"])
            self.assertEqual(acc["total_pass"], 32)   # (5+3)*4
            self.assertEqual(acc["total_fail"], 0)
            self.assertEqual(acc["ran"], 32)
            self.assertEqual(acc["expected_all"], 32)  # 20 + 12
            self.assertTrue(acc["integrity_ok"])
            self.assertTrue(acc["shard_rc_ok"])
            self.assertTrue(acc["count_ok"])
            self.assertTrue(acc["summary_ok"])
            self.assertEqual(acc["crashed_shards"], [])
            self.assertEqual(acc["binaries"][0],
                             {"name": "codegen", "passed": 20, "failed": 0,
                              "ran": 20, "expected": 20, "registry": 20,
                              "selected": 0, "summaries": 4})

    def test_crashed_shard_detected_from_rc_file(self):
        # rc=139 (segfault) but JSON written with rc=0 (test_main wrote it
        # before the crash-on-exit? no — the JSON rc is the test-fail code).
        # The rc FILE is authority: a nonzero rc file => crashed_shard.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 5, 0, 20, rc=0)
            self._shard(d, "codegen", 1, 5, 0, 20, rc=139)  # killed
            self._shard(d, "codegen", 2, 5, 0, 20)
            self._shard(d, "codegen", 3, 5, 0, 20)
            acc = summary.account(d, 4, ["codegen"])
            self.assertFalse(acc["shard_rc_ok"])
            self.assertFalse(acc["integrity_ok"])
            self.assertEqual(acc["crashed_shards"], ["codegen/1"])

    def test_crash_uses_rc_file_not_json_rc_field(self):
        # JSON rc=0 (clean) but the PROCESS rc file says 1 -> still a crash.
        # Proves account() trusts the rc file over the JSON field (the load-
        # bearing reason: a killed shard's JSON rc only records normal exits).
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "sema", 0, 5, 0, 20, rc=0)
            self._shard(d, "sema", 1, 5, 0, 20, rc=0)
            # Override rc file to nonzero AFTER writing a clean JSON.
            self._rc(d, "sema", 1, 137)
            acc = summary.account(d, 2, ["sema"])
            self.assertEqual(acc["crashed_shards"], ["sema/1"])
            self.assertFalse(acc["shard_rc_ok"])

    def test_missing_rc_file_is_a_crash(self):
        # No rc file at all -> ${rc:-1} -> crash (run_parallel.sh:202).
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 5, 0, 20, rc=0)
            self._shard(d, "codegen", 1, 5, 0, 20, rc=0)
            os.unlink(os.path.join(d, "rc.codegen.1"))  # subshell died
            acc = summary.account(d, 2, ["codegen"])
            self.assertEqual(acc["crashed_shards"], ["codegen/1"])
            self.assertFalse(acc["shard_rc_ok"])

    def test_missing_summary_detected(self):
        # Shard exited 0 (rc=0) but wrote NO summary (no JSON, no log line) ->
        # summary_ok=0 (a slice was not validated). Stricter than the undercount
        # check (#1861). In the dual-emit gate JSON-present <-> log-line-present
        # (Phase 2a), so "no JSON" is exactly "printed no summary".
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 5, 0, 20, rc=0)
            self._shard(d, "codegen", 1, 5, 0, 20, rc=0,
                        write_json=False, write_log=False)
            acc = summary.account(d, 2, ["codegen"])
            self.assertFalse(acc["summary_ok"])
            self.assertFalse(acc["integrity_ok"])
            self.assertEqual(acc["missing_summary_shards"], ["codegen/1"])
            # rc was 0 so NOT a crash:
            self.assertTrue(acc["shard_rc_ok"])

    def test_undercount_detected(self):
        # A shard that dies mid-run drops passed counts: ran < registry.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 5, 0, 20)
            self._shard(d, "codegen", 1, 3, 0, 20)   # only 3, not 5
            self._shard(d, "codegen", 2, 5, 0, 20)
            self._shard(d, "codegen", 3, 5, 0, 20)
            acc = summary.account(d, 4, ["codegen"])
            self.assertFalse(acc["count_ok"])
            self.assertEqual(acc["undercount_bins"],
                             ["codegen: ran 18 of 20 registered"])

    def test_subset_mode_expected_is_selected(self):
        # BEZEL_TEST_LIST: expected = selected (not registry). A binary that
        # selects fewer than its registry must NOT undercount.
        with tempfile.TemporaryDirectory() as d:
            for i in range(4):
                self._shard(d, "codegen", i, 2, 0, 20, selected=8)
            acc = summary.account(d, 4, ["codegen"], subset=True)
            self.assertTrue(acc["subset_mode"])
            self.assertEqual(acc["global_selected"], 8)
            self.assertEqual(acc["expected_all"], 8)  # selected, not 20
            self.assertTrue(acc["count_ok"])          # ran 8 == selected 8
            self.assertTrue(acc["integrity_ok"])

    def test_subset_empty_is_never_a_pass(self):
        # A subset run that selected 0 tests validated nothing -> fail loudly
        # (#2100), even though ran==expected==0.
        with tempfile.TemporaryDirectory() as d:
            for i in range(4):
                self._shard(d, "codegen", i, 0, 0, 20, selected=0)
            acc = summary.account(d, 4, ["codegen"], subset=True)
            self.assertTrue(acc["subset_empty"])
            self.assertFalse(acc["integrity_ok"])

    def test_names_order_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            for i in range(2):
                self._shard(d, "ownership", i, 4, 0, 8)
                self._shard(d, "codegen", i, 6, 0, 12)
            acc = summary.account(d, 2, ["ownership", "codegen"])
            self.assertEqual([b["name"] for b in acc["binaries"]],
                             ["ownership", "codegen"])

    def test_equivalence_to_legacy_on_clean_run(self):
        # THE equivalence pin: account(JSON) == legacy grep accounting on the
        # same fixture. This is the unit-level proof the Phase 2c switch relies
        # on (the dual-read at the run level is the integration proof).
        with tempfile.TemporaryDirectory() as d:
            for i in range(4):
                self._shard(d, "codegen", i, 5, 0, 20)
                self._shard(d, "sema", i, 3, 1, 12)
                self._shard(d, "support", i, 2, 0, 8)
            names = ["codegen", "sema", "support"]
            acc = summary.account(d, 4, names)
            leg = self._legacy_account(d, 4, names, subset=False)
            for k in ("total_pass", "total_fail", "shard_rc_ok", "count_ok",
                      "summary_ok", "crashed_shards",
                      "missing_summary_shards", "undercount_bins"):
                self.assertEqual(acc[k], leg[k],
                                 "account() != legacy on %s" % k)

    def test_equivalence_to_legacy_on_mixed_defects(self):
        # Realistic dual-emit coupling (JSON-present <-> log-line-present, Phase
        # 2a): a crashed/summary-less shard has NEITHER. One timeout crash
        # (shard1, rc=124, no summary) over a subset run; account() must still
        # match the legacy grep path field-for-field.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 2, 0, 20, selected=8)
            self._shard(d, "codegen", 1, rc=124, passed=0, failed=0, total=20,
                        selected=8, write_json=False, write_log=False)
            self._shard(d, "codegen", 2, 2, 0, 20, selected=8)
            self._shard(d, "codegen", 3, 2, 0, 20, selected=8)
            names = ["codegen"]
            acc = summary.account(d, 4, names, subset=True)
            leg = self._legacy_account(d, 4, names, subset=True)
            for k in ("total_pass", "total_fail", "shard_rc_ok", "count_ok",
                      "summary_ok", "global_selected", "crashed_shards",
                      "missing_summary_shards", "undercount_bins"):
                self.assertEqual(acc[k], leg[k],
                                 "account() != legacy on %s" % k)
            self.assertFalse(acc["integrity_ok"])


class EmitShellTest(_Fixture):
    def test_round_trips_through_bash(self):
        # emit_shell's output must be `eval`-safe and reproduce the values.
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            for i in range(2):
                self._shard(d, "codegen", i, 5, 0, 10)
            acc = summary.account(d, 2, ["codegen"])
            blob = summary.emit_shell(acc)
            # eval in bash and echo selected keys back.
            script = ("set -e; " + blob + "; "
                      'echo "$rp_total_pass|$rp_integrity_ok|'
                      '${#rp_bin_names[@]}|${rp_bin_pass[0]}|'
                      '${rp_crashed_shards[*]}"')
            out = subprocess.run(
                ["bash", "-c", script], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertEqual(out.stdout.strip(), "10|1|1|10|")

    def test_quotes_names_with_single_quotes(self):
        # A pathological binary name containing a single quote must survive eval
        # (shell_quote escapes it). Real names are simple, but the helper must
        # be safe.
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "co'de", 0, 1, 0, 1)
            acc = summary.account(d, 1, ["co'de"])
            blob = summary.emit_shell(acc)
            out = subprocess.run(
                ["bash", "-c", blob + "; printf '%s' \"${rp_bin_names[0]}\""],
                capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertEqual(out.stdout, "co'de")


class RunSummaryRoundTripTest(_Fixture):
    def test_write_then_harvest(self):
        with tempfile.TemporaryDirectory() as d:
            for i in range(2):
                self._shard(d, "codegen", i, 5, 0, 10)
                self._shard(d, "sema", i, 3, 0, 6)
            acc = summary.account(d, 2, ["codegen", "sema"])
            path = os.path.join(d, "run.json")
            summary.write_run_summary(path, acc)
            gtests, ran = summary.harvest_run_summary(path)
            self.assertEqual(gtests, 16)  # (5+3)*2
            self.assertEqual(ran, {"codegen": 10, "sema": 6})

    def test_harvest_missing_returns_none(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(
                summary.harvest_run_summary(os.path.join(d, "nope.json")),
                (None, None))

    def test_harvest_wrong_schema_returns_none(self):
        # A shard-summary (not a run-summary) must be refused, not mis-read.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.json")
            io.atomic_json_write(p, {"schema": summary.SCHEMA, "passed": 5})
            self.assertEqual(summary.harvest_run_summary(p), (None, None))

    def test_write_is_atomic_valid_json(self):
        # The run-summary is the gate.sh bridge: it must be valid JSON.
        with tempfile.TemporaryDirectory() as d:
            for i in range(2):
                self._shard(d, "codegen", i, 5, 0, 10)
            acc = summary.account(d, 2, ["codegen"])
            path = os.path.join(d, "run.json")
            summary.write_run_summary(path, acc)
            with open(path) as f:
                obj = json.loads(f.read())  # must not raise
            self.assertEqual(obj["schema"], summary.RUN_SCHEMA)


class AccountCLITest(_Fixture):
    """Smoke-test the `account` and `run-summary harvest` CLIs via the exact
    invocation run_parallel.sh uses (`PYTHONPATH=scripts python3 -m
    gate.summary ...`). Pins the module-path form (the codebase convention,
    matching verdict.py/trend.py) so a future `from gate import` addition can't
    silently break the gate's reader."""

    # PYTHONPATH must point at scripts/ (the parent of gate/) so `gate` is a
    # package — matching run_parallel's `PYTHONPATH=$repo_root/scripts`.
    _SCRIPTS_DIR = os.path.join(os.path.dirname(__file__), "..", "..")

    def _run_cli(self, args):
        import subprocess
        env = dict(os.environ, PYTHONPATH=os.path.abspath(self._SCRIPTS_DIR))
        return subprocess.run([sys.executable, "-m", "gate.summary"] + args,
                              capture_output=True, text=True, env=env)

    def test_account_emits_sourceable_output(self):
        with tempfile.TemporaryDirectory() as d:
            for i in range(2):
                self._shard(d, "codegen", i, 5, 0, 10)
            out = self._run_cli(["account", d, "--conc", "2", "--", "codegen"])
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("rp_total_pass=10", out.stdout)
            self.assertIn("rp_integrity_ok=1", out.stdout)
            self.assertIn("rp_bin_names=('codegen')", out.stdout)

    def test_account_writes_run_summary_and_harvest_reads_it(self):
        with tempfile.TemporaryDirectory() as d:
            for i in range(2):
                self._shard(d, "codegen", i, 5, 0, 10)
            run_path = os.path.join(d, "run.json")
            out = self._run_cli(
                ["account", d, "--conc", "2", "--run-summary", run_path,
                 "--", "codegen"])
            self.assertEqual(out.returncode, 0, out.stderr)
            # harvest reads back what account wrote (the gate.sh bridge).
            hout = self._run_cli(["run-summary", "harvest", run_path])
            self.assertIn("rp_run_present=1", hout.stdout)
            self.assertIn("rp_run_gtests=10", hout.stdout)

    def test_check_cli_exits_nonzero_on_mismatch(self):
        # The dual-read assertion relies on check exiting 1 on mismatch.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 5, 0, 10)  # legacy says 5
            _write_json(d, "codegen", 0, {  # JSON says 4 -> mismatch
                "schema": summary.SCHEMA, "binary": "b", "shard_index": 0,
                "shard_count": 2, "passed": 4, "failed": 0, "total": 10,
                "selected": 10, "asan_mode": False, "rc": 0, "elapsed_s": 1.0})
            out = self._run_cli(["check", d])
            self.assertEqual(out.returncode, 1)
            self.assertIn("MISMATCH", out.stdout + out.stderr)

    def test_check_cli_exits_zero_on_agreement(self):
        with tempfile.TemporaryDirectory() as d:
            for i in range(2):
                self._shard(d, "codegen", i, 5, 0, 10)
            out = self._run_cli(["check", d])
            self.assertEqual(out.returncode, 0)


class ExtractCrashCulpritTest(unittest.TestCase):
    r"""extract_crash_culprit mirrors run_parallel.sh:313 — `grep -aE '^\[ RUN'
    <log> | tail -1 | sed -E 's/^\[ RUN  \] //'`. per #1608: the last-run test
    is a MISATTRIBUTION for the crash culprit, but it is the documented behavior
    consumers expect, so it is preserved byte-for-byte."""

    def test_returns_last_run_test_name(self):
        # tail -1: the LAST [ RUN ] line wins, not the first.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0,
                       "[ RUN  ] First.test\n[  OK  ] First.test\n"
                       "[ RUN  ] Second.test\n")
            self.assertEqual(summary.extract_crash_culprit(d, "codegen/0"),
                             "Second.test")

    def test_missing_log_returns_empty(self):
        # `2>/dev/null` — a missing shard log reads as <none> (empty culprit).
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(summary.extract_crash_culprit(d, "codegen/9"), "")

    def test_no_run_line_returns_empty(self):
        # grep found nothing => tail -1 empty => <none>.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, "12 passed, 0 failed, 40 total\n")
            self.assertEqual(summary.extract_crash_culprit(d, "codegen/0"), "")

    def test_strips_exact_run_marker(self):
        # The marker is "[ RUN  ] " (two spaces before the bracket — the exact
        # emission of test_main.cpp:238); the test name follows verbatim.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "sema", 1, "[ RUN  ] SuiteA.qux\n")
            self.assertEqual(summary.extract_crash_culprit(d, "sema/1"),
                             "SuiteA.qux")

    def test_malformed_shard_id_returns_empty(self):
        # partition('/') finds no '/' — a defensive guard (run_parallel always
        # passes name/idx, but the function must not crash on bad input).
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(summary.extract_crash_culprit(d, "nodash"), "")


class ExtractLogFailuresTest(unittest.TestCase):
    r"""extract_log_failures mirrors arbitration.py's `_FAIL_LINE_RE` — `[ FAIL
    ] <test>` first-token test names from a shard log. Per #1608/#3018: the
    `died on:` attribution is the last [ RUN ] line (a misattribution); the
    actual failing test is signaled by [ FAIL ]. Surfaced in the banner so
    readers see the real culprit."""

    def test_returns_all_fail_lines_in_order(self):
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "sema", 5,
                       "[ RUN  ] sema_zero_one_reject_1424.a\n"
                       "[  OK  ] sema_zero_one_reject_1424.a\n"
                       "[ RUN  ] sema_other.actual_failure\n"
                       "[ FAIL ] sema_other.actual_failure\n"
                       "[ RUN  ] sema_zero_one_reject_1424.b\n")
            self.assertEqual(
                summary.extract_log_failures(d, "sema/5"),
                ["sema_other.actual_failure"])

    def test_no_fail_lines_returns_empty(self):
        # The common case — a shard dies after its last [ RUN ] but before any
        # [ FAIL ] gets printed (the "clean signal-killed" path). The banner
        # suppresses the new line entirely.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "sema", 5,
                       "[ RUN  ] sema_zero_one_reject_1424.a\n"
                       "[  OK  ] sema_zero_one_reject_1424.a\n"
                       "[ RUN  ] sema_zero_one_reject_1424.b\n")
            self.assertEqual(summary.extract_log_failures(d, "sema/5"), [])

    def test_missing_log_returns_empty(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(summary.extract_log_failures(d, "sema/5"), [])

    def test_malformed_shard_id_returns_empty(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(summary.extract_log_failures(d, "nodash"), [])

    def test_multiple_fail_lines_all_preserved(self):
        # A shard that surfaces multiple failing tests (the misattribution
        # case: the real culprit is the LAST [FAIL], the others are earlier
        # observed failures). Preserve order so the LAST [FAIL] is naturally
        # the last entry — the most recent culprit is the most diagnostic.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "sema", 5,
                       "[ FAIL ] sema_a.first\n"
                       "[ FAIL ] sema_b.second\n"
                       "[ FAIL ] sema_c.third\n")
            self.assertEqual(
                summary.extract_log_failures(d, "sema/5"),
                ["sema_a.first", "sema_b.second", "sema_c.third"])


class FormatPhantomWarningTest(unittest.TestCase):
    """format_phantom_warning mirrors run_parallel.sh:283-289 (#2100). The
    'WARNING: only' literal is LOAD-BEARING — gate.sh:637 and agent_check.sh:266
    grep it, so the exact text is pinned."""

    def test_warns_when_selected_below_listed(self):
        self.assertEqual(
            summary.format_phantom_warning(listed=10, selected=7),
            "bezel_tests: WARNING: only 7 of 10 listed tests matched the "
            "registry (BEZEL_TEST_LIST has phantom/renamed names)")

    def test_no_warning_when_selected_equals_listed(self):
        self.assertEqual(summary.format_phantom_warning(listed=8, selected=8), "")

    def test_no_warning_when_selected_above_listed(self):
        self.assertEqual(summary.format_phantom_warning(listed=5, selected=9), "")

    def test_no_warning_when_listed_zero(self):
        # An empty/unset list cannot have phantom names; selected(>=0) < 0 is
        # never true, matching bash's `${listed:-0}` default.
        self.assertEqual(summary.format_phantom_warning(listed=0, selected=0), "")

    def test_literal_is_preserved_verbatim(self):
        # The exact bytes gate.sh/agent_check grep for — the contract.
        out = summary.format_phantom_warning(listed=3, selected=1)
        self.assertTrue(out.startswith("bezel_tests: WARNING: only 1 of 3"))
        self.assertIn("BEZEL_TEST_LIST has phantom/renamed names", out)


class FormatFailureBannerTest(unittest.TestCase):
    """format_failure_banner mirrors run_parallel.sh:298-317. The 'shard crash
    / undercount' literal is LOAD-BEARING — gate_full_cycle.sh:79 (retry/accept)
    and gate.sh:106 (verdict-truncation detection) grep it; the em-dashes are
    pinned byte-for-byte."""

    def test_no_defect_returns_empty(self):
        self.assertEqual(summary.format_failure_banner(
            ran=10, expected=10, nbin=2, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=4,
            crashed_culprits=[]), "")

    def test_crashed_only_with_culprit(self):
        out = summary.format_failure_banner(
            ran=8, expected=10, nbin=1, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=2,
            crashed_culprits=[("codegen/0", "SuiteA.qux")])
        self.assertEqual(out, "\n".join([
            "############################################################",
            "## GATE FAILURE: shard crash / undercount — result is NOT trustworthy",
            "##   ran 8 of 10 expected tests (2 unaccounted) across 1 binaries",
            "##   shard codegen/0 exited nonzero — died on: SuiteA.qux",
            "############################################################"]))

    def test_crashed_with_empty_culprit_renders_none(self):
        # ${last:-<none>} — an empty culprit (shard died at startup) => <none>.
        out = summary.format_failure_banner(
            ran=0, expected=5, nbin=1, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=1,
            crashed_culprits=[("codegen/0", "")])
        self.assertIn("died on: <none>", out)

    def test_undercount_bins_each_get_a_line(self):
        out = summary.format_failure_banner(
            ran=3, expected=8, nbin=2,
            undercount_bins=["codegen: ran 1 of 3 registered",
                             "sema: ran 2 of 5 registered"],
            subset_empty=False, missing_summary_shards=[], njobs=4,
            crashed_culprits=[])
        self.assertIn("##   UNDERCOUNT codegen: ran 1 of 3 registered", out)
        self.assertIn("##   UNDERCOUNT sema: ran 2 of 5 registered", out)
        # unaccounted = expected - ran = 5
        self.assertIn("ran 3 of 8 expected tests (5 unaccounted) across 2 binaries",
                      out)

    def test_subset_empty_line(self):
        out = summary.format_failure_banner(
            ran=0, expected=0, nbin=2, undercount_bins=[],
            subset_empty=True, missing_summary_shards=[], njobs=4,
            crashed_culprits=[])
        self.assertIn("##   SUBSET run selected 0 tests in EVERY binary "
                      "(empty/unmatched BEZEL_TEST_LIST) — nothing was validated",
                      out)

    def test_missing_summary_line_with_count_and_list(self):
        # "N of M shards ... shard(s) a b" — count + space-joined list.
        out = summary.format_failure_banner(
            ran=4, expected=4, nbin=2, undercount_bins=[],
            subset_empty=False,
            missing_summary_shards=["sema/1", "codegen/0"], njobs=4,
            crashed_culprits=[])
        self.assertIn("##   2 of 4 shards printed NO completion summary "
                      "(slice not validated): shard(s) sema/1 codegen/0", out)

    def test_all_defects_combined_byte_identity_pin(self):
        # The full banner — a snapshot pin (judgement, not behaviour): the
        # literal lines, the em-dashes, and the ordering are all contracts.
        out = summary.format_failure_banner(
            ran=3, expected=10, nbin=2,
            undercount_bins=["codegen: ran 1 of 3 registered"],
            subset_empty=True,
            missing_summary_shards=["sema/1"], njobs=4,
            crashed_culprits=[("codegen/0", "Last.Run"),
                              ("frontend/1", "")])
        self.assertEqual(out, "\n".join([
            "############################################################",
            "## GATE FAILURE: shard crash / undercount — result is NOT trustworthy",
            "##   ran 3 of 10 expected tests (7 unaccounted) across 2 binaries",
            "##   UNDERCOUNT codegen: ran 1 of 3 registered",
            "##   SUBSET run selected 0 tests in EVERY binary "
            "(empty/unmatched BEZEL_TEST_LIST) — nothing was validated",
            "##   1 of 4 shards printed NO completion summary "
            "(slice not validated): shard(s) sema/1",
            "##   shard codegen/0 exited nonzero — died on: Last.Run",
            "##   shard frontend/1 exited nonzero — died on: <none>",
            "############################################################"]))

    def test_load_bearing_literal_preserved(self):
        # gate_full_cycle.sh:79 + gate.sh:106 grep 'shard crash / undercount'.
        out = summary.format_failure_banner(
            ran=0, expected=1, nbin=1, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=1,
            crashed_culprits=[("x/0", "")])
        self.assertIn("shard crash / undercount", out)

    def test_crashed_with_actual_failures_appends_actual_fail_line(self):
        # Per #3018: when a shard log carries [FAIL] lines, the banner names
        # the real culprit (the test that ran and FAILED) alongside the
        # preserved last-[RUN] attribution. The new line is conditional and
        # byte-identical for the common no-FAIL case (#3018 byte-stability pin).
        #
        # fix #3668: the shard here is a GENUINE crash — it is in
        # missing_summary_shards, i.e. it printed no completion summary, so its
        # last [ RUN ] really may be where it died. The fixture used to pass an
        # EMPTY missing_summary_shards, which describes a shard that COMPLETED
        # its slice; that case now takes the "completed its slice" wording (see
        # test_completed_shard_with_a_fail_does_not_get_a_died_on_line). Making
        # the fixture say what its name claims is the whole update.
        out = summary.format_failure_banner(
            ran=8, expected=10, nbin=1, undercount_bins=[],
            subset_empty=False, missing_summary_shards=["sema/5"], njobs=2,
            crashed_culprits=[("sema/5", "sema_zero_one_reject_1424.last_one")],
            crashed_log_failures=[("sema/5", ["sema_other.actual_failure"])])
        self.assertIn("died on: sema_zero_one_reject_1424.last_one", out)
        self.assertIn("actual FAIL: sema_other.actual_failure", out)

    def test_crashed_without_failures_omits_actual_fail_line(self):
        # No [FAIL] lines in the log -> the new line is suppressed entirely.
        # The banner stays byte-identical to the pre-#3018 output for this
        # case (the common "clean signal-killed" path).
        out = summary.format_failure_banner(
            ran=8, expected=10, nbin=1, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=2,
            crashed_culprits=[("sema/5", "sema_zero_one_reject_1424.last_one")],
            crashed_log_failures=[("sema/5", [])])
        self.assertIn("died on: sema_zero_one_reject_1424.last_one", out)
        self.assertNotIn("actual FAIL:", out)

    def test_crashed_log_failures_none_default_omits_actual_fail_line(self):
        # The new parameter defaults to None (back-compat for any caller that
        # doesn't pass it). The new line MUST be suppressed in that case —
        # the pre-#3018 banner is byte-identical for the default invocation.
        out = summary.format_failure_banner(
            ran=8, expected=10, nbin=1, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=2,
            crashed_culprits=[("sema/5", "sema_zero_one_reject_1424.last_one")])
        self.assertIn("died on: sema_zero_one_reject_1424.last_one", out)
        self.assertNotIn("actual FAIL:", out)


class CompletedShardNotADiedOnVictimTest(unittest.TestCase):
    """fix #3668: a shard that RAN ITS WHOLE SLICE and exited nonzero because
    one test FAILED must not stamp its last (passing) test as `died on:`.

    Measured shape (Run 10 cycle-1 ASan gate at a9ca834, shard module/7):
    RUN=190, OK=189, FAIL=1, a printed `189 passed, 1 failed` summary and a
    SHARD_ELAPSED marker — a shard that completed, not one that died. The
    banner nevertheless printed
    `died on: xmod_with_replacement_type_3177....`, arbitration turned that
    `died on:` into a suspect, re-ran the (passing) suite five times under
    ASan and stamped an innocent test FLAKY.

    The distinguishing fact is one account() already computes: a shard that
    printed no completion summary is in `missing_summary_shards`; a shard that
    completed is not. The banner is the consumer that must read it —
    `extract_crash_culprit`'s last-[ RUN ] contract (#1608) is untouched.
    """

    # The measured names, kept verbatim so the pin reads as the incident.
    INNOCENT = ("xmod_with_replacement_type_3177."
                "imported_positional_record_with_target_is_type_checked_9_5")
    REAL = ("xmod_abstract_class_indexer_unoverridden_row12."
            "cross_module_indexer_row12")

    def _banner(self, missing):
        return summary.format_failure_banner(
            ran=1522, expected=1523, nbin=6,
            undercount_bins=["module: ran 1522 of 1523 registered"],
            subset_empty=False, missing_summary_shards=missing, njobs=48,
            crashed_culprits=[("module/7", self.INNOCENT)],
            crashed_log_failures=[("module/7", [self.REAL])])

    def test_completed_shard_with_a_fail_does_not_get_a_died_on_line(self):
        out = self._banner(missing=[])
        # The false accusation is gone...
        self.assertNotIn("died on:", out)
        self.assertNotIn(self.INNOCENT, out)
        # ...and the shard line names the test that actually failed.
        self.assertIn("##   shard module/7 exited nonzero — completed its "
                      "slice; actual FAIL: " + self.REAL, out)

    def test_crashed_shard_with_a_fail_keeps_both_lines(self):
        # The shard that genuinely died (no completion summary) is unchanged:
        # `died on:` is still emitted, because for a real crash the last
        # [ RUN ] victim CAN be the culprit and arbitration must still see it
        # (arbitration.py deliberately arbitrates died-on victims).
        out = self._banner(missing=["module/7"])
        self.assertIn("##   shard module/7 exited nonzero — died on: "
                      + self.INNOCENT, out)
        self.assertIn("##   shard module/7 actual FAIL: " + self.REAL, out)

    def test_completed_shard_without_a_fail_keeps_died_on(self):
        # LOUD -> QUIET is a rejected trade: a shard that completed its slice
        # and STILL exited nonzero with no [ FAIL ] line (a teardown exit(70),
        # an atexit abort) has no real culprit to name, so the last-[ RUN ]
        # attribution stays — it is the only marker arbitration would have.
        out = summary.format_failure_banner(
            ran=1523, expected=1523, nbin=6, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=48,
            crashed_culprits=[("module/7", self.INNOCENT)],
            crashed_log_failures=[("module/7", [])])
        self.assertIn("died on: " + self.INNOCENT, out)

    def test_load_bearing_literals_survive(self):
        # The banner header + the 'shard crash / undercount' literal
        # (gate_full_cycle.sh:79, gate.sh:106) are unaffected by the rewording.
        out = self._banner(missing=[])
        self.assertIn("## GATE FAILURE: shard crash / undercount — result is "
                      "NOT trustworthy", out)
        self.assertIn("##   shard module/7 exited nonzero", out)

    def test_two_shards_one_completed_one_crashed(self):
        # Mixed run: only the completed shard is reworded; the crashed one is
        # byte-identical to today, so a real crash is never quietened.
        out = summary.format_failure_banner(
            ran=100, expected=200, nbin=2, undercount_bins=[],
            subset_empty=False, missing_summary_shards=["codegen/3"], njobs=16,
            crashed_culprits=[("module/7", self.INNOCENT),
                              ("codegen/3", "codegen_x.victim")],
            crashed_log_failures=[("module/7", [self.REAL]),
                                  ("codegen/3", ["codegen_y.real"])])
        self.assertIn("##   shard module/7 exited nonzero — completed its "
                      "slice; actual FAIL: " + self.REAL, out)
        self.assertIn("##   shard codegen/3 exited nonzero — died on: "
                      "codegen_x.victim", out)
        self.assertIn("##   shard codegen/3 actual FAIL: codegen_y.real", out)
        # Exactly ONE died-on line survives — the crashed shard's.
        self.assertEqual(sum(1 for ln in out.splitlines() if "died on:" in ln),
                         1)

    def test_cli_end_to_end_from_a_real_completed_shard_log(self):
        # The whole path run_parallel actually walks: a real shard log with
        # every [ RUN ] terminated, one [ FAIL ], and a completion summary.
        # --missing-summary is empty (the shard wrote its summary), so the CLI
        # must reach the same verdict as the formatter above.
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "module", 7,
                       "[ RUN  ] {r}\n[ FAIL ] {r} (12 ms)\n"
                       "[ RUN  ] {i}\n[  OK  ] {i} (7 ms)\n"
                       "189 passed, 1 failed (shard 7/8), 1523 total\n"
                       "SHARD_ELAPSED 30.85\n".format(r=self.REAL,
                                                       i=self.INNOCENT))
            env = dict(os.environ)
            root = os.path.join(os.path.dirname(__file__), "..", "..")
            env["PYTHONPATH"] = root
            out = subprocess.run(
                [sys.executable, "-m", "gate.summary", "format-failure",
                 "--ran", "1522", "--expected", "1523", "--nbin", "1",
                 "--njobs", "8", "--crashed", "module/7", "--logdir", d],
                capture_output=True, text=True, env=env, cwd=root)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertNotIn("died on:", out.stdout)
            self.assertIn("completed its slice; actual FAIL: " + self.REAL,
                          out.stdout)


class FormatCLITest(unittest.TestCase):
    """The format-phantom / format-failure CLIs that run_parallel calls."""

    def _run_cli(self, args):
        import subprocess
        env = dict(os.environ)
        env["PYTHONPATH"] = os.path.join(os.path.dirname(__file__),
                                         "..", "..")
        out = subprocess.run([sys.executable, "-m", "gate.summary"] + args,
                             capture_output=True, text=True, env=env,
                             cwd=os.path.join(os.path.dirname(__file__),
                                              "..", ".."))
        return out

    def test_format_phantom_cli_emits_warning(self):
        out = self._run_cli(["format-phantom", "--listed", "10",
                             "--selected", "7"])
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(out.stdout,
                         "bezel_tests: WARNING: only 7 of 10 listed tests "
                         "matched the registry (BEZEL_TEST_LIST has phantom/"
                         "renamed names)\n")

    def test_format_phantom_cli_emits_nothing_when_no_warning(self):
        out = self._run_cli(["format-phantom", "--listed", "5",
                             "--selected", "5"])
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(out.stdout, "")

    def test_format_failure_cli_with_culprit_extraction(self):
        # The CLI does the culprit extraction from the shard log itself
        # (run_parallel no longer carries the grep|tail|sed).
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0,
                       "[ RUN  ] First.test\n[ RUN  ] Second.test\n")
            out = self._run_cli(
                ["format-failure", "--ran", "8", "--expected", "10",
                 "--nbin", "1", "--njobs", "2", "--crashed", "codegen/0",
                 "--logdir", d])
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("shard crash / undercount", out.stdout)
            self.assertIn("died on: Second.test", out.stdout)

    def test_format_failure_cli_no_defect_emits_nothing(self):
        # Per #3012 / Phase 2c: the bash `[[ -n "$_rp_py_failure" ]]` guard
        # only prints when there is a defect. The CLI must return "" for a
        # no-defect invocation so the guard never fires spuriously.
        with tempfile.TemporaryDirectory() as d:
            out = self._run_cli(
                ["format-failure", "--ran", "10", "--expected", "10",
                 "--nbin", "2", "--njobs", "4", "--logdir", d])
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertEqual(out.stdout, "")


class ExtractShardElapsedTest(unittest.TestCase):
    """extract_shard_elapsed mirrors the SHARD_ELAPSED grep at
    tests/run_parallel.sh:163-164 (narrow #3222). Returns the LAST matching
    line's float value — matching the `awk tail -1` semantics in the original
    so a shard's recorded wall time is recoverable even if the harness printed
    the marker twice (once in a setup hook, once at exit).
    """

    def test_returns_last_marker_when_present_once(self):
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, "12 passed\nSHARD_ELAPSED 3.14\n")
            self.assertEqual(
                summary.extract_shard_elapsed(d, "codegen/0"), 3.14)

    def test_returns_last_marker_when_present_multiple_times(self):
        # awk 'tail -1' — a stray earlier marker must not win.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0,
                       "SHARD_ELAPSED 1.00\n12 passed\nSHARD_ELAPSED 2.50\n")
            self.assertEqual(
                summary.extract_shard_elapsed(d, "codegen/0"), 2.50)

    def test_missing_log_returns_none(self):
        # Mirrors `2>/dev/null` — a missing log reads as no marker.
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(
                summary.extract_shard_elapsed(d, "codegen/9"))

    def test_no_marker_returns_none(self):
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, "12 passed, 0 failed\n")
            self.assertIsNone(
                summary.extract_shard_elapsed(d, "codegen/0"))

    def test_malformed_shard_id_returns_none(self):
        # partition('/') finds no '/' — defensive guard (caller always passes
        # name/idx, but the function must not crash on bad input).
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(summary.extract_shard_elapsed(d, "nodash"))

    def test_malformed_marker_returns_none(self):
        # `SHARD_ELAPSED abc` — non-numeric marker is unusable, returns None
        # rather than raising (the caller's "conservatively leave in
        # crashed_shards" rule then applies).
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, "SHARD_ELAPSED abc\n")
            self.assertIsNone(
                summary.extract_shard_elapsed(d, "codegen/0"))


class TimeoutClassificationTest(_Fixture):
    """Narrow #3222: a shard with rc in {124,137} AND SHARD_ELAPSED near the
    configured ceiling is reclassified as `timed_out_shards` (a CEILING KILL —
    an INFRA defect, fix the ceiling) rather than `crashed_shards` (a SUITE
    defect, fix the test). The two sides of that AND are each load-bearing:

      - rc in {124,137} alone catches every `timeout`-killed shard, including
        deadlocks that happened to outlive the ceiling (those are real crashes
        that the timeout happens to catch). Without the elapsed check, a
        deadlock at second 30 of a 3600s ASan ceiling would be misclassified
        as a ceiling kill.
      - elapsed >= 0.95*ceiling alone would silently reclassify any rc=124
        shard regardless of WHY it took that long — including a real crash
        whose `timeout` rc got mangled by an unrelated layer.

    Together they isolate the genuine "budget exceeded" case.
    """

    def test_rc124_with_elapsed_near_ceiling_is_timed_out(self):
        # The headline shape: ASan shard hit 1795s of a 1800s ceiling.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 5, 0, 20)
            self._shard(d, "codegen", 1, 0, 0, 0,
                        write_json=False, write_log=False,
                        rc=124, shard_elapsed=1795.0)
            self._shard(d, "codegen", 2, 5, 0, 20)
            self._shard(d, "codegen", 3, 5, 0, 20)
            acc = summary.account(d, 4, ["codegen"], shard_timeout=1800)
            self.assertEqual(acc["timed_out_shards"], ["codegen/1"])
            self.assertEqual(acc["crashed_shards"], [])
            self.assertIn("codegen/1", acc["missing_summary_shards"])

    def test_rc124_with_elapsed_far_below_ceiling_is_still_a_crash(self):
        # The deadlock shape: rc=124 came from `timeout`, but the shard only
        # ran 30 seconds. This is a real crash (a deadlock that happens to
        # outlive a tight ceiling, or any other early-death that `timeout`
        # catches) — must NOT be reclassified as a ceiling kill.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 5, 0, 20)
            self._shard(d, "codegen", 1, 0, 0, 0,
                        write_json=False, write_log=False,
                        rc=124, shard_elapsed=30.0)
            acc = summary.account(d, 2, ["codegen"], shard_timeout=1800)
            self.assertEqual(acc["timed_out_shards"], [])
            self.assertEqual(acc["crashed_shards"], ["codegen/1"])

    def test_rc137_with_elapsed_near_ceiling_is_timed_out(self):
        # `timeout -k 10` (tests/run_parallel.sh:159) SIGKILLs after the grace
        # period, returning 137. Same classification rule applies — this is a
        # ceiling kill, the shard was alive but slow.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 0, 0, 0,
                        write_json=False, write_log=False,
                        rc=137, shard_elapsed=1805.0)
            acc = summary.account(d, 1, ["codegen"], shard_timeout=1800)
            self.assertEqual(acc["timed_out_shards"], ["codegen/0"])
            self.assertEqual(acc["crashed_shards"], [])

    def test_rc139_with_elapsed_near_ceiling_is_a_crash(self):
        # Segfault (rc=139) — even if the elapsed is suspiciously close to the
        # ceiling, rc=139 is NOT a timeout exit code. The ceiling
        # classification must NOT swallow a real segfault.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 0, 0, 0,
                        write_json=False, write_log=False,
                        rc=139, shard_elapsed=1799.5)
            acc = summary.account(d, 1, ["codegen"], shard_timeout=1800)
            self.assertEqual(acc["timed_out_shards"], [])
            self.assertEqual(acc["crashed_shards"], ["codegen/0"])

    def test_rc1_with_elapsed_near_ceiling_is_a_crash(self):
        # Test failure (rc=1) — even if the run took near-ceiling time, a
        # test-failure exit is NOT a timeout. A genuine test failure takes
        # the whole slice to manifest.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 0, 0, 0,
                        write_json=False, write_log=False,
                        rc=1, shard_elapsed=1800.0)
            acc = summary.account(d, 1, ["codegen"], shard_timeout=1800)
            self.assertEqual(acc["timed_out_shards"], [])
            self.assertEqual(acc["crashed_shards"], ["codegen/0"])

    def test_rc124_below_95_percent_of_ceiling_stays_a_crash(self):
        # The 0.95 threshold (CEILING_KILL_THRESHOLD) — 95% gives 5% slack
        # for the SHARD_ELAPSED marker's 1s /proc/uptime resolution and the
        # post-kill cleanup between `timeout` SIGTERM and the subshell
        # recording rc. A shard that died at 94% of the ceiling is most
        # likely a real crash that happened to take ~ceiling time, not a
        # ceiling hit.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 0, 0, 0,
                        write_json=False, write_log=False,
                        rc=124, shard_elapsed=1700.0)  # 94.4% of 1800
            acc = summary.account(d, 1, ["codegen"], shard_timeout=1800)
            self.assertEqual(acc["timed_out_shards"], [])
            self.assertEqual(acc["crashed_shards"], ["codegen/0"])

    def test_no_shard_timeout_keeps_pre_3222_behaviour(self):
        # shard_timeout=None (the pre-#3222 default) — every rc!=0 shard goes
        # to crashed_shards, including rc=124. Back-compat pin: the new
        # classification is opt-in by virtue of the caller passing the
        # ceiling, so a call site that doesn't is unaffected.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 0, 0, 0,
                        write_json=False, write_log=False,
                        rc=124, shard_elapsed=1800.0)
            acc = summary.account(d, 1, ["codegen"], shard_timeout=None)
            self.assertEqual(acc["timed_out_shards"], [])
            self.assertEqual(acc["crashed_shards"], ["codegen/0"])

    def test_rc124_without_elapsed_marker_stays_a_crash(self):
        # A missing SHARD_ELAPSED marker on an rc=124 shard is rare; the
        # conservative rule is to leave it in crashed_shards (rather than
        # reclassify on incomplete evidence). Pin the rule.
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 0, 0, 0,
                        write_json=False, write_log=False,
                        rc=124, shard_elapsed=None)
            # write_log=False above also drops the legacy line; with no log
            # at all extract_shard_elapsed returns None. Reconstruct a log
            # that has the legacy summary but no SHARD_ELAPSED marker.
            _write_log(d, "codegen", 0, "0 passed, 0 failed (shard 0/1), 1 total\n")
            acc = summary.account(d, 1, ["codegen"], shard_timeout=1800)
            self.assertEqual(acc["timed_out_shards"], [])
            self.assertEqual(acc["crashed_shards"], ["codegen/0"])

    def test_multiple_ceiling_kills_all_listed(self):
        # All-codegen-shards-killed-at-ceiling shape — the one that motivated
        # cf84884. Every shard in the list, every one reclassified.
        with tempfile.TemporaryDirectory() as d:
            for i in range(4):
                self._shard(d, "codegen", i, 0, 0, 0,
                            write_json=False, write_log=False,
                            rc=124, shard_elapsed=1800.0)
            acc = summary.account(d, 4, ["codegen"], shard_timeout=1800)
            self.assertEqual(
                acc["timed_out_shards"],
                ["codegen/0", "codegen/1", "codegen/2", "codegen/3"])
            self.assertEqual(acc["crashed_shards"], [])


class TimedOutBannerSectionTest(unittest.TestCase):
    """Narrow #3222: the GATE FAILURE banner surfaces ceiling-killed shards in
    a DISTINCT block ("TIMED OUT at ceiling") before the generic
    "exited nonzero" block, with wording that names the defect class so an
    operator reads "fix the ceiling" rather than "chase the test reader".

    Pin: when timed_out_shards is empty (the pre-#3222 shape), the banner is
    byte-identical for the no-real-defect case.
    """

    def test_timed_out_section_surfaces_count_and_list(self):
        out = summary.format_failure_banner(
            ran=10, expected=10, nbin=2, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=8,
            crashed_culprits=[],
            timed_out_shards=["codegen/0", "codegen/1", "codegen/2",
                              "codegen/3", "codegen/4", "codegen/5",
                              "codegen/6", "codegen/7"])
        # The defect class is named up front — operator sees "TIMED OUT at
        # ceiling" before "exited nonzero".
        self.assertIn("##   8 shard(s) hit the configured per-shard ceiling",
                      out)
        self.assertIn("##   shard(s) TIMED OUT at ceiling: "
                      "codegen/0 codegen/1 codegen/2 codegen/3 "
                      "codegen/4 codegen/5 codegen/6 codegen/7", out)
        # The "raise BEZEL_SHARD_TIMEOUT" hint names the action — narrow
        # #3222 explicitly required the operator-actionable wording.
        self.assertIn("raise BEZEL_SHARD_TIMEOUT", out)
        # No spurious "exited nonzero" lines — timed_out_shards don't carry
        # a died-on attribution (a misattribution per #1608).
        self.assertNotIn("exited nonzero", out)
        self.assertNotIn("died on:", out)

    def test_no_timed_out_shards_omits_section(self):
        # timed_out_shards=None (default) — banner stays byte-identical for
        # the no-real-defect case. Pin: existing consumers see no change.
        out_none = summary.format_failure_banner(
            ran=10, expected=10, nbin=2, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=8,
            crashed_culprits=[])
        out_empty = summary.format_failure_banner(
            ran=10, expected=10, nbin=2, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=8,
            crashed_culprits=[], timed_out_shards=[])
        self.assertEqual(out_none, "")
        self.assertEqual(out_empty, "")
        self.assertNotIn("TIMED OUT", out_none)
        self.assertNotIn("TIMED OUT", out_empty)

    def test_timed_out_and_real_crash_both_surfaced(self):
        # Mixed defect shape — some shards ceiling-killed, one segfault.
        # The banner names BOTH distinctly: ceiling-kills first (operator
        # action: raise the ceiling), then the segfault (operator action:
        # investigate the test).
        out = summary.format_failure_banner(
            ran=10, expected=20, nbin=2, undercount_bins=[],
            subset_empty=False, missing_summary_shards=[], njobs=8,
            crashed_culprits=[("sema/0", "SuiteA.qux")],
            timed_out_shards=["codegen/0", "codegen/1"])
        # TIMED OUT block comes BEFORE the "exited nonzero" block — order
        # is a contract, see the format_failure_banner docstring.
        idx_timed = out.index("TIMED OUT at ceiling:")
        idx_died = out.index("died on:")
        self.assertLess(idx_timed, idx_died,
                        "TIMED OUT section must surface before died-on section")
        # Both lists named.
        self.assertIn("codegen/0 codegen/1", out)
        self.assertIn("died on: SuiteA.qux", out)


class TimeoutCLITest(_Fixture):
    """The new --shard-timeout arg on `account` and --timed-out arg on
    `format-failure`. Pins the CLI plumbing that tests/run_parallel.sh uses
    (the comment on the new arg in summary.py calls these out)."""

    _SCRIPTS_DIR = os.path.join(os.path.dirname(__file__), "..", "..")

    def _run_cli(self, args):
        import subprocess
        env = dict(os.environ, PYTHONPATH=os.path.abspath(self._SCRIPTS_DIR))
        return subprocess.run([sys.executable, "-m", "gate.summary"] + args,
                              capture_output=True, text=True, env=env)

    def test_account_cli_emits_timed_out_shards_list(self):
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 0, 0, 0,
                        write_json=False, write_log=False,
                        rc=124, shard_elapsed=1800.0)
            out = self._run_cli(
                ["account", d, "--conc", "1", "--shard-timeout", "1800",
                 "--", "codegen"])
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("rp_timed_out_shards=('codegen/0')", out.stdout)
            self.assertIn("rp_crashed_shards=()", out.stdout)

    def test_account_cli_without_shard_timeout_keeps_pre_3222(self):
        with tempfile.TemporaryDirectory() as d:
            self._shard(d, "codegen", 0, 0, 0, 0,
                        write_json=False, write_log=False,
                        rc=124, shard_elapsed=1800.0)
            out = self._run_cli(
                ["account", d, "--conc", "1", "--", "codegen"])
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("rp_timed_out_shards=()", out.stdout)
            self.assertIn("rp_crashed_shards=('codegen/0')", out.stdout)

    def test_format_failure_cli_with_timed_out_emits_section(self):
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, "SHARD_ELAPSED 1800.00\n")
            out = self._run_cli([
                "format-failure", "--ran", "8", "--expected", "10",
                "--nbin", "1", "--njobs", "2",
                "--timed-out", "codegen/0", "--logdir", d])
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("TIMED OUT at ceiling", out.stdout)
            self.assertIn("codegen/0", out.stdout)

    def test_format_failure_cli_without_timed_out_omits_section(self):
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, "[ RUN  ] Last.Run\n")
            out = self._run_cli([
                "format-failure", "--ran", "8", "--expected", "10",
                "--nbin", "1", "--njobs", "2",
                "--crashed", "codegen/0", "--logdir", d])
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("died on: Last.Run", out.stdout)
            self.assertNotIn("TIMED OUT", out.stdout)


class PerShardCeilingRatioLogTest(unittest.TestCase):
    """Narrow #3222: per-shard wall-time log now records the elapsed/ceiling
    ratio so a reader can see how close shards got to BEZEL_SHARD_TIMEOUT —
    a high-water mark that lets real cycles answer whether the cf84884 x2
    multiplier is sized right (the body file: "let real cycles answer it,
    rather than tuning the constant on the strength of one ratio").

    The loop lives in tests/run_parallel.sh. The pin is bash-level (the awk
    ratio calculation has its own edge cases), so we drive the loop via
    subprocess sourcing the actual run_parallel.sh with a mocked
    run_parallel's main harness, against a minimal mocked logdir.
    """

    _REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__),
                                              "..", "..", ".."))
    _RUN_PARALLEL_SH = os.path.join(_REPO_ROOT, "tests", "run_parallel.sh")

    def _run_loop(self, logdir, conc, sto):
        """Source tests/run_parallel.sh's per-shard timing loop with a
        mocked `names` / `nbin` so the loop body runs against our logdir
        WITHOUT launching the real shard machinery. This pins the loop AS
        IT IS WRITTEN in the source, not a parallel copy — a drift between
        source and test would fail loudly.

        We extract JUST the for-for block (the comment + the two nested
        for loops that follow), not everything from the comment onwards,
        so the snippet doesn't pull in later references like `subset_mode`
        / `bin_pass` that the parent harness sets up."""
        import subprocess
        # Stub the parent-harness vars the loop body touches (nbin / conc /
        # names / logdir). BEZEL_SHARD_TIMEOUT is in the env (set below),
        # and the loop reads it via ${BEZEL_SHARD_TIMEOUT:-} so it never
        # touches an unbound name there.
        prelude = (
            "nbin=1\n"
            "conc={conc}\n"
            "names=(codegen)\n"
            "logdir=\"$1\"\n"
            # sto is set inline by the loop body so the unset/zero tests can
            # exercise the [[ -n \"$sto\" ]] / [[ \"$sto\" -gt 0 ]] guards
            # without us overriding BEZEL_SHARD_TIMEOUT in env. Override
            # BEZEL_SHARD_TIMEOUT in env (per-call) instead — the loop's
            # fallback handles the unset case naturally.
        ).format(conc=conc)
        with open(self._RUN_PARALLEL_SH) as f:
            content = f.read()
        # Find the per-shard wall-times block: the comment, the two nested
        # for loops, and the matching `done`s. We anchor on the comment and
        # the next blank line / next non-indented line.
        idx = content.find("# Per-shard wall times")
        if idx < 0:
            self.fail("per-shard wall-times block not found in run_parallel.sh")
        # Walk forward from the comment, collecting lines until we see the
        # first line that doesn't belong to the block. The block is two
        # nested for/if/done constructs followed by a blank line; we stop
        # at the first blank line AFTER at least one done was seen.
        body_lines = []
        seen_done = 0
        for line in content[idx:].splitlines():
            body_lines.append(line)
            if line.strip() == "done":
                seen_done += 1
                if seen_done == 2:
                    break
        body = "\n".join(body_lines)
        env = dict(os.environ, BEZEL_SHARD_TIMEOUT=str(sto))
        return subprocess.run(
            ["bash", "-c", prelude + body, "_", logdir],
            capture_output=True, text=True, env=env)

    def test_ceiling_ratio_logged_when_both_set(self):
        # The shape that motivated the log: a shard cleared the ceiling by
        # exactly the measured ratio (1.88x), so the next ASan cycle's ceiling
        # choice is observable in the per-shard line.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0,
                       "12 passed, 0 failed (shard 0/4), 40 total\n"
                       "SHARD_ELAPSED 1795.50\n")
            out = self._run_loop(d, 4, 1800)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("shard codegen.0: 1795.50s (ceiling 1800s, 99.8%)",
                          out.stdout)

    def test_ceiling_ratio_logged_near_ceiling_for_asan(self):
        # ASan pass — the 1.9x ratio is the headline. A shard at 1915s of a
        # 1800s ceiling (the cf84884 measurement) logs the right ratio so a
        # reader sees 106.4% — confirming the shard cleared the plain
        # ceiling AND hit the ASan ceiling (the very defect cf84884 fixed).
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0,
                       "SHARD_ELAPSED 1915.61\n")
            out = self._run_loop(d, 1, 1800)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("106.4%", out.stdout)

    def test_no_ceiling_in_env_falls_back_to_bare_elapsed(self):
        # The pre-#3222 path — no BEZEL_SHARD_TIMEOUT in env. Output must NOT
        # fabricate a ratio, and must remain byte-identical for this case so
        # existing consumers (gate_full_cycle.sh, gate.sh) see no change.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, "SHARD_ELAPSED 12.34\n")
            out = self._run_loop(d, 1, "")  # empty sto
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("shard codegen.0: 12.34s", out.stdout)
            self.assertNotIn("ceiling", out.stdout)
            self.assertNotIn("%", out.stdout)

    def test_missing_elapsed_marker_skips_line(self):
        # A shard that died before printing the marker contributes no line —
        # matches the bash `[[ -n "$et" ]]` guard. Back-compat: the loop is
        # observation-only, so a missing line is silent (not an error).
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0,
                       "12 passed, 0 failed (shard 0/1), 40 total\n")
            out = self._run_loop(d, 1, 1800)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertEqual(out.stdout.strip(), "")

    def test_zero_ceiling_falls_back_to_bare_elapsed(self):
        # Defence-in-depth: an explicit zero BEZEL_SHARD_TIMEOUT must NOT
        # trigger awk's divide-by-zero (it would print "inf%" which is a
        # worse misleading signal than a missing ratio). The `sto > 0`
        # guard short-circuits the same way as unset.
        with tempfile.TemporaryDirectory() as d:
            _write_log(d, "codegen", 0, "SHARD_ELAPSED 12.34\n")
            out = self._run_loop(d, 1, 0)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("shard codegen.0: 12.34s", out.stdout)
            self.assertNotIn("ceiling", out.stdout)

    def test_multiple_shards_all_have_ratios(self):
        # The full-suite shape — every shard's ratio is logged, so a reader
        # can spot the slowest shard and the cluster near the ceiling.
        with tempfile.TemporaryDirectory() as d:
            for i in range(3):
                _write_log(d, "codegen", i,
                           "SHARD_ELAPSED {}\n".format(100.0 + i * 50))
            out = self._run_loop(d, 3, 1000)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("shard codegen.0: 100.0s (ceiling 1000s, 10.0%)",
                          out.stdout)
            self.assertIn("shard codegen.1: 150.0s (ceiling 1000s, 15.0%)",
                          out.stdout)
            self.assertIn("shard codegen.2: 200.0s (ceiling 1000s, 20.0%)",
                          out.stdout)


if __name__ == "__main__":
    unittest.main()
