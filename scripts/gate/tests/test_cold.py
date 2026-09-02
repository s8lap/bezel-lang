"""Tests for gate.cold (Phase 5, #3010 / #2100 / #2918 / #3000).

The load-bearing pins the task requires:
  1. the cold TRUTH TABLE — COLD iff identity-changed OR cache_n<2000 OR
     marker-changed; else WARM. Exhaustive over the three boolean signals plus
     the cache_n threshold boundary.
  2. count_cache uses `-L` — a symlinked cache root returns the REAL count, not
     0 (the #3000 defect: find without -L returned 0 against 102,299 entries).
  3. the ceiling is cold=1800s / warm=900s for BOTH oracles (#2918 hoisted the
     plain warm ceiling off the 300s hardcoded fallback).

Plus: evaluate() returns the ColdResult triple; count_cache fail-safe (0 on a
missing root); the threshold boundary (1999 cold, 2000 warm).
  4. (#3690) count_cache counts ENTRIES: the `<entry>.sz` size sidecars a
     no-fsync-mode store writes (#3669) and in-flight `.tmp.` temps (#3003)
     are excluded, so cache_n cannot double toward the TIGHTER warm ceiling.
"""

import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import cold  # noqa: E402


class IsColdTruthTableTest(unittest.TestCase):
    """Pin 1: the cold predicate. COLD iff ANY signal fires; else WARM.

    The three signals (gate.sh:600-628): identity_changed, cache_n<2000,
    marker_changed. WARM requires ALL THREE quiet AND a non-trivial cache.
    """

    def test_all_quiet_and_full_cache_is_warm(self):
        # The only WARM case: core.a NOT rebuilt, cache full, marker matches.
        self.assertFalse(cold.is_cold(False, 50000, False))

    def test_identity_changed_implies_cold(self):
        # Signal 1 (gate.sh:601): core.a rebuilt this gate ⇒ cold regardless.
        for n in (0, 1999, 2000, 100000):
            self.assertTrue(cold.is_cold(True, n, False),
                            "identity_changed with cache_n=%d should be COLD" % n)

    def test_near_empty_cache_implies_cold(self):
        # Signal 2 (gate.sh:601): cache_n < 2000 ⇒ cold regardless.
        self.assertTrue(cold.is_cold(False, 0, False))
        self.assertTrue(cold.is_cold(False, 1, False))
        self.assertTrue(cold.is_cold(False, 1999, False))

    def test_marker_changed_implies_cold(self):
        # Signal 3 (gate.sh:622-625): recorded identity != computed ⇒ cold
        # regardless. THE load-bearing signal: a timed-out previous gate leaves a
        # full-looking store under a STALE identity; only the marker catches it.
        for n in (0, 1999, 2000, 100000):
            self.assertTrue(cold.is_cold(False, n, True),
                            "marker_changed with cache_n=%d should be COLD" % n)

    def test_threshold_boundary_2000_is_warm(self):
        # cache_n == 2000 is NOT < 2000, so with the other two signals quiet it
        # is WARM (the boundary belongs to the warm side). 1999 is cold.
        self.assertFalse(cold.is_cold(False, 2000, False))
        self.assertTrue(cold.is_cold(False, 1999, False))

    def test_any_two_signals_still_cold(self):
        # OR semantics: it only takes one. Combinations confirm no short-circuit
        # accidentally masks a firing signal.
        import itertools
        for ic, mc in itertools.product([False, True], repeat=2):
            with self.subTest(identity_changed=ic, marker_changed=mc):
                # cache_n below threshold ⇒ cold no matter the other two.
                self.assertTrue(cold.is_cold(ic, 1999, mc))
                # And WARM needs all three quiet:
                if not ic and not mc:
                    self.assertFalse(cold.is_cold(ic, 50000, mc))
                else:
                    self.assertTrue(cold.is_cold(ic, 50000, mc))

    def test_return_type_is_python_bool(self):
        # Callers (`if cold:`) rely on truthiness, but the contract is a bool,
        # not e.g. an int that happens to be 0/1.
        self.assertIs(cold.is_cold(False, 50000, False), False)
        self.assertIs(cold.is_cold(True, 50000, False), True)


