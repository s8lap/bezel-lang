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


if __name__ == "__main__":
    unittest.main()
