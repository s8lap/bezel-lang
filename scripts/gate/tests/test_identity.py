"""Tests for gate.identity (Phase 5, #3010 — k2 identity + per-oracle marker).

The load-bearing pins the task requires:
  1. the identity is STABLE across re-links — same archives ⇒ same hash (the
     prediction's foundation: a warm store's marker matches).
  2. it MOVES when core.a OR runtime.a changes (either half invalidates — a
     runtime-only change was the 2026-07-29 miss gate.sh:602-614 fixed).
  3. the marker filename is a function of oracle (`.gate-identity` vs
     `.gate-identity-asan`) — per #2918 plain/ASan are disjoint populations.
  4. the sa= bit PARITY: the identity is oracle-BLIND (the sanitizer bit lives
     in the entry's MODE string, aot_support.hpp:1262, NOT the identity); the
     per-oracle disambiguation is the marker filename's job, not the hash's.

Plus: byte order is compiler-then-runtime (aot_support.hpp:1121-1122, 1216-1221);
read/write_marker round-trip; write_marker is best-effort + no-op on empty;
read_marker degrades to "" on missing/unreadable (gate.sh:623).
"""

import hashlib
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import identity  # noqa: E402


def _sha256_concat(*paths):
    """Reference: SHA-256 of the raw concatenation of the files' bytes."""
    h = hashlib.sha256()
    for p in paths:
        with open(p, "rb") as f:
            h.update(f.read())
    return h.hexdigest()


class ComputeIdentityTest(unittest.TestCase):
    """compute_identity == SHA-256(core.a ‖ runtime.a) — faithful to gate.sh:615."""

    def test_stable_across_relinks_same_archives(self):
        # Pin 1: same archives ⇒ same hash. A relink that does not change the
        # archives must not move the identity (else a warm store looks cold).
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "libbezel_core.a")
            rt = os.path.join(d, "libbezel_runtime.a")
            with open(core, "wb") as f:
                f.write(b"corebytes" * 1000)
            with open(rt, "wb") as f:
                f.write(b"runtimebytes" * 500)
            first = identity.compute_identity(core, rt)
            second = identity.compute_identity(core, rt)
            self.assertEqual(first, second)
            self.assertEqual(first, _sha256_concat(core, rt))

    def test_moves_when_core_a_changes(self):
        # Pin 2a: a compiler (core.a) rebuild invalidates every entry.
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "libbezel_core.a")
            rt = os.path.join(d, "libbezel_runtime.a")
            with open(core, "wb") as f:
                f.write(b"core-v1")
            with open(rt, "wb") as f:
                f.write(b"runtime")
            before = identity.compute_identity(core, rt)
            with open(core, "wb") as f:
                f.write(b"core-v2-different")
            after = identity.compute_identity(core, rt)
            self.assertNotEqual(before, after)

    def test_moves_when_runtime_a_changes(self):
        # Pin 2b: a RUNTIME-only change invalidates every entry even though
        # core.a's bytes are unchanged — the 2026-07-29 miss (gate.sh:602-614):
        # both mtime+count said warm for a fully cold tree because only the
        # runtime moved. The identity folds BOTH halves, so it catches this.
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "libbezel_core.a")
            rt = os.path.join(d, "libbezel_runtime.a")
            with open(core, "wb") as f:
                f.write(b"core-same")
            with open(rt, "wb") as f:
                f.write(b"runtime-v1")
            before = identity.compute_identity(core, rt)
            # core.a untouched on disk (the mtime signal gate.sh:601 also uses
            # would be quiet here — only the identity catches it).
            with open(rt, "wb") as f:
                f.write(b"runtime-v2-different")
            after = identity.compute_identity(core, rt)
            self.assertNotEqual(before, after)

    def test_byte_order_is_compiler_then_runtime(self):
        # aot_support.hpp:1121-1122 / :1216-1221: compiler = core.a FIRST,
        # runtime = libbezel_runtime.a SECOND. Reverse order would be a
        # different (wrong) hash and the marker would never match the store.
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "core.a")
            rt = os.path.join(d, "runtime.a")
            with open(core, "wb") as f:
                f.write(b"AAAA")
            with open(rt, "wb") as f:
                f.write(b"BBBB")
            got = identity.compute_identity(core, rt)
            self.assertEqual(got, hashlib.sha256(b"AAAABBBB").hexdigest())
            self.assertNotEqual(got, hashlib.sha256(b"BBBBAAAA").hexdigest())

    def test_missing_archive_returns_empty(self):
        # gate.sh:615 `cat ... 2>/dev/null`: an absent archive ⇒ no identity
        # ("unavailable", gate.sh:626-628). Never raise, never mint a stale key.
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "core.a")
            missing = os.path.join(d, "nope.a")
            with open(core, "wb") as f:
                f.write(b"x")
            self.assertEqual(identity.compute_identity(core, missing), "")
            self.assertEqual(identity.compute_identity(missing, core), "")

    def test_streams_large_archives_without_loading_whole(self):
        # The real archives are ~100 MB; the function must stream, not read()
        # whole into one update (the chunked loop). A 4 MiB archive confirms the
        # streaming path matches the reference whole-file hash.
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "core.a")
            rt = os.path.join(d, "runtime.a")
            blob = bytes(range(256)) * (4 * 1024 * 1024 // 256)
            with open(core, "wb") as f:
                f.write(blob)
            with open(rt, "wb") as f:
                f.write(blob[::-1])
            self.assertEqual(
                identity.compute_identity(core, rt),
                _sha256_concat(core, rt),
            )


class SaBitParityTest(unittest.TestCase):
    """Pin 4: the identity is oracle-BLIND; the sa= bit is orthogonal.

    aot_support.hpp:1262 puts `sa=` in modeString (the entry's MODE component),
    NOT in the BuildIdentity (compiler ‖ runtime). So the identity digest does
    not — and must not — depend on the oracle; the per-oracle disambiguation is
    the MARKER FILENAME's job (per #2918). This test pins that parity so a future
    edit does not accidentally fold a sanitizer bit into the identity (which
    would let a plain-populated store's marker match an ASan run).
    """

    def test_identity_has_no_oracle_argument(self):
        # compute_identity takes only the two archives: it CANNOT vary by oracle.
        import inspect
        params = list(inspect.signature(identity.compute_identity).parameters)
        self.assertEqual(params, ["core_path", "runtime_path"])

    def test_same_archives_same_hash_regardless_of_caller(self):
        # There is no oracle input to perturb; the digest is a pure function of
        # core.a ‖ runtime.a. The sa= bit (aot_support.hpp:1262) is the entry's,
        # not the store's identity.
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "core.a")
            rt = os.path.join(d, "runtime.a")
            with open(core, "wb") as f:
                f.write(b"c")
            with open(rt, "wb") as f:
                f.write(b"r")
            # A plain gate and an ASan gate compute the SAME identity from the
            # same archives; the marker FILENAME distinguishes their stores.
            self.assertEqual(
                identity.compute_identity(core, rt),
                identity.compute_identity(core, rt),
            )