class CeilingTest(unittest.TestCase):
    """Pin 3: cold=1800s, warm=900s, BOTH oracles. per #2918 / gate.sh:629-662."""

    def test_cold_ceiling_is_1800(self):
        self.assertEqual(cold.ceiling(True), 1800)
        self.assertEqual(cold.CEILING_COLD, 1800)

    def test_warm_ceiling_is_900(self):
        self.assertEqual(cold.ceiling(False), 900)
        self.assertEqual(cold.CEILING_WARM, 900)

    def test_warm_is_not_the_old_300_fallback(self):
        # #3000 follow-on / f60c1cb: the plain oracle used to fall through to
        # run_parallel.sh's hardcoded 300s and was killed at it (5156 of 16482
        # tests). 900s, NOT 300s, is the warm ceiling now.
        self.assertNotEqual(cold.CEILING_WARM, 300)
        self.assertGreater(cold.CEILING_WARM, 300)

    def test_cold_ceiling_is_higher_than_warm(self):
        self.assertGreater(cold.CEILING_COLD, cold.CEILING_WARM)


class CountCacheFindLTest(unittest.TestCase):
    """Pin 2: count_cache uses `find -L` — a symlinked root returns the real
    count, not 0. per #3000 (gate.sh:587-599)."""

    def _seed(self, root, n):
        os.makedirs(root)
        for i in range(n):
            with open(os.path.join(root, "entry%d.bin" % i), "wb") as f:
                f.write(b"x")

    def test_counts_real_directory(self):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "bincache")
            self._seed(cache, 7)
            self.assertEqual(cold.count_cache(cache), 7)

    def test_find_L_load_bearing_symlinked_root(self):
        # THE #3000 pin. The default cache root is a SYMLINK on this box
        # (/var/tmp/bezel-scratch/aot-bincache -> /mnt/fast/aot-bincache), and
        # `find` without -L does not traverse a symlinked START POINT. The real
        # store held 102,299 entries; the bash read 0, so cache_n<2000 always
        # fired and the gate always predicted COLD. count_cache MUST follow the
        # symlink and return the real count.
        with tempfile.TemporaryDirectory() as d:
            real = os.path.join(d, "real-store")
            self._seed(real, 5)
            link = os.path.join(d, "linked-store")
            os.symlink(real, link)
            self.assertEqual(cold.count_cache(link), 5,
                             "symlinked cache root must return the REAL count, "
                             "not 0 (the -L in find is load-bearing, #3000)")

    def test_invokes_find_with_dash_L(self):
        # The -L must be literal in the argv (auditable), not an os.walk
        # equivalent: the faithful port of gate.sh:599 is `find -L <root> -type f`
        # (the #3690 entry-only exclusions follow it; see
        # CountCacheCountsEntriesOnlyTest.test_argv_excludes_sidecars_and_temps_literally).
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            self._seed(cache, 2)
            seen = {}

            class FakeRun:
                def __init__(self, argv_returned):
                    seen["argv"] = argv_returned

            real_run = subprocess.run

            def capture(argv, **kw):
                seen["argv"] = argv
                return real_run(argv, **kw)

            try:
                import gate.cold as c
                orig = c.subprocess.run
                c.subprocess.run = capture
                self.assertEqual(c.count_cache(cache), 2)
            finally:
                c.subprocess.run = orig
            self.assertEqual(seen["argv"][:2], ["find", "-L"],
                             "argv must start with `find -L`: %r" % (seen["argv"],))

    def test_empty_store_returns_zero(self):
        with tempfile.TemporaryDirectory() as d:
            empty = os.path.join(d, "empty")
            os.makedirs(empty)
            self.assertEqual(cold.count_cache(empty), 0)

    def test_missing_root_returns_zero(self):
        # An absent/inaccessible store reads 0 — fail-safe: cache_n<2000 ⇒ cold
        # by signal 2 regardless, so a missing store is correctly predicted cold.
        self.assertEqual(cold.count_cache("/var/tmp/bezel-scratch/no-such-store-xyz"),
                         0)

    def test_does_not_count_subdirectories_as_files(self):
        # `-type f` — only regular files, not the subdir entries themselves.
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            os.makedirs(os.path.join(cache, "sub"))
            for name in ("a.bin", "b.bin", "sub/c.bin"):
                with open(os.path.join(cache, name), "wb") as f:
                    f.write(b"x")
            self.assertEqual(cold.count_cache(cache), 3)


