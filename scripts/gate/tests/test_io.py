"""Tests for gate.io (#3010, Phase 0).

The load-bearing case is the atomic_json_write CRASH INVARIANT: a simulated
mid-write crash (os.replace raises) must leave the target at its prior state
(or absent if it had none) and must not leave a partial file at the target
path. That is the #3000/#3003 shape this helper exists to make impossible, and
it is the foundation every later phase composes on.
"""

import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import io  # noqa: E402


class AtomicJsonWriteTest(unittest.TestCase):
    def test_writes_pretty_and_reads_back(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "status.json")
            io.atomic_json_write(p, {"result": "pass", "rc": 0})
            with open(p) as f:
                self.assertEqual(json.load(f), {"result": "pass", "rc": 0})

    def test_overwrites_existing_target_wholly(self):
        # The target must be the NEW document in full, not new-merged-with-old
        # and not new-appended-to-old.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "t.json")
            io.atomic_json_write(p, {"old": True, "padding": "X" * 500})
            io.atomic_json_write(p, {"new": True})
            with open(p) as f:
                self.assertEqual(json.load(f), {"new": True})

    def test_creates_parent_dirs(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "nested", "deeper", "t.json")
            io.atomic_json_write(p, {"a": 1})
            self.assertTrue(os.path.isfile(p))

    def test_crash_before_replace_leaves_existing_target_intact(self):
        # The crash invariant: simulate a mid-write crash by making the final
        # os.replace raise. The pre-existing target must be UNCHANGED (the old
        # bytes, still valid JSON), and no temp must be left behind in the dir.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "t.json")
            io.atomic_json_write(p, {"prior": True, "keep": "me"})
            with open(p, "rb") as f:
                prior_bytes = f.read()

            with mock.patch("gate.io.os.replace", side_effect=OSError("boom")):
                with self.assertRaises(OSError):
                    io.atomic_json_write(p, {"should": "not land"})

            # Target still holds the prior document, byte-for-byte.
            with open(p, "rb") as f:
                self.assertEqual(f.read(), prior_bytes)
            with open(p) as f:
                self.assertEqual(json.load(f), {"prior": True, "keep": "me"})
            # No partial file at the target name, no orphaned temp left behind.
            leftovers = [
                n for n in os.listdir(d)
                if n != "t.json" and n.endswith(".tmp")
            ]
            self.assertEqual(leftovers, [], f"orphaned temps: {leftovers}")

    def test_crash_before_replace_when_no_prior_target_leaves_none(self):
        # If there was no target to begin with, a crash must leave NO target at
        # all — not a zero-byte file and not a partial document. "Nothing was
        # checked" (exit 2) and "checked, all green" (exit 0) must stay
        # distinguishable after a crash.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "fresh.json")
            with mock.patch("gate.io.os.replace", side_effect=OSError("boom")):
                with self.assertRaises(OSError):
                    io.atomic_json_write(p, {"x": 1})
            self.assertFalse(os.path.exists(p))


class ReadJsonlTest(unittest.TestCase):
    def test_reads_records_and_skips_blanks(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "log.jsonl")
            with open(p, "w") as f:
                f.write('{"a": 1}\n\n  \n{"a": 2}\n')
            self.assertEqual(io.read_jsonl(p), [{"a": 1}, {"a": 2}])

    def test_skips_unparseable_silently(self):
        # The general-purpose helper does NOT fail loud; trend.py's newest-line
        # refusal is its own, richer reader on purpose.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "log.jsonl")
            with open(p, "w") as f:
                f.write('{"a": 1}\n{not json}\n{"a": 3}\n')
            self.assertEqual(io.read_jsonl(p), [{"a": 1}, {"a": 3}])

    def test_empty_file_yields_empty_list(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "empty.jsonl")
            open(p, "w").close()
            self.assertEqual(io.read_jsonl(p), [])


if __name__ == "__main__":
    unittest.main()
