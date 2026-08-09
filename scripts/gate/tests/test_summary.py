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
               write_log=True, legacy_extra=""):
        """Write one shard's rc, JSON, and legacy log line. `selected=None`
        means non-subset (no 'selected' in the legacy line); an int means
        subset mode (the line carries ', N selected')."""
        self._rc(logdir, name, idx, rc)
        if write_json:
            obj = {"schema": schema, "binary": "bin/" + name,
                   "shard_index": idx, "shard_count": 4,
                   "passed": passed, "failed": failed, "total": total,
                   "selected": selected if selected is not None else total,
                   "asan_mode": False, "rc": (0 if failed == 0 else 1),
                   "elapsed_s": 1.0}
            _write_json(logdir, name, idx, obj)
        if write_log:
            line = "{} passed, {} failed (shard {}/4), {} total".format(
                passed, failed, idx, total)
            if selected is not None:
                line = "{} passed, {} failed, {} selected, {} total".format(
                    passed, failed, selected, total)
            _write_log(logdir, name, idx, legacy_extra + line + "\n")

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


if __name__ == "__main__":
    unittest.main()
