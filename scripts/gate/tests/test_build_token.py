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
  6. Test runs take a token (scripts/test_token.sh, spawned by
     tests/test_main.cpp). MEASURED 2026-11-04 ~19:30: the pool read
     `available=20 held=0` while ~20 bezel_tests_* processes ran. A fake test
     process speaks the same descriptor protocol test_main.cpp does: one token
     is held while it runs and comes home when it exits (also on SIGKILL of
     the test); the daemon never re-mints it; a nested run does not take a
     second one; BEZEL_JOBSERVER=off bypasses; a SIGKILLed holder's token is
     restored by the audit; `status` counts the test and its token. When a
     hooked bezel_tests_support is built, the same is checked on the real
     binary, plus the enumeration bypass.
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


TEST_TOKEN = os.path.abspath(os.path.join(SCRIPTS, "test_token.sh"))

# A stand-in for a bezel_tests_<area> process: the descriptor protocol of
# tests/test_main.cpp's holdTestToken, verbatim. Prints `held` / `none` to the
# marker once the holder has answered, then sleeps argv[2] seconds. With
# argv[3] it starts a NESTED fake test (with this process's environment, as a
# test binary started by a test would) and waits for it.
FAKE_TEST = r"""
import os, sys, time
marker, secs = sys.argv[1], float(sys.argv[2])
with open("/proc/self/comm", "w") as f:
    f.write("bezel_tests_fak")
def high_pipe():
    r, w = os.pipe2(os.O_CLOEXEC)
    out = [fcntl.fcntl(x, fcntl.F_DUPFD_CLOEXEC, 10) for x in (r, w)]
    os.close(r); os.close(w)
    return out
import fcntl
life_r, life_w = high_pipe()
ready_r, ready_w = high_pipe()
if os.environ.get("BEZEL_JOBSERVER") == "off" or os.environ.get("BEZEL_TEST_TOKEN_HELD"):
    got = b""
else:
    os.posix_spawnp("bash", ["bash", os.environ["FAKE_TEST_TOKEN_SCRIPT"]], os.environ,
                    file_actions=[(os.POSIX_SPAWN_DUP2, life_r, 0),
                                  (os.POSIX_SPAWN_DUP2, ready_w, 3),
                                  (os.POSIX_SPAWN_OPEN, 1, "/dev/null", os.O_WRONLY, 0)])
    os.close(life_r); os.close(ready_w)
    got = os.read(ready_r, 1)
    if got:
        os.environ["BEZEL_TEST_TOKEN_HELD"] = "1"
with open(marker, "w") as f:
    f.write("held" if got else "none")
if len(sys.argv) > 3:
    import subprocess
    subprocess.run([sys.executable, __file__, sys.argv[3], str(secs)], check=True)
else:
    time.sleep(secs)
"""


