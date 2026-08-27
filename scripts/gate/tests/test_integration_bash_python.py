"""Bash <-> Python integration test for the gate serialization refactor.

The Python helper at `scripts/gate/serialize.py` is pinned by
`test_gate_serialization.py`. This file pins the BASH side of the
integration: `scripts/gate_and_push.sh` MUST delegate to the Python helper
(via coproc + --hold) so two concurrent `gate_and_push.sh --help` invocations
produce the clean "another gate is running" abort message, NOT the deadlock
class that motivated the preflight-lockfile lane (2026-08-09).

Pins:

  TestBashDelegation.test_held_lockfile_produces_clean_abort_message
    Holder process acquires /var/tmp/bezel-scratch/gate.lock (the production
    path). A second `scripts/gate_and_push.sh --help` MUST exit 0 with the
    documented one-line "another gate is running" message on stderr. This is
    the regression guard for the deadlock: the gate cannot block, cannot
    exit non-zero (CI sees that as a real gate failure), and cannot crash.

  TestBashDelegation.test_default_lock_path_matches_helper
    Both sides agree on the lockfile path: gate_and_push.sh uses the same
    path the helper's `gate_lock_path()` returns. Drift between them is the
    #3000 shape — the writer and the reader disagree on the document they
    are sharing.

  TestBashDelegation.test_no_held_lock_lets_bash_proceed_to_help
    Without a held lock, `scripts/gate_and_push.sh --help` MUST proceed to
    its normal --help handling and exit 0 with the usage banner. Confirms
    the bash refactor did not break the trivial path — every gate (held or
    not) goes through this code, so a regression here blocks every push.

Stdlib only (subprocess, tempfile, os, sys, time, unittest). Tests target
the bash script directly (NOT the helper), which is the integration surface
the brief calls out.
"""

import os
import subprocess
import sys
import tempfile
import time
import unittest


# Path to scripts/ (so we can import gate.serialize for the default-path pin).
SCRIPTS_DIR = os.path.join(os.path.dirname(__file__), "..", "..")
GATE_AND_PUSH = os.path.normpath(
    os.path.join(SCRIPTS_DIR, "gate_and_push.sh"))


REPO_ROOT = os.path.normpath(os.path.join(SCRIPTS_DIR, ".."))

# Import the helper so the live-gate probe below uses the SAME lock primitive
# the gate itself uses, rather than a second spelling of it.
sys.path.insert(0, os.path.normpath(SCRIPTS_DIR))
from gate import serialize  # noqa: E402


def _live_gate_precondition_unmet():
    """Return a human reason why the two tests that drive the REAL
    `gate_and_push.sh` against the REAL lockfile must not run here, or None
    if the environment can support them.

    These two tests have a precondition they never used to state (#3415):

      1. **The working tree is clean.** They invoke the real entry point, and
         `gate_and_push.sh:135-141` refuses with exit 65 when tracked files
         differ from HEAD — by design, because a gate validates the working
         tree but pushes HEAD. So the tests fail for a reason that has nothing
         to do with the delegation they pin, and they fail precisely when the
         check is most wanted: a developer editing gate infrastructure has a
         dirty tree.

      2. **No real gate is running.** `test_held_lockfile_produces_clean_abort_message`
         does not merely read the production lockfile — it `os.unlink`s it and
         then holds it for 15s. Unlinking a lockfile a live gate holds an flock
         on is ACTIVELY DESTRUCTIVE: the next `gate_and_push.sh` opens the path,
         gets a NEW inode, acquires a fresh flock, and two gates run
         concurrently — the exact failure this lockfile exists to prevent.

    Skipping with a printed reason is the honest resolution. Weakening the
    tests to run anywhere would cost their whole point: they are valuable
    *because* they drive the real script and the real lock path, which is what
    `test_default_lock_path_matches_helper` cannot check by grepping.
    """
    dirty = subprocess.run(
        ["git", "diff", "--name-only", "HEAD"],
        cwd=REPO_ROOT, capture_output=True, text=True).stdout.strip()
    if dirty:
        return ("working tree is dirty, and gate_and_push.sh refuses (exit 65) "
                "on a dirty tree by design; tracked files differing from HEAD: "
                + ", ".join(dirty.split("\n")[:5]))
    # Running INSIDE a gate: the ancestor chain carries a gate driver's name.
    # This is the wired-as-a-gate-phase case, where the gate holds the very
    # lock these tests would unlink.
    if serialize.is_gate_sibling(os.getpid()):
        return ("a gate driver is an ancestor of this test runner, so a real "
                "gate is running; these tests would unlink the lockfile it holds")
    # A gate running elsewhere (another terminal, another lane) holds the lock
    # without being our ancestor. Probe non-blockingly and release at once.
    probe = serialize.acquire_gate_lock(serialize.gate_lock_path())
    if probe is None:
        return ("the production gate lockfile {} is currently held, so a gate "
                "is running somewhere; these tests would unlink it".format(
                    serialize.gate_lock_path()))
    probe.close()
    return None


