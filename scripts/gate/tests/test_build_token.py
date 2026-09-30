"""Tests for scripts/build_token.sh, the per-job build token launcher, and the
daemon side of its protocol in scripts/build_jobserver.py.

Why the launcher exists: 2026-09-30 13:16, a lane brief's
`cmake --build build --target bezel_tests_sema -j` ran `make -j` (unlimited)
outside the jobserver; 499 concurrent clang, and the kernel OOM killer took
three Claude sessions. The launcher sits in every generated compile/link rule,
so the bound holds however the build was started.

Pin groups:
  1. Unjoined builds are bounded by the pool: a bare `make -j`, a `make -j`
     that inherited our MAKEFLAGS, and `ninja -j64` that inherited it (ninja
     keeps exporting MAKEFLAGS while ignoring it, probed on 1.13.1) never run
     more than N jobs; every token comes home. Jobs outlive several audits,
     so this also pins that the daemon does not re-mint launcher-held tokens
     (an idle-looking pool with only launchers in it is busy).
  2. A joined make is not double-charged: its jobs pass straight through, and
     the build reaches N + 1 (its implicit slot) instead of deadlocking.
  3. No launcher-aware daemon (none at all, or one without `features`): the
     machine-wide flock slots bound the build instead, and the FIFO's tokens
     are left alone.
  4. Exit codes pass through; BEZEL_JOBSERVER=off runs the job directly.
  5. Crash safety: a SIGKILLed launcher's token is restored by the audit; a
     job never inherits the FIFO; a waiter whose daemon dies falls back to
     the slots instead of hanging.
Everything runs in a TemporaryDirectory; nothing touches the production pool.
"""

import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(HERE, "..", "..")
DAEMON = os.path.join(SCRIPTS, "build_jobserver.py")
LAUNCHER = os.path.abspath(os.path.join(SCRIPTS, "build_token.sh"))
sys.path.insert(0, SCRIPTS)

import build_jobserver as bj  # noqa: E402

MAKE = shutil.which("make")
NINJA = shutil.which("ninja-build") or shutil.which("ninja")
FLOCK = shutil.which("flock")