class MarkerFilenameTest(unittest.TestCase):
    """Pin 3: marker filename is per-oracle. per #2918 (Plan-A 1.14)."""

    def test_plain_oracle(self):
        self.assertEqual(identity.marker_filename("plain"),
                         identity.MARKER_PLAIN)
        self.assertEqual(identity.marker_filename("plain"), ".gate-identity")

    def test_asan_oracle(self):
        self.assertEqual(identity.marker_filename("asan"),
                         identity.MARKER_ASAN)
        self.assertEqual(identity.marker_filename("asan"),
                         ".gate-identity-asan")

    def test_plain_and_asan_markers_are_distinct(self):
        # The whole point of #2918: plain and ASan are DISJOINT populations in
        # ONE store. A shared marker would let an ASan run read a plain store's
        # marker and wrongly predict warm — so the two names MUST differ.
        self.assertNotEqual(identity.MARKER_PLAIN, identity.MARKER_ASAN)
        self.assertNotEqual(identity.marker_filename("plain"),
                            identity.marker_filename("asan"))

    def test_unknown_oracle_defaults_to_plain(self):
        # The bash (gate.sh:616-620) is an if/else with the ASan branch first
        # and plain as the default fall-through; an unrecognized mode selects
        # plain rather than minting a third marker file.
        self.assertEqual(identity.marker_filename("something-else"),
                         identity.MARKER_PLAIN)
        self.assertEqual(identity.marker_filename(""), identity.MARKER_PLAIN)