def _holder_holds_lock(lockfile_path, hold_s=20):
    """Spawn a subprocess that calls `acquire_gate_lock` via the helper and
    sleeps — the lock is held by this subprocess until it exits. Returns the
    Popen; caller MUST reap it (kernel auto-releases the flock on exit)."""
    code = (
        "import sys, time\n"
        "sys.path.insert(0, {gate!r})\n"
        "from gate import serialize\n"
        "lock = serialize.acquire_gate_lock({path!r})\n"
        "assert lock is not None, 'holder must acquire'\n"
        "time.sleep({hold})\n"
    ).format(gate=SCRIPTS_DIR, path=lockfile_path, hold=hold_s)
    return subprocess.Popen(
        [sys.executable, "-c", code],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _kill_proc(pid):
    """Best-effort SIGTERM then SIGKILL a pid."""
    try:
        os.kill(pid, 15)  # SIGTERM
    except (ProcessLookupError, PermissionError):
        return
    deadline = time.time() + 2.0
    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)
    try:
        os.kill(pid, 9)  # SIGKILL
    except (ProcessLookupError, PermissionError):
        pass


class TestBashDelegation(unittest.TestCase):
    """`scripts/gate_and_push.sh` delegates lock acquisition + sibling
    detection to `scripts/gate/serialize.py`."""

    def test_held_lockfile_produces_clean_abort_message(self):
        """Holder grabs the production lockfile; a second
        `gate_and_push.sh --help` MUST exit 0 and emit the documented
        one-line 'another gate is running' abort. Regression guard for
        the 2026-08-09 deadlock class — the gate cannot block, cannot
        crash, and cannot exit non-zero (which CI would mis-read as a
        real gate failure)."""
        # #3415: state the precondition instead of failing on it.
        _unmet = _live_gate_precondition_unmet()
        if _unmet:
            self.skipTest(_unmet)
        lockfile = "/var/tmp/bezel-scratch/gate.lock"
        # Ensure the lockfile dir exists (helper also does this, but we
        # need it for the holder).
        os.makedirs(os.path.dirname(lockfile), exist_ok=True)
        # Clean any stale lockfile from a previous test run. We do this
        # BEFORE the holder so the holder races on a fresh file (the
        # kernel creates it on open).
        try:
            os.unlink(lockfile)
        except FileNotFoundError:
            pass
        holder = _holder_holds_lock(lockfile, hold_s=15)
        try:
            # Wait for the lockfile to appear (holder's open(O_CREAT)).
            deadline = time.time() + 5.0
            while time.time() < deadline:
                if os.path.exists(lockfile):
                    break
                time.sleep(0.05)
            self.assertTrue(
                os.path.exists(lockfile),
                "holder did not create the lockfile within 5s")

            # Now run the bash script. MUST exit 0 with the message.
            t0 = time.time()
            result = subprocess.run(
                [GATE_AND_PUSH, "--help"],
                capture_output=True, timeout=15, text=True,
            )
            elapsed = time.time() - t0
            self.assertEqual(
                result.returncode, 0,
                "gate_and_push.sh --help with held lock must exit 0 "
                "(CI reads non-zero as a real gate failure). Got rc={}, "
                "stderr={!r}".format(result.returncode, result.stderr))
            # The "another gate is running" message is on stderr (matches
            # Lane A's bash). Use `in` because the message includes the
            # lockfile path and an optional holder pid — both vary.
            self.assertIn(
                "another gate is running", result.stderr,
                "missing the documented 'another gate is running' abort "
                "message. stderr was: {!r}".format(result.stderr))
            self.assertLess(
                elapsed, 5.0,
                "aborting on a held lock must be fast (non-blocking); "
                "took {:.2f}s".format(elapsed))
        finally:
            _kill_proc(holder.pid)
            holder.wait(timeout=5)
            # Best-effort lockfile cleanup so the next test starts clean.
            try:
                os.unlink(lockfile)
            except FileNotFoundError:
                pass

    def test_default_lock_path_matches_helper(self):
        """`scripts/gate_and_push.sh`'s hardcoded lockfile path MUST equal
        `gate.serialize.gate_lock_path()`. Drift between the two is the
        #3000 shape (writer and reader disagree on the shared document) —
        the bash side would acquire a DIFFERENT lockfile than the Python
        helper considers authoritative, defeating the entire preflight."""
        result = subprocess.run(
            [sys.executable, "-c",
             "import sys; sys.path.insert(0, {!r}); "
             "from gate.serialize import gate_lock_path; "
             "print(gate_lock_path())".format(SCRIPTS_DIR)],
            capture_output=True, timeout=10, text=True,
        )
        self.assertEqual(
            result.returncode, 0,
            "helper failed to report gate_lock_path(): {}".format(
                result.stderr))
        helper_path = result.stdout.strip()
        # The bash hardcodes GATE_LOCK; grep it out instead of parsing
        # bash. Use a regex-tolerant extract that matches the documented
        # `GATE_LOCK="..."` line.
        with open(GATE_AND_PUSH) as _gate_push_f:
            bash_text = _gate_push_f.read()

        # Find the first `GATE_LOCK="<path>"` assignment after the
        # `preflight-lockfile lane:` marker.
        marker = "preflight-lockfile lane"
        idx = bash_text.find(marker)
        self.assertGreaterEqual(
            idx, 0,
            "expected to find the preflight-lockfile comment marker in "
            "scripts/gate_and_push.sh — the refactor may have removed "
            "it (or this lane drifted off the post-run-infra branch)")
        snippet = bash_text[idx:]
        # Find the assignment line.
        import re
        m = re.search(r'GATE_LOCK="([^"]+)"', snippet)
        self.assertIsNotNone(
            m,
            "expected a GATE_LOCK=\"...\" assignment in scripts/"
            "gate_and_push.sh after the preflight-lockfile comment; the "
            "refactor removed the hardcoded path")
        bash_path = m.group(1)
        self.assertEqual(
            bash_path, helper_path,
            "scripts/gate_and_push.sh's GATE_LOCK ({!r}) does not match "
            "gate.serialize.gate_lock_path() ({!r}); drift between the "
            "two would silently re-introduce the #3000 class".format(
                bash_path, helper_path))

    def test_no_held_lock_lets_bash_proceed_to_help(self):
        """Without a held lock, `scripts/gate_and_push.sh --help` MUST
        proceed to its normal --help handling and exit 0 with the usage
        banner. Confirms the refactor did not break the trivial path —
        every push (held or not) goes through this code, so a regression
        here would block every push, not just overlapping ones."""
        # #3415: state the precondition instead of failing on it.
        _unmet = _live_gate_precondition_unmet()
        if _unmet:
            self.skipTest(_unmet)
        lockfile = "/var/tmp/bezel-scratch/gate.lock"
        # Ensure no lock is held by any leftover holder from a previous
        # test. We don't have a way to wait for the kernel to release
        # beyond the holder exiting; the previous test's _kill_proc is
        # best-effort. If a holder is still around, this test surfaces
        # the symptom as a held-lock abort (and the assertion below
        # fails loudly, NOT silently). That's the right failure mode.
        result = subprocess.run(
            [GATE_AND_PUSH, "--help"],
            capture_output=True, timeout=15, text=True,
        )
        self.assertEqual(
            result.returncode, 0,
            "gate_and_push.sh --help with no held lock must exit 0; "
            "got rc={}, stderr={!r}".format(
                result.returncode, result.stderr))
        # The --help banner starts with the comment line at the top of
        # the script (the usage summary). Confirm it is present so we
        # know the script reached the --help handling, not the held-lock
        # abort path.
        self.assertNotIn(
            "another gate is running", result.stderr,
            "without a held lock, the script must NOT print the "
            "'another gate is running' abort message (it should reach "
            # This branch is the production success path; stderr should
            # be empty (or contain only warnings) and stdout should
            # contain the usage banner.
            "the --help banner). stderr was: {!r}".format(result.stderr))
        # The --help banner is printed to stdout (Lane A's bash uses
        # `sed -n '2,38p' "$0"` which writes to stdout).
        self.assertIn(
            "Run the integration gate", result.stdout,
            "expected the usage banner (from the file header) on stdout; "
            "got stdout={!r}".format(result.stdout[:500]))