def _wait_for(pred, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return pred()


def _slurp(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ""


def _peak(log):
    with open(log) as f:
        events = sorted((float(t), 1 if s == "+" else -1)
                        for s, t in (line.split() for line in f))
    running = peak = 0
    for _, delta in events:
        running += delta
        peak = max(peak, running)
    return peak, sum(1 for _, d in events if d == 1)


def _run(cmd, env, timeout, cwd=None):
    """Run a build in its own process group and kill the whole group on a
    timeout: a launcher left waiting on a token must not outlive the test."""
    p = subprocess.Popen(cmd, env=env, cwd=cwd, stdout=subprocess.DEVNULL,
                         start_new_session=True)
    try:
        return p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        raise


def _job(log, secs):
    """A shell job that logs its start and end around a sleep."""
    return (f"echo \"+ $$(date +%s.%N)\" >> {log}; sleep {secs}; "
            f"echo \"- $$(date +%s.%N)\" >> {log}")


@unittest.skipUnless(FLOCK and os.path.isdir("/proc"), "needs Linux /proc and flock")
class LauncherTestBase(unittest.TestCase):
    TOKENS = 2
    START_DAEMON = True

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="bezel-tok-test-")
        self.slots = os.path.join(self.dir, "slots")
        self.log = os.path.join(self.dir, "jobs.log")
        self.env = {k: v for k, v in os.environ.items()
                    if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "BEZEL_JOBSERVER",
                                 "CMAKE_BUILD_PARALLEL_LEVEL")}
        self.env.update(BEZEL_JOBSERVER_DIR=self.dir,
                        BEZEL_BUILD_TOKEN_SLOTS_DIR=self.slots,
                        BEZEL_BUILD_TOKEN_SLOTS="16",
                        BEZEL_BUILD_TOKEN_POLL="0.3")
        self.proc = None
        if self.START_DAEMON:
            self.proc = subprocess.Popen(
                [sys.executable, DAEMON, "serve", "--dir", self.dir,
                 "--tokens", str(self.TOKENS), "--interval", "0.2", "--confirm", "2"])
            self.assertTrue(_wait_for(lambda: os.path.exists(
                os.path.join(self.dir, "features")) and bj.alive(self.dir)),
                "daemon never came up")

    def tearDown(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        shutil.rmtree(self.dir, ignore_errors=True)

    def avail(self):
        fd = os.open(os.path.join(self.dir, "fifo"), os.O_RDONLY | os.O_NONBLOCK)
        try:
            return bj.available_tokens(fd)
        finally:
            os.close(fd)

    def makefile(self, jobs, secs):
        mk = os.path.join(self.dir, "Makefile")
        with open(mk, "w") as f:
            f.write("all: " + " ".join(f"t{i}" for i in range(jobs)) + "\n"
                    "t%:\n"
                    f"\t@{LAUNCHER} sh -c '{_job(self.log, secs)}'\n")
        return mk

    def ninjafile(self, jobs, secs):
        sub = os.path.join(self.dir, "n")
        os.makedirs(sub, exist_ok=True)
        with open(os.path.join(sub, "build.ninja"), "w") as f:
            f.write("rule s\n"
                    f"  command = {LAUNCHER} sh -c '{_job(self.log, secs)}; touch $out'\n")
            for i in range(jobs):
                f.write(f"build o{i}: s\n")
        return sub


@unittest.skipUnless(MAKE, "GNU make not installed")
class UnjoinedBuildIsBoundedTest(LauncherTestBase):
    def test_bare_make_j_takes_tokens(self):
        # The incident's shape: `make -j`, no MAKEFLAGS, no dev_heavy. Jobs span
        # several audits, so a daemon that re-minted launcher-held tokens (an
        # idle-looking pool with only launchers in it) would push the peak up.
        mk = self.makefile(8, 1.0)
        self.assertEqual(_run([MAKE, "-s", "-j", "-f", mk], self.env, 120, self.dir), 0)
        peak, started = _peak(self.log)
        self.assertEqual(started, 8)
        self.assertLessEqual(peak, self.TOKENS, "a bare make -j ran more jobs than tokens")
        self.assertGreaterEqual(peak, 2, "the pool never ran jobs in parallel")
        self.assertTrue(_wait_for(lambda: self.avail() == self.TOKENS),
                        "a launcher-held token never came home")

    def test_make_with_explicit_j_and_inherited_auth_takes_tokens(self):
        # An explicit -j makes make drop the inherited auth (it never opens the
        # FIFO), but the jobs may still see a MAKEFLAGS naming the pool.
        mk = self.makefile(6, 0.6)
        env = dict(self.env, MAKEFLAGS=f"-j --jobserver-auth=fifo:{self.dir}/fifo")
        self.assertEqual(_run([MAKE, "-s", "-j", "-f", mk], env, 120, self.dir), 0)
        peak, started = _peak(self.log)
        self.assertEqual(started, 6)
        self.assertLessEqual(peak, self.TOKENS)

    @unittest.skipUnless(NINJA, "ninja not installed")
    def test_ninja_j64_with_inherited_makeflags_takes_tokens(self):
        # ninja 1.13 with an explicit -j ignores the jobserver but still exports
        # the inherited MAKEFLAGS: MAKEFLAGS alone must not count as joined.
        sub = self.ninjafile(6, 0.6)
        env = dict(self.env, MAKEFLAGS=f"-j --jobserver-auth=fifo:{self.dir}/fifo")
        self.assertEqual(_run([NINJA, "-C", sub, "-j64"], env, 120), 0)
        peak, started = _peak(self.log)
        self.assertEqual(started, 6)
        self.assertLessEqual(peak, self.TOKENS)


@unittest.skipUnless(MAKE, "GNU make not installed")
class JoinedBuildIsNotDoubleChargedTest(LauncherTestBase):
    def test_joined_make_reaches_tokens_plus_implicit_slot(self):
        # If a joined make's jobs also took tokens, the make's two token-backed
        # jobs would wait on tokens the make itself holds: at best the build
        # runs below its share, at worst it never finishes.
        mk = self.makefile(9, 0.5)
        env = dict(self.env, MAKEFLAGS=f"-j --jobserver-auth=fifo:{self.dir}/fifo")
        self.assertEqual(_run([MAKE, "-s", "-f", mk], env, 60, self.dir), 0)
        peak, started = _peak(self.log)
        self.assertEqual(started, 9)
        self.assertEqual(peak, self.TOKENS + 1)
        self.assertTrue(_wait_for(lambda: self.avail() == self.TOKENS))

    @unittest.skipUnless(NINJA, "ninja not installed")
    def test_joined_ninja_passes_through(self):
        sub = self.ninjafile(9, 0.5)
        env = dict(self.env, MAKEFLAGS=f"-j --jobserver-auth=fifo:{self.dir}/fifo")
        self.assertEqual(_run([NINJA, "-C", sub], env, 60), 0)
        peak, started = _peak(self.log)
        self.assertEqual(started, 9)
        self.assertGreater(peak, self.TOKENS, "a joined ninja's jobs were charged twice")
        self.assertLessEqual(peak, self.TOKENS + 1)


@unittest.skipUnless(MAKE, "GNU make not installed")
class NoDaemonUsesSlotsTest(LauncherTestBase):
    START_DAEMON = False

    def test_no_daemon_bounds_make_j_by_slots(self):
        mk = self.makefile(6, 0.5)
        env = dict(self.env, BEZEL_BUILD_TOKEN_SLOTS="2")
        self.assertEqual(_run([MAKE, "-s", "-j", "-f", mk], env, 60, self.dir), 0)
        peak, started = _peak(self.log)
        self.assertEqual(started, 6)
        self.assertLessEqual(peak, 2)
        self.assertGreaterEqual(peak, 2)


@unittest.skipUnless(MAKE, "GNU make not installed")
class OldDaemonUsesSlotsTest(LauncherTestBase):
    TOKENS = 3

    def test_daemon_without_features_is_not_used(self):
        # An older daemon (no `features`) would read launcher-held tokens as a
        # leak and re-mint them, so the launcher must not draw on its FIFO.
        os.unlink(os.path.join(self.dir, "features"))
        mk = self.makefile(4, 0.5)
        env = dict(self.env, BEZEL_BUILD_TOKEN_SLOTS="1")
        seen = []
        p = subprocess.Popen([MAKE, "-s", "-j", "-f", mk], env=env, cwd=self.dir,
                             start_new_session=True)
        try:
            end = time.time() + 60
            while p.poll() is None and time.time() < end:
                seen.append(self.avail())
                time.sleep(0.05)
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        self.assertEqual(p.returncode, 0)
        peak, started = _peak(self.log)
        self.assertEqual(started, 4)
        self.assertEqual(peak, 1, "the slots did not bound the build")
        self.assertEqual(min(seen), self.TOKENS, "the launcher took an old daemon's token")


class DirectRunTest(LauncherTestBase):
    def test_exit_code_passes_through_with_a_token(self):
        self.assertEqual(_run([LAUNCHER, "sh", "-c", "exit 7"], self.env, 30), 7)
        self.assertTrue(_wait_for(lambda: self.avail() == self.TOKENS))

    def test_exit_code_passes_through_with_a_slot(self):
        env = dict(self.env, BEZEL_JOBSERVER_DIR=os.path.join(self.dir, "none"))
        self.assertEqual(_run([LAUNCHER, "sh", "-c", "exit 5"], env, 30), 5)
        self.assertTrue(os.path.isdir(self.slots))

    def test_jobserver_off_runs_directly(self):
        env = dict(self.env, BEZEL_JOBSERVER="off",
                   BEZEL_BUILD_TOKEN_SLOTS_DIR=os.path.join(self.dir, "unused"))
        self.assertEqual(_run([LAUNCHER, "sh", "-c", "exit 4"], env, 30), 4)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "unused")))
        self.assertEqual(self.avail(), self.TOKENS)