class TestRunTakesATokenTest(LauncherTestBase):
    """Group 6: the test-run token (scripts/test_token.sh)."""

    def setUp(self):
        super().setUp()
        self.fake = os.path.join(self.dir, "fake_test.py")
        with open(self.fake, "w") as f:
            f.write(FAKE_TEST)
        self.env["FAKE_TEST_TOKEN_SCRIPT"] = TEST_TOKEN
        self.env.pop("BEZEL_TEST_TOKEN_HELD", None)
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait(timeout=10)
        super().tearDown()

    def fake_test(self, name, secs, env=None, nested=None):
        marker = os.path.join(self.dir, name)
        argv = [sys.executable, self.fake, marker, str(secs)]
        if nested:
            argv.append(os.path.join(self.dir, nested))
        p = subprocess.Popen(argv, env=env or self.env, start_new_session=True)
        self.procs.append(p)
        return p, marker

    def children(self, pid):
        try:
            with open(f"/proc/{pid}/task/{pid}/children") as f:
                return [int(c) for c in f.read().split()]
        except OSError:
            return []

    def holder_of(self, pid):
        for c in self.children(pid):
            if _slurp(f"/proc/{c}/comm").strip().startswith("bzl-tok-"):
                return c
        return None

    def script(self, env, stdin, ready):
        """test_token.sh with `stdin` on fd 0 and `ready` on fd 3, the way
        test_main.cpp spawns it."""
        return subprocess.Popen(
            ["bash", "-c", f'exec bash "$0" 3>&{ready} {ready}>&-', TEST_TOKEN],
            env=env, stdin=stdin, pass_fds=(ready,), start_new_session=True)

    def census(self, pid):
        """(is `pid` counted as a test, tests holding a token of THIS pool,
        tests queued for one). `tests` itself is box-wide, so other lanes'
        test runs are in it; the holding/waiting counts are per pool."""
        _, holding, waiting = bj.test_census(os.path.join(self.dir, "fifo"), os.getpid())
        return pid in bj.test_pids(), holding, waiting

    def test_one_token_held_while_the_test_runs_and_returned_on_exit(self):
        # Runs across ~15 audits (interval 0.2 s, confirm 2): a daemon that did
        # not count the test's holder would re-mint its token (avail back to N).
        p, marker = self.fake_test("t1", 3.0)
        self.assertTrue(_wait_for(lambda: _slurp(marker) == "held"), "no token taken")
        seen = []
        while p.poll() is None:
            seen.append(self.avail())
            time.sleep(0.1)
            if len(seen) == 10:
                holder = self.holder_of(p.pid)
                self.assertIsNotNone(holder, "no holder child under the test")
                self.assertEqual(_slurp(f"/proc/{holder}/comm").strip(), "bzl-tok-held")
                self.assertEqual(self.census(p.pid), (True, 1, 0))
        self.assertEqual(p.wait(), 0)
        self.assertGreaterEqual(len(seen), 15)
        self.assertEqual(set(seen[:-2]), {self.TOKENS - 1},
                         "the test's token was re-minted or never taken")
        self.assertTrue(_wait_for(lambda: self.avail() == self.TOKENS),
                        "the test's token never came home")
        repaired = [line.split("\t")[-1].strip()
                    for line in _slurp(os.path.join(self.dir, "samples.tsv")).splitlines()
                    if not line.startswith("epoch")]
        self.assertTrue(repaired and set(repaired) == {"0"},
                        f"the audit repaired the pool while a test held a token: {repaired}")

    def test_a_sigkilled_test_returns_its_token(self):
        # The holder's stdin is a pipe only the test writes: SIGKILL of the
        # test is EOF for the holder, which then hands the token back itself.
        p, marker = self.fake_test("t2", 60)
        self.assertTrue(_wait_for(lambda: _slurp(marker) == "held"))
        holder = self.holder_of(p.pid)
        self.assertIsNotNone(holder)
        os.kill(p.pid, signal.SIGKILL)
        p.wait(timeout=10)
        self.assertTrue(_wait_for(lambda: self.avail() == self.TOKENS, timeout=5),
                        "a SIGKILLed test's token never came home")
        self.assertTrue(_wait_for(lambda: not os.path.exists(f"/proc/{holder}"), timeout=5),
                        "the holder outlived its test")

    def test_a_nested_test_does_not_take_a_second_token(self):
        # With a pool of 1, a nested run that queued for a second token while
        # its parent held the only one would deadlock.
        drain = os.open(os.path.join(self.dir, "fifo"), os.O_RDWR | os.O_NONBLOCK)
        try:
            os.read(drain, self.TOKENS - 1)  # leave exactly one token
            p, marker = self.fake_test("outer", 0.5, nested="inner")
            self.assertEqual(p.wait(timeout=30), 0, "the nested run deadlocked")
            self.assertEqual(_slurp(marker), "held")
            self.assertEqual(_slurp(os.path.join(self.dir, "inner")), "none")
        finally:
            os.write(drain, b"+" * (self.TOKENS - 1))
            os.close(drain)
        self.assertTrue(_wait_for(lambda: self.avail() == self.TOKENS))

    def test_script_honours_token_held_by_itself(self):
        # test_token.sh's own guard (test_main.cpp checks it first, too).
        r, w = os.pipe()
        lr, lw = os.pipe()
        env = dict(self.env, BEZEL_TEST_TOKEN_HELD="1")
        p = self.script(env, lr, w)
        os.close(w); os.close(lr)
        self.assertEqual(os.read(r, 1), b"T")
        self.assertEqual(p.wait(timeout=10), 0)
        self.assertEqual(self.avail(), self.TOKENS)
        os.close(r); os.close(lw)

    def test_jobserver_off_takes_nothing(self):
        env = dict(self.env, BEZEL_JOBSERVER="off",
                   BEZEL_BUILD_TOKEN_SLOTS_DIR=os.path.join(self.dir, "unused"))
        p, marker = self.fake_test("t3", 1.0, env=env)
        self.assertTrue(_wait_for(lambda: _slurp(marker) == "none"))
        self.assertEqual(self.avail(), self.TOKENS)
        self.assertIsNone(self.holder_of(p.pid))
        self.assertEqual(self.census(p.pid), (True, 0, 0))
        self.assertEqual(p.wait(timeout=10), 0)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "unused")))

    def test_jobserver_off_through_the_script_itself(self):
        # build_token.sh case 1: the script runs straight through, no token.
        r, w = os.pipe()
        lr, lw = os.pipe()
        env = dict(self.env, BEZEL_JOBSERVER="off")
        p = self.script(env, lr, w)
        os.close(w); os.close(lr)
        self.assertEqual(os.read(r, 1), b"T")
        self.assertEqual(self.avail(), self.TOKENS)
        os.close(lw)  # the "test" exits
        self.assertEqual(p.wait(timeout=10), 0)
        os.close(r)

    def test_a_sigkilled_holder_token_is_restored_by_the_audit(self):
        p, marker = self.fake_test("t4", 60)
        self.assertTrue(_wait_for(lambda: _slurp(marker) == "held"))
        holder = self.holder_of(p.pid)
        self.assertIsNotNone(holder)
        os.kill(holder, signal.SIGKILL)
        self.assertTrue(_wait_for(lambda: self.avail() == self.TOKENS, timeout=10),
                        "a SIGKILLed holder's token was never restored")
        self.assertEqual(p.poll(), None, "killing the holder killed the test")

    def test_a_test_queues_when_the_pool_is_drained(self):
        holders = [subprocess.Popen([LAUNCHER, "sleep", "30"], env=self.env,
                                    stderr=subprocess.DEVNULL, start_new_session=True)
                   for _ in range(self.TOKENS)]
        self.procs.extend(holders)
        self.assertTrue(_wait_for(lambda: self.avail() == 0))
        p, marker = self.fake_test("t5", 0.2)
        self.assertTrue(_wait_for(lambda: self.census(p.pid) == (True, 0, 1)),
                        "the queued test is not reported as waiting")
        self.assertEqual(_slurp(marker), "", "the test ran without a token")
        os.killpg(holders[0].pid, signal.SIGTERM)
        self.assertTrue(_wait_for(lambda: _slurp(marker) == "held"))
        self.assertEqual(p.wait(timeout=10), 0)

    def test_status_line_reports_tests_separately(self):
        p, marker = self.fake_test("t6", 3.0)
        self.assertTrue(_wait_for(lambda: _slurp(marker) == "held"))
        out = subprocess.run([sys.executable, DAEMON, "status", "--dir", self.dir],
                             env=self.env, capture_output=True, text=True, timeout=20).stdout
        self.assertIn("tok_held=1 ", out)
        self.assertRegex(out, r" tests=[1-9]\d* tests_holding=1 tests_wait=0 ")