class CountCacheCountsEntriesOnlyTest(unittest.TestCase):
    """Pin 4 (#3690): count_cache counts cache ENTRIES, not every file.

    A no-fsync-mode store (BEZEL_AOT_STORE_FSYNC=0, #3669 — the mode the gate
    runs in since #3690) writes a `<entry>.sz` size sidecar beside every entry.
    A bare `find -type f` counts both, so cache_n doubles for such entries and
    COLD_CACHE_THRESHOLD=2000 stops meaning 2000 entries and starts meaning
    ~1000. That error is NOT fail-safe: it biases the verdict toward WARM, whose
    900s ceiling is the TIGHTER one — a shard killed at the ceiling on a store
    that is colder than the count says. In-flight `.tmp.` store temps (#3003)
    are not entries either. Pin 2's `-L` still holds with the exclusions.
    """

    def _seed(self, root, n, sidecars=False):
        os.makedirs(root, exist_ok=True)
        for i in range(n):
            entry = os.path.join(root, "%064x" % i)   # entries are 64-hex keys
            with open(entry, "wb") as f:
                f.write(b"x")
            if sidecars:
                with open(entry + ".sz", "w") as f:
                    f.write("1")

    def test_size_sidecars_are_not_entries(self):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            self._seed(cache, 7, sidecars=True)
            self.assertEqual(cold.count_cache(cache), 7,
                             "N entries + N `.sz` sidecars must count N, not 2N")

    def test_sidecars_cannot_lift_a_cold_store_over_the_threshold(self):
        # THE error-direction pin. 1500 real entries is COLD (< 2000). Counted
        # with their 1500 sidecars the store reads 3000 => WARM => the 900s
        # ceiling on a store that will miss nearly everything. cache_n is 1500.
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            self._seed(cache, 1500, sidecars=True)
            r = cold.evaluate(False, cache, False)
            self.assertEqual(r.cache_n, 1500)
            self.assertIs(r.cold, True)
            self.assertEqual(r.ceiling, cold.CEILING_COLD)

    def test_store_temps_are_not_entries(self):
        # The three temp shapes the harness writes (tests/support/aot_support.hpp:
        # the entry temp `<key>.tmp.<pid>.<n>`, the copy temp `.tmp.<pid>.<n>`
        # and the sidecar temp `.tmp.sz.<pid>.<n>`). The store's own GC never
        # counts them as entries either (#3003).
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            self._seed(cache, 3)
            for name in ("%064x.tmp.4242.0" % 9, ".tmp.4242.1", ".tmp.sz.4242.2"):
                with open(os.path.join(cache, name), "wb") as f:
                    f.write(b"x")
            self.assertEqual(cold.count_cache(cache), 3)

    def test_symlinked_root_with_sidecars_keeps_the_real_entry_count(self):
        # #3000 x #3690: the exclusions must not cost the -L. A symlinked root
        # holding 5 entries + 5 sidecars reads 5 — not 0 and not 10.
        with tempfile.TemporaryDirectory() as d:
            real = os.path.join(d, "real-store")
            self._seed(real, 5, sidecars=True)
            link = os.path.join(d, "linked-store")
            os.symlink(real, link)
            self.assertEqual(cold.count_cache(link), 5)

    def test_argv_excludes_sidecars_and_temps_literally(self):
        # Auditable in the argv, like the -L: `! -name '*.sz'` and
        # `! -name '*.tmp.*'` after `-type f`.
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            self._seed(cache, 1)
            seen = {}
            real_run = subprocess.run

            def capture(argv, **kw):
                seen["argv"] = argv
                return real_run(argv, **kw)

            try:
                cold.subprocess.run = capture
                self.assertEqual(cold.count_cache(cache), 1)
            finally:
                cold.subprocess.run = real_run
            argv = seen["argv"]
            self.assertEqual(argv[:5], ["find", "-L", cache, "-type", "f"], argv)
            triples = [argv[i:i + 3] for i in range(5, len(argv))]
            self.assertIn(["!", "-name", "*.sz"], triples, argv)
            self.assertIn(["!", "-name", "*.tmp.*"], triples, argv)