class CrashSafetyTest(LauncherTestBase):
    def _launch(self, *cmd, env=None):
        return subprocess.Popen([LAUNCHER, *cmd], env=env or self.env,
                                start_new_session=True)

    def test_holder_comm_and_job_does_not_inherit_the_fifo(self):
        marker = os.path.join(self.dir, "job.pid")
        p = self._launch("sh", "-c", f"echo $$ > {marker}; exec sleep 30")
        try:
            self.assertTrue(_wait_for(lambda: _slurp(marker).strip()))
            job = int(_slurp(marker))
            with open(f"/proc/{p.pid}/comm") as f:
                self.assertEqual(f.read().strip(), "bzl-tok-held")
            fifo = os.stat(os.path.join(self.dir, "fifo"))
            for fd in os.listdir(f"/proc/{job}/fd"):
                try:
                    st = os.stat(f"/proc/{job}/fd/{fd}")
                except OSError:
                    continue
                self.assertFalse(st.st_ino == fifo.st_ino and st.st_dev == fifo.st_dev,
                                 "the job inherited the token FIFO")
            held, makes, launchers = bj.expected_outstanding(
                os.path.join(self.dir, "fifo"), os.getpid())
            self.assertEqual((held, launchers), (1, 1))
        finally:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait(timeout=10)

    def test_sigkilled_holder_token_is_restored(self):
        p = self._launch("sleep", "30")
        try:
            self.assertTrue(_wait_for(lambda: self.avail() == self.TOKENS - 1),
                            "the launcher never took a token")
        finally:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait(timeout=10)
        self.assertTrue(_wait_for(lambda: self.avail() == self.TOKENS),
                        "a SIGKILLed launcher's token was never restored")

    def test_waiter_falls_back_to_slots_when_the_daemon_dies(self):
        holders = [self._launch("sleep", "30") for _ in range(self.TOKENS)]
        done = os.path.join(self.dir, "waiter.done")
        w = None
        try:
            self.assertTrue(_wait_for(lambda: self.avail() == 0))
            w = self._launch("sh", "-c", f"touch {done}")
            time.sleep(0.8)
            self.assertFalse(os.path.exists(done), "the waiter ran without a token")
            self.proc.terminate()
            self.proc.wait(timeout=10)
            self.assertEqual(w.wait(timeout=20), 0)
            self.assertTrue(os.path.exists(done))
        finally:
            for h in holders + ([w] if w and w.poll() is None else []):
                os.killpg(h.pid, signal.SIGKILL)
                h.wait(timeout=10)


class DaemonFeaturesTest(LauncherTestBase):
    def test_features_names_the_daemon_and_goes_away_with_it(self):
        with open(os.path.join(self.dir, "features")) as f:
            self.assertEqual(f.read().split(), ["launcher1", str(self.proc.pid)])
        self.proc.terminate()
        self.proc.wait(timeout=10)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "features")))

    def test_samples_carry_the_launchers_column(self):
        samples = os.path.join(self.dir, "samples.tsv")
        self.assertTrue(_wait_for(lambda: _slurp(samples).count("\n") >= 3))
        with open(os.path.join(self.dir, "samples.tsv")) as f:
            header, row = f.readline(), f.readline()
        self.assertEqual(header, bj.SAMPLES_HEADER)
        self.assertEqual(len(row.rstrip("\n").split("\t")),
                         len(header.rstrip("\n").split("\t")))


if __name__ == "__main__":
    unittest.main()