def _hooked_test_binary():
    """build/tests/bezel_tests_support, if it is built with the token hook."""
    root = os.path.abspath(os.path.join(SCRIPTS, ".."))
    manifest = os.path.join(root, "build", "tests", "test_binaries.txt")
    for line in _slurp(manifest).splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[0] == "support" and os.path.exists(parts[1]):
            with open(parts[1], "rb") as f:
                if b"/scripts/test_token.sh" in f.read():
                    return parts[1]
    return None


HOOKED = _hooked_test_binary()


@unittest.skipUnless(HOOKED, "no bezel_tests_support built with the test-token hook")
class RealTestBinaryTakesATokenTest(LauncherTestBase):
    """Group 6 on the real binary (tests/test_main.cpp holdTestToken)."""
    TOKENS = 1
    # Fast and always registered: the harness's own shard/registry tests.
    FILTER = os.environ.get("BEZEL_TOKEN_TEST_FILTER", "")

    def setUp(self):
        super().setUp()
        self.env.pop("BEZEL_TEST_TOKEN_HELD", None)
        self.env["BEZEL_SHARD_COUNT"] = "1"
        names = subprocess.run([HOOKED], env=dict(self.env, BEZEL_TEST_ENUMERATE="1"),
                               capture_output=True, text=True, timeout=60).stdout.split()
        self.assertTrue(names, "the binary enumerated nothing")
        self.env["BEZEL_TEST_LIST"] = os.path.join(self.dir, "one.txt")
        with open(self.env["BEZEL_TEST_LIST"], "w") as f:
            f.write((self.FILTER or names[0]) + "\n")
        # Drain the pool: a run that takes a token must now wait.
        self.hog = subprocess.Popen([LAUNCHER, "sleep", "120"], env=self.env,
                                    stderr=subprocess.DEVNULL, start_new_session=True)
        self.assertTrue(_wait_for(lambda: self.avail() == 0))

    def tearDown(self):
        if self.hog.poll() is None:
            os.killpg(self.hog.pid, signal.SIGKILL)
            self.hog.wait(timeout=10)
        super().tearDown()

    def run_bin(self, env, timeout):
        return subprocess.run([HOOKED], env=env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE, text=True, timeout=timeout)

    def test_a_run_waits_for_a_token_and_returns_it(self):
        p = subprocess.Popen([HOOKED], env=self.env, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            with self.assertRaises(subprocess.TimeoutExpired):
                p.wait(timeout=3)
            os.killpg(self.hog.pid, signal.SIGTERM)
            self.hog.wait(timeout=10)
            self.assertEqual(p.wait(timeout=120), 0)
            self.assertIn("waiting for a build token", p.stderr.read())
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
            p.stderr.close()
        self.assertTrue(_wait_for(lambda: self.avail() == self.TOKENS))

    def test_enumeration_takes_no_token(self):
        r = self.run_bin(dict(self.env, BEZEL_TEST_ENUMERATE="1"), 60)
        self.assertEqual(r.returncode, 0)
        self.assertNotIn("waiting for a build token", r.stderr)

    def test_jobserver_off_takes_no_token(self):
        r = self.run_bin(dict(self.env, BEZEL_JOBSERVER="off"), 120)
        self.assertEqual(r.returncode, 0)
        self.assertNotIn("waiting for a build token", r.stderr)

    def test_token_already_held_takes_no_second_one(self):
        r = self.run_bin(dict(self.env, BEZEL_TEST_TOKEN_HELD="1"), 120)
        self.assertEqual(r.returncode, 0)
        self.assertNotIn("waiting for a build token", r.stderr)


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