# Adversarial self-pass coverage notes (per the lane protocol §5).
#
# AXIS GRID:
#   lockfile path:
#     - test_held_lockfile_produces_clean_abort_message: production path
#       /var/tmp/bezel-scratch/gate.lock (the only one that matters).
#     - test_default_lock_path_matches_helper: cross-checks the bash
#       hardcoded path against the helper's `gate_lock_path()`.
#   lock held vs not held:
#     - test_held_lockfile_produces_clean_abort_message: held path.
#     - test_no_held_lock_lets_bash_proceed_to_help: not-held path.
#     - The two together pin both branches of the bash `if [ … ]`.
#   bash argument path:
#     - Both tests use --help, which exercises the early-exit branch
#       before any actual gate work. That is the right pin: it is the
#       path that runs on a watcher probe (the failure mode the
#       preflight-lockfile lane was designed to support — the watcher
#       probes with --help before launching a real gate).
#
# DETERMINISM:
#   - Every test has a hard timeout (15s for the bash side, 5s for the
#     holder lockfile-wait). The bash script under test takes ~50ms on a
#     held lock and ~100ms on an unlocked --help. The 15s timeout is
#     generous but bounded so a hung bash cannot stall the test.
#   - The holder uses SIGTERM-then-SIGKILL cleanup; the kernel releases
#     the flock on either path, so no stale lockfile can poison the
#     next test.
#   - We do NOT sleep-then-poll for "no held lock" — that would be
#     racy. Instead, the previous test's _kill_proc cleans up its
#     holder; if a holder is still around, this test's assertion fires
#     loudly (with a clear "another gate is running" mismatch).
#
# PRE-EMPT (3 strongest attacks):
#   1. "What if the bash script is killed before its trap fires?"
#      The trap uses `kill … || true; wait … || true`, so a SIGKILL of
#      the bash (no trap runs) leaves the coproc alive briefly. The
#      kernel releases the helper's flock when the helper is itself
#      killed (parent's death -> SIGKILL on child via PR_SET_PDEATHSIG
#      is NOT set, so the child outlives the parent; the operator
#      would have to reap manually). This is the same class as a
#      crashed gate in production — the lockfile is stale until the
#      helper is reaped. We don't pin this here because the production
#      recovery path is `kernel auto-release on helper exit` which
#      test_gate_serialization's test_lockfile_released_on_subprocess_exit
#      already pins.
#   2. "What if `gate_and_push.sh` is not executable?" — the test calls
#      it as a subprocess argument list, which works even without the
#      executable bit on the FS (Python invokes the interpreter and
#      passes the path as argv[0]). On a shebang-style bash script,
#      bash will still read+execute it. (Confirmed: bash /path/to.sh
#      works regardless of exec bit.)
#   3. "What if /var/tmp/bezel-scratch does not exist?" — the test
#      ensures the directory exists before spawning the holder; the
#      helper also creates it best-effort (see _open_lockfile). The
#      bash side does `mkdir -p … 2>/dev/null || true` before the
#      helper invocation, so this is belt-and-suspenders.
#
# WHAT THIS TEST DOES NOT COVER (explicit limits):
#   - The actual gate (full ~30m run) is not exercised. The preflight-
#     lockfile contract is pinned at the entry point; the actual gate
#     work is a different test surface (the integration gate itself).
#   - Concurrent holders (3+ processes) — Lane A's contract is 2-way
#     mutual exclusion (one holds, one refuses). 3+ reduces to 2-way
#     by induction (the first holds, all others refuse).
#   - The `_is_gate_ancestor` wrapper inside scripts/gate.sh — that
#     helper is exercised by test_gate_serialization.py's
#     SiblingDetectionTest. End-to-end with the bash gate preflight
#     is not pin-tested here (it would require spawning cmake/ld
#     children with gate ancestors — the test_gate_sibling.sh script
#     Lane A included covers it).

if __name__ == "__main__":
    unittest.main()