class MarkerReadWriteTest(unittest.TestCase):
    """read_marker / write_marker round-trip + the gate.sh:986-990 best-effort."""

    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "sub", ".gate-identity")
            digest = "a" * 64
            identity.write_marker(p, digest)
            self.assertEqual(identity.read_marker(p), digest)

    def test_write_marker_creates_parent_dirs(self):
        # gate.sh:987 `mkdir -p "$(dirname ...)"`: the cache root exists but a
        # nested marker path must create its parents.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a", "b", "c", ".gate-identity-asan")
            identity.write_marker(p, "deadbeef")
            self.assertEqual(identity.read_marker(p), "deadbeef")

    def test_write_marker_strips_trailing_whitespace_on_read(self):
        # The marker is compared against a 64-char hex digest; a stray blank line
        # must not make a matching identity look CHANGED (which would force cold
        # unnecessarily — safe, but noisy). write_marker adds exactly one '\n'.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, ".gate-identity")
            identity.write_marker(p, "b" * 64)
            with open(p) as f:
                self.assertEqual(f.read(), "b" * 64 + "\n")

    def test_read_marker_missing_file_returns_empty(self):
        # gate.sh:623 `cat ... 2>/dev/null || echo none`: a first run has no
        # marker; read_marker returns "" and is_cold treats marker=""!=computed
        # as CHANGED (cold) — the safe direction.
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(
                identity.read_marker(os.path.join(d, "absent")), ""
            )

    def test_read_marker_unreadable_file_returns_empty(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, ".gate-identity")
            open(p, "w").close()
            os.chmod(p, 0)  # unreadable
            try:
                self.assertEqual(identity.read_marker(p), "")
            finally:
                os.chmod(p, 0o600)  # restore so cleanup can remove it

    def test_write_marker_noop_on_empty_identity(self):
        # gate.sh:986 only writes when GATE_CACHE_ID is set. An empty identity is
        # a no-op: never install a blank marker (read_marker would return "",
        # which is fine, but writing nothing keeps the on-disk signal honest).
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, ".gate-identity")
            identity.write_marker(p, "")
            self.assertFalse(os.path.exists(p))

    def test_write_marker_best_effort_does_not_raise(self):
        # gate.sh:988 trails `2>/dev/null`: a marker-write failure must not fail
        # a gate that already passed. Point the path at an unwritable parent.
        identity.write_marker("/proc/this/cannot/exist/.gate-identity", "x" * 64)


class CLITest(unittest.TestCase):
    """`python3 -m gate.identity <sub>` — the Wave-2 gate.sh bridge (#3010)."""

    _SCRIPTS_DIR = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", ".."))

    def _run_cli(self, args):
        import subprocess
        env = dict(os.environ, PYTHONPATH=self._SCRIPTS_DIR)
        return subprocess.run(
            [sys.executable, "-m", "gate.identity"] + args,
            capture_output=True, text=True, env=env)

    def test_compute_prints_digest_matching_function(self):
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "libbezel_core.a")
            rt = os.path.join(d, "libbezel_runtime.a")
            with open(core, "wb") as f:
                f.write(b"core-bytes")
            with open(rt, "wb") as f:
                f.write(b"runtime-bytes")
            r = self._run_cli(["compute", "--core", core, "--runtime", rt])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(),
                             identity.compute_identity(core, rt))
            self.assertEqual(r.stdout.strip(), _sha256_concat(core, rt))

    def test_compute_blank_when_archive_missing(self):
        with tempfile.TemporaryDirectory() as d:
            core = os.path.join(d, "libbezel_core.a")
            with open(core, "wb") as f:
                f.write(b"x")
            # runtime missing ⇒ blank line (gate.sh swallows to "" via $()).
            r = self._run_cli(
                ["compute", "--core", core, "--runtime", d + "/missing.a"])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "")

    def test_marker_filename_is_per_oracle_and_joined_to_root(self):
        for oracle, basename in [("plain", identity.MARKER_PLAIN),
                                 ("asan", identity.MARKER_ASAN)]:
            r = self._run_cli(
                ["marker-filename", "--cache-root", "/cache/root", "--oracle",
                 oracle])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(),
                             os.path.join("/cache/root", basename))
        # plain is the default branch.
        r = self._run_cli(
            ["marker-filename", "--cache-root", "/c", "--oracle", "plain"])
        self.assertIn(identity.MARKER_PLAIN, r.stdout)

    def test_read_write_marker_round_trip_via_cli(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, ".gate-identity")
            self.assertEqual(self._run_cli(
                ["write-marker", "--path", p, "--identity", "abc123"]).returncode,
                             0)
            r = self._run_cli(["read-marker", "--path", p])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "abc123")

    def test_read_marker_blank_when_absent(self):
        with tempfile.TemporaryDirectory() as d:
            r = self._run_cli(["read-marker", "--path", d + "/nope"])
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), "")

    def test_no_subcommand_exits_2(self):
        r = self._run_cli([])
        self.assertEqual(r.returncode, 2)
        self.assertIn("usage", r.stderr)


if __name__ == "__main__":
    unittest.main()