class EvaluateTest(unittest.TestCase):
    """evaluate() returns the ColdResult(cache_n, cold, ceiling) triple."""

    def test_warm_store(self):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            os.makedirs(cache)
            for i in range(3000):
                open(os.path.join(cache, "e%d" % i), "wb").close()
            r = cold.evaluate(False, cache, False)
            self.assertEqual(r.cache_n, 3000)
            self.assertIs(r.cold, False)
            self.assertEqual(r.ceiling, 900)

    def test_cold_via_identity(self):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            os.makedirs(cache)
            for i in range(3000):
                open(os.path.join(cache, "e%d" % i), "wb").close()
            r = cold.evaluate(True, cache, False)  # core.a rebuilt this gate
            self.assertIs(r.cold, True)
            self.assertEqual(r.ceiling, 1800)

    def test_cold_via_near_empty_overrides_full_marker(self):
        # Even with identity stable and marker matching, a near-empty store is
        # cold — the count is its own independent signal.
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            os.makedirs(cache)
            open(os.path.join(cache, "only-one"), "wb").close()
            r = cold.evaluate(False, cache, False)
            self.assertEqual(r.cache_n, 1)
            self.assertIs(r.cold, True)
            self.assertEqual(r.ceiling, 1800)


class CLITest(unittest.TestCase):
    """`python3 -m gate.cold evaluate` — the Wave-2 gate.sh bridge (#3010).

    Pins the shell-sourceable stdout contract gate.sh evals: cache_n / cold /
    ceiling, each on its own line, with cold as 0/1 (bash-testable).
    """

    _SCRIPTS_DIR = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", ".."))

    def _run_cli(self, args):
        env = dict(os.environ, PYTHONPATH=self._SCRIPTS_DIR)
        return subprocess.run(
            [sys.executable, "-m", "gate.cold"] + args,
            capture_output=True, text=True, env=env)

    def _eval_sourceable(self, out):
        """Parse the `k=v` stdout lines the way gate.sh's eval would."""
        env = {}
        for line in out.stdout.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                env[k] = v
        return env

    def test_warm_store_emits_sourceable_triple(self):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            os.makedirs(cache)
            for i in range(cold.COLD_CACHE_THRESHOLD + 5):
                open(os.path.join(cache, "e%d" % i), "wb").close()
            r = self._run_cli(["evaluate", "--cache-root", cache,
                               "--identity-changed", "0", "--marker-changed", "0"])
            self.assertEqual(r.returncode, 0, r.stderr)
            env = self._eval_sourceable(r)
            self.assertEqual(env["cold"], "0")
            self.assertEqual(env["ceiling"], "900")

    def test_identity_changed_forces_cold(self):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            os.makedirs(cache)
            for i in range(cold.COLD_CACHE_THRESHOLD + 5):
                open(os.path.join(cache, "e%d" % i), "wb").close()
            r = self._run_cli(["evaluate", "--cache-root", cache,
                               "--identity-changed", "1", "--marker-changed", "0"])
            self.assertEqual(r.returncode, 0, r.stderr)
            env = self._eval_sourceable(r)
            self.assertEqual(env["cold"], "1")
            self.assertEqual(env["ceiling"], "1800")

    def test_marker_changed_forces_cold_even_warm_count(self):
        # The load-bearing #2100 case: a full-looking store under a STALE key.
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            os.makedirs(cache)
            for i in range(5000):
                open(os.path.join(cache, "e%d" % i), "wb").close()
            r = self._run_cli(["evaluate", "--cache-root", cache,
                               "--identity-changed", "0", "--marker-changed", "1"])
            env = self._eval_sourceable(r)
            self.assertEqual(env["cold"], "1")
            self.assertEqual(env["ceiling"], "1800")

    def test_cache_n_reports_find_l_count(self):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            os.makedirs(cache)
            for i in range(3):
                open(os.path.join(cache, "e%d" % i), "wb").close()
            r = self._run_cli(["evaluate", "--cache-root", cache,
                               "--identity-changed", "0", "--marker-changed", "0"])
            env = self._eval_sourceable(r)
            self.assertEqual(env["cache_n"], "3")

    def test_cache_n_excludes_size_sidecars(self):
        # #3690: the value gate.sh evals is the ENTRY count; the `<entry>.sz`
        # sidecars a no-fsync-mode store writes (#3669) are not entries.
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "c")
            os.makedirs(cache)
            for i in range(3):
                open(os.path.join(cache, "e%d" % i), "wb").close()
                open(os.path.join(cache, "e%d.sz" % i), "wb").close()
            r = self._run_cli(["evaluate", "--cache-root", cache,
                               "--identity-changed", "0", "--marker-changed", "0"])
            env = self._eval_sourceable(r)
            self.assertEqual(env["cache_n"], "3")

    def test_no_subcommand_exits_2(self):
        r = self._run_cli([])
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()
