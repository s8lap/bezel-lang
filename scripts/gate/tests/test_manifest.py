"""Tests for gate.manifest (Phase 5, #3010 — build/tests/test_binaries.txt).

The load-bearing pins the task requires:
  1. read_manifest parses `<group>\\t<path>\\t<#sources>` (the CMake format,
     tests/CMakeLists.txt:242-243; run_parallel.sh:53 `IFS=$'\\t' read -r n p w`).
  2. read_manifest HARD-FAILS on a malformed row and on an EMPTY manifest — a
     manifest a consumer cannot trust must never be silently truncated (#3000
     shape: plausible-looking garbage a reader proceeds on). A missing binary's
     row is the #3011 concern.
  3. partition_is_complete — the two CMake-enforced partition invariants
     (Plan-A 1.13 / tests/CMakeLists.txt:25-44) as a read-time defense:
       - no source in TWO groups (double-count ⇒ sum too big)
       - the union == the flat tests/**/*.cpp glob (an unclaimed dir or a source
         in NO group ⇒ sum too small)

Plus: blank-line skipping, the Binary namedtuple-like value, the shared-entry-
point adjustment (test_main.cpp is in the glob but not in any group's count).
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import manifest  # noqa: E402


def _write(path, text):
    with open(path, "w") as f:
        f.write(text)


class ReadManifestTest(unittest.TestCase):
    """Pin 1 + 2: parse the CMake format; hard-fail on malformed/empty."""

    def test_parses_well_formed_rows(self):
        # tests/CMakeLists.txt:242-243: `<group>\t$<TARGET_FILE:...>\t<#sources>`.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "test_binaries.txt")
            _write(p,
                   "codegen\t/build/tests/bezel_tests_codegen\t1200\n"
                   "ownership\t/build/tests/bezel_tests_ownership\t300\n"
                   "support\t/build/tests/bezel_tests_support\t50\n")
            rows = manifest.read_manifest(p)
            self.assertEqual(len(rows), 3)
            self.assertEqual(rows[0],
                             manifest.Binary("codegen",
                                             "/build/tests/bezel_tests_codegen",
                                             1200))
            self.assertEqual(rows[1].group, "ownership")
            self.assertEqual(rows[1].num_sources, 300)
            self.assertEqual(rows[2].path, "/build/tests/bezel_tests_support")
            self.assertEqual(rows[2].num_sources, 50)

    def test_skips_blank_lines(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "tb.txt")
            _write(p,
                   "sema\t/build/b_sema\t10\n"
                   "\n"
                   "   \n"
                   "module\t/build/b_module\t5\n")
            rows = manifest.read_manifest(p)
            self.assertEqual([r.group for r in rows], ["sema", "module"])

    def test_num_sources_is_int(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "tb.txt")
            _write(p, "g\t/build/b\t42\n")
            [row] = manifest.read_manifest(p)
            self.assertIsInstance(row.num_sources, int)
            self.assertEqual(row.num_sources, 42)

    def test_hard_fail_wrong_field_count(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "tb.txt")
            _write(p, "codegen\t/build/b\n")  # only 2 fields
            with self.assertRaises(ValueError) as cm:
                manifest.read_manifest(p)
            self.assertIn("3 tab-separated fields", str(cm.exception))

    def test_hard_fail_empty_group_or_path(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "tb.txt")
            _write(p, "\t/build/b\t10\n")
            with self.assertRaises(ValueError):
                manifest.read_manifest(p)

    def test_hard_fail_non_integer_sources(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "tb.txt")
            _write(p, "codegen\t/build/b\tnotanumber\n")
            with self.assertRaises(ValueError) as cm:
                manifest.read_manifest(p)
            self.assertIn("#sources is not an integer", str(cm.exception))

    def test_hard_fail_negative_sources(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "tb.txt")
            _write(p, "codegen\t/build/b\t-1\n")
            with self.assertRaises(ValueError):
                manifest.read_manifest(p)

    def test_hard_fail_empty_manifest(self):
        # per tests/CMakeLists.txt:26-29: an empty manifest means the suite would
        # silently vanish — the gate must NEVER proceed on zero binaries.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "tb.txt")
            _write(p, "")
            with self.assertRaises(ValueError) as cm:
                manifest.read_manifest(p)
            self.assertIn("empty", str(cm.exception))

    def test_hard_fail_whitespace_only_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "tb.txt")
            _write(p, "\n\n   \n\n")
            with self.assertRaises(ValueError):
                manifest.read_manifest(p)

    def test_missing_file_raises_file_not_found(self):
        # gate.sh / run_parallel.sh refuse preflight with a message; the reader
        # itself surfaces FileNotFoundError so the caller distinguishes "absent"
        # (reconfigure needed) from "present but malformed" (ValueError).
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(FileNotFoundError):
                manifest.read_manifest(os.path.join(d, "nope.txt"))

    def test_space_is_not_a_field_separator(self):
        # The separator is a TAB (CMake emits `\t`; run_parallel reads IFS=$'\t').
        # A space-separated row has ONE field ⇒ hard-fail, not a silent mis-split.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "tb.txt")
            _write(p, "codegen /build/b 1200\n")
            with self.assertRaises(ValueError):
                manifest.read_manifest(p)


class BinaryTest(unittest.TestCase):
    def test_equality_and_hash(self):
        b1 = manifest.Binary("g", "/p", 3)
        b2 = manifest.Binary("g", "/p", 3)
        b3 = manifest.Binary("g", "/p", 4)
        self.assertEqual(b1, b2)
        self.assertNotEqual(b1, b3)
        self.assertEqual(hash(b1), hash(b2))
        # Usable in a set (partition_is_complete relies on unique-group checks).
        self.assertEqual(len({b1, b2, b3}), 2)

    def test_repr(self):
        b = manifest.Binary("codegen", "/build/b", 10)
        self.assertIn("codegen", repr(b))
        self.assertIn("/build/b", repr(b))


class PartitionIsCompleteTest(unittest.TestCase):
    """Pin 3: the read-time defense over the two CMake partition invariants
    (Plan-A 1.13 / tests/CMakeLists.txt:25-44). The manifest carries only
    per-group COUNTS, so the check is aggregate — but that is enough to catch a
    broken partition loudly."""

    def _g(self, n):
        # A synthetic flat glob of n test *.cpp paths (INCLUDING the shared entry
        # point test_main.cpp, as the on-disk glob would see it).
        return ["tests/dir%d/file%d.cpp" % (i, i) for i in range(n)]

    def test_valid_partition_matches_glob(self):
        # 5 partitioned sources + 1 shared entry point = 6 in the glob. The
        # manifest declares the 5 partitioned across two groups.
        rows = [manifest.Binary("codegen", "/b1", 3),
                manifest.Binary("sema", "/b2", 2)]
        self.assertTrue(manifest.partition_is_complete(rows, self._g(6)))

    def test_double_counted_source_is_caught(self):
        # Invariant: no source in TWO groups. A double-count inflates the sum
        # above (glob - entry), so the union no longer matches the flat glob.
        rows = [manifest.Binary("codegen", "/b1", 3),
                manifest.Binary("sema", "/b2", 3)]  # 6 declared, but glob-1=5
        self.assertFalse(manifest.partition_is_complete(rows, self._g(6)))

    def test_unclaimed_dir_is_caught(self):
        # Invariant: no UNCLAIMED area / no source in NO group. An unclaimed
        # tests/<dir>/ adds sources to the glob the manifest does not declare ⇒
        # sum < (glob - entry). This is the read-time analog of the CMake
        # FATAL_ERROR at tests/CMakeLists.txt:82-97.
        rows = [manifest.Binary("codegen", "/b1", 3),
                manifest.Binary("sema", "/b2", 1)]  # 4 declared, but glob-1=5
        self.assertFalse(manifest.partition_is_complete(rows, self._g(6)))

    def test_empty_group_rejected(self):
        # A binary with zero sources is a phantom group — never valid.
        rows = [manifest.Binary("codegen", "/b1", 0),
                manifest.Binary("sema", "/b2", 5)]
        self.assertFalse(manifest.partition_is_complete(rows, self._g(6)))

    def test_duplicate_group_rejected(self):
        # Two binaries claiming the same group — a malformed partition (the group
        # name names per-shard log files; run_parallel.sh:42-45 relies on unique
        # labels). The count may match, but the partition is still broken.
        rows = [manifest.Binary("codegen", "/b1", 3),
                manifest.Binary("codegen", "/b2", 2)]
        self.assertFalse(manifest.partition_is_complete(rows, self._g(6)))

    def test_duplicate_path_rejected(self):
        rows = [manifest.Binary("codegen", "/same", 3),
                manifest.Binary("sema", "/same", 2)]  # same binary path twice
        self.assertFalse(manifest.partition_is_complete(rows, self._g(6)))

    def test_empty_manifest_rejected(self):
        self.assertFalse(manifest.partition_is_complete([], self._g(1)))

    def test_shared_entry_point_adjustment(self):
        # The shared entry point (test_main.cpp) is in the glob but NOT in any
        # group's count — it compiles into EVERY binary
        # (tests/CMakeLists.txt:71-74, 109, 138). So manifest-total == glob-1.
        rows = [manifest.Binary("support", "/b", 5)]
        # glob of 6 ⇒ 6 - 1 entry = 5 declared ⇒ complete.
        self.assertTrue(manifest.partition_is_complete(rows, self._g(6)))
        # glob of 5 ⇒ 5 - 1 = 4 != 5 ⇒ the entry is double-counted ⇒ incomplete.
        self.assertFalse(manifest.partition_is_complete(rows, self._g(5)))

    def test_num_shared_entry_override(self):
        # A caller whose glob already EXCLUDES the entry point passes 0.
        rows = [manifest.Binary("support", "/b", 5)]
        self.assertTrue(manifest.partition_is_complete(rows, self._g(5),
                                                      num_shared_entry=0))


class CLITest(unittest.TestCase):
    """`python3 -m gate.manifest binaries` — the Wave-2 gate.sh bridge (#3010).

    Replaces the old `cut -f2 build/tests/test_binaries.txt` hand-parse. Pins
    that the CLI prints one path per line and that a malformed manifest fails
    loudly (the loose cut used to silently skip a bad row).
    """

    _SCRIPTS_DIR = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", ".."))

    def _run_cli(self, args):
        import subprocess
        env = dict(os.environ, PYTHONPATH=self._SCRIPTS_DIR)
        return subprocess.run(
            [sys.executable, "-m", "gate.manifest"] + args,
            capture_output=True, text=True, env=env)

    def test_binaries_prints_paths_one_per_line(self):
        with tempfile.TemporaryDirectory() as d:
            mp = os.path.join(d, "test_binaries.txt")
            with open(mp, "w") as f:
                f.write("codegen\t/build/b1\t42\n")
                f.write("sema\t/build/b2\t17\n")
            r = self._run_cli(["binaries", "--path", mp])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.splitlines(), ["/build/b1", "/build/b2"])

    def test_binaries_skips_blank_lines(self):
        with tempfile.TemporaryDirectory() as d:
            mp = os.path.join(d, "test_binaries.txt")
            with open(mp, "w") as f:
                f.write("\ncodegen\t/build/b1\t42\n\n")
            r = self._run_cli(["binaries", "--path", mp])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.splitlines(), ["/build/b1"])

    def test_binaries_fails_loudly_on_malformed_row(self):
        # The loose `cut -f2` would silently print a partial field; read_manifest
        # raises (a manifest a consumer cannot trust is FATAL, #3000 shape).
        with tempfile.TemporaryDirectory() as d:
            mp = os.path.join(d, "test_binaries.txt")
            with open(mp, "w") as f:
                f.write("garbage row with no tabs\n")
            r = self._run_cli(["binaries", "--path", mp])
            self.assertNotEqual(r.returncode, 0)

    def test_binaries_fails_on_empty_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            mp = os.path.join(d, "test_binaries.txt")
            open(mp, "w").close()
            r = self._run_cli(["binaries", "--path", mp])
            self.assertNotEqual(r.returncode, 0)

    def test_no_subcommand_exits_2(self):
        r = self._run_cli([])
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()
