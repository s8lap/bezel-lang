"""#3463 / #3464 — the gate's terminal markers, and the commit it pushes.

TWO defects on `scripts/gate_and_push.sh`, both found on a real Run 7 cycle
(2026-08-28) and both in the "the documented workflow does not work" class.

#3463 — THE SELF-TEST PHASE CLOBBERED THE LIVE GATE'S MARKERS.

`gate_and_push.sh` publishes two files as its completion signal:

    $BEZEL_GATE_PIDFILE   its pid, written at start, removed at exit
    $BEZEL_GATE_DONEFILE  its exit status, written once from the EXIT trap

Both default to fixed paths under /var/tmp/bezel-scratch. They were written
UNCONDITIONALLY at the top of the script, before the argument parse, the
clean-tree refusal and the serialization lock — so `gate_and_push.sh --help`
published markers too. #3415 then wired `scripts/gate/tests/` into every gate
cycle as the `gate-selftest` phase, and several tests here drive the REAL
script. Measured in this lane, one module at a time, against a sentinel pair:

    test_repo_identity           -> pidfile DELETED, donefile "1"  (gate FAILED)
    test_integration_bash_python -> pidfile DELETED, donefile "0"  (gate PASSED)

Both are false, and the failure direction is the dangerous one: the donefile
does not go MISSING, it asserts a verdict about a gate that is still running.
The "0" is the worse of the two — it comes from the held-lock abort, which by
definition fires while somebody else's gate owns those files. On the observed
cycle `scripts/wait_pid.sh` exited 3 ("cannot read pidfile") roughly fifty
minutes early, over a gate that went on to pass both oracles.

The issue proposed two fixes and asked for one. Measurement says neither alone
is sufficient, so both are here plus a third:

  * Moving the publication after the argument parse (issue option 2) does NOT
    fix test_repo_identity, whose probe runs the script with NO ARGUMENTS
    against a stub cycle. That invocation IS a gate as far as the script can
    tell, and publishing is correct for it. It is the one that wrote "1".
  * Isolating the tests' markers (issue option 1) does NOT fix an operator's
    `gate_and_push.sh --help` probe against a live gate, which is a documented
    thing to do and clobbers exactly the same way.

So: the script publishes only once it is committed to gating (pinned by
`NonGatingInvocationTest`), the tests redirect their markers into a temp dir
(`marker_isolation`, pinned by `SelfTestSuiteIsolationTest`), and the phase
itself redirects them for every child (pinned by `PhaseRedirectsMarkersTest`).

#3464 — THE GATE COULD NOT PUSH FROM AN INTEGRATION WORKTREE.

`fix #3413` requires gating from the tree you launched from, so a batch is gated
from a worktree on a branch like `r7-c1-integration` that TRACKS `origin/main`.
The final step was a bare `git push`, which git's default push.default=simple
refuses whenever the local branch name differs from its upstream branch name:

    fatal: The upstream branch of your current branch does not match the name
    of your current branch.

Two individually-correct fixes that jointly broke the workflow. `PushTest`
drives the real script against a throwaway bare remote — never `origin` — and
pins that the gated SHA reaches refs/heads/main, that HEAD moving mid-run is
refused, and that the destination can never be inferred silently.

Stdlib only; no build. Runs inside the #3415 gate-selftest phase.
"""

import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
SCRIPTS = REPO / "scripts"
GATE_AND_PUSH = SCRIPTS / "gate_and_push.sh"
FULL_CYCLE = SCRIPTS / "gate_full_cycle.sh"
WAIT_PID = SCRIPTS / "wait_pid.sh"

sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(HERE))
from marker_isolation import marker_env  # noqa: E402

# A git environment that works on a machine with no user identity configured and
# cannot be perturbed by the operator's own config. /dev/null for both config
# scopes also pins push.default at git's built-in default (`simple`), which is
# the setting #3464 is about — reading the operator's real config here would
# make the push tests pass or fail depending on whose machine ran them.
GIT_ENV = dict(
    os.environ,
    GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
    GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t",
    GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null",
)

SENTINEL_PID = b"999999\n"
SENTINEL_DONE = b"SENTINEL-UNTOUCHED\n"


def _git(cwd, *args, **kw):
    env = dict(GIT_ENV)
    env.update(kw.pop("env_extra", {}))
    return subprocess.run(("git",) + args, cwd=str(cwd), env=env,
                          capture_output=True, text=True, timeout=60)


def _make_repo(path):
    """A real git repo at `path` carrying a physical copy of scripts/.

    A copy, not a symlink: #3413 made the script's own location decide the root,
    so a probe must get its own copy or it would resolve back to this checkout
    and gate the wrong tree.
    """
    path = pathlib.Path(path)
    path.mkdir(parents=True, exist_ok=True)
    _git(path, "init", "-q", "-b", "main")
    shutil.copytree(str(SCRIPTS), str(path / "scripts"))
    (path / "marker.txt").write_text("seed\n")
    _git(path, "add", "-A")
    _git(path, "commit", "-q", "-m", "seed")
    return pathlib.Path(os.path.realpath(str(path)))


def _write_script(path, body):
    path = pathlib.Path(path)
    path.write_text("#!/usr/bin/env bash\n" + body)
    path.chmod(0o755)


def _sentinels(directory):
    """Plant a sentinel marker pair; return (env, pidfile, donefile)."""
    d = pathlib.Path(directory)
    pid, done = d / "gate.pid", d / "gate.done"
    pid.write_bytes(SENTINEL_PID)
    done.write_bytes(SENTINEL_DONE)
    env = dict(GIT_ENV)
    env["BEZEL_GATE_PIDFILE"] = str(pid)
    env["BEZEL_GATE_DONEFILE"] = str(done)
    return env, pid, done


class _MarkerAssertions(unittest.TestCase):
    def assertSentinelsUntouched(self, pid, done, what, evidence=""):
        self.assertTrue(
            pid.exists(),
            "%s DELETED the sentinel pidfile. A gate's pid is how "
            "scripts/wait_pid.sh identifies it; removing it makes the waiter "
            "exit 3 (\"cannot read pidfile\") over a gate that is still "
            "running (#3463).%s" % (what, evidence))
        self.assertEqual(
            pid.read_bytes(), SENTINEL_PID,
            "%s OVERWROTE the sentinel pidfile (#3463).%s" % (what, evidence))
        self.assertTrue(
            done.exists(),
            "%s REMOVED the sentinel donefile (#3463).%s" % (what, evidence))
        self.assertEqual(
            done.read_bytes(), SENTINEL_DONE,
            "%s WROTE A VERDICT into the sentinel donefile — it now reads %r. "
            "That is the dangerous direction: the marker does not go missing, "
            "it asserts an outcome for a gate that has not finished (#3463).%s"
            % (what, done.read_bytes(), evidence))


class NonGatingInvocationTest(_MarkerAssertions):
    """No invocation that declines to gate may touch the marker files.

    Every shape below returns before `gate_and_push.sh` runs a gate. Each one
    used to publish, because publication sat above the argument parse.
    """

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.mkdtemp(prefix="gate-markers-nongating-")
        cls.repo = _make_repo(os.path.join(cls._tmp, "repo"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls._tmp, ignore_errors=True)

    def _run(self, args, env_extra=None, cwd=None):
        with tempfile.TemporaryDirectory() as d:
            env, pid, done = _sentinels(d)
            env["GATE_SKIP_LOCK"] = "1"
            env.update(env_extra or {})
            r = subprocess.run(
                [str(self.repo / "scripts" / "gate_and_push.sh")] + list(args),
                cwd=str(cwd or self.repo), env=env, capture_output=True,
                text=True, timeout=120)
            self.assertSentinelsUntouched(
                pid, done, "gate_and_push.sh %s" % (" ".join(args) or "(no args)"),
                evidence="\n  rc=%d\n  stdout=%r\n  stderr=%r"
                         % (r.returncode, r.stdout[-400:], r.stderr[-400:]))
            return r

    def test_help_publishes_nothing(self):
        """The shape the issue calls out: a `--help` probe, which CLAUDE.md
        describes an operator running against a live gate."""
        r = self._run(["--help"])
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_quick_refusal_publishes_nothing(self):
        r = self._run(["--quick"])
        self.assertEqual(r.returncode, 64,
                         "--quick must still be REFUSED (#2633/#2917)")

    def test_unknown_argument_publishes_nothing(self):
        r = self._run(["--nonsense"])
        self.assertEqual(r.returncode, 64)

    def test_dirty_tree_refusal_publishes_nothing(self):
        """The clean-tree refusal (exit 65) runs BEFORE the argument parse, so
        it is not covered by 'publish after parsing' alone — publication has to
        sit below it too."""
        (self.repo / "marker.txt").write_text("dirtied\n")
        try:
            r = self._run([])
            self.assertEqual(r.returncode, 65,
                             "a dirty tree must still refuse (stdout=%r)" % r.stdout)
        finally:
            _git(self.repo, "checkout", "--", "marker.txt")

    def test_held_lock_abort_publishes_nothing(self):
        """THE WORST ONE. The 'another gate is running' abort fires exactly
        when another gate owns these files, and exits 0 — so publishing there
        wrote a PASS verdict over a live gate mid-cycle. That is the "0" this
        lane measured coming out of test_integration_bash_python.

        The probe uses a lockfile inside the temp repo's own copy of the script,
        never the production one: grabbing /var/tmp/bezel-scratch/gate.lock is
        precisely the thing two other tests in this directory have to skip for.
        """
        with tempfile.TemporaryDirectory() as d:
            repo = _make_repo(os.path.join(d, "locked"))
            lock = os.path.join(d, "probe.lock")
            script = repo / "scripts" / "gate_and_push.sh"
            text = script.read_text()
            patched = re.sub(r'GATE_LOCK="[^"]+"',
                             'GATE_LOCK="%s"' % lock, text, count=1)
            self.assertNotEqual(patched, text,
                                "could not redirect GATE_LOCK in the copy — the "
                                "assignment moved or changed shape")
            script.write_text(patched)
            _git(repo, "commit", "-q", "-am", "probe lock")

            holder_code = (
                "import sys, time\n"
                "sys.path.insert(0, %r)\n"
                "from gate import serialize\n"
                "lk = serialize.acquire_gate_lock(%r)\n"
                "assert lk is not None\n"
                "print('HELD', flush=True)\n"
                "time.sleep(60)\n" % (str(SCRIPTS), lock))
            holder = subprocess.Popen(
                [sys.executable, "-c", holder_code],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            try:
                self.assertEqual(holder.stdout.readline().strip(), "HELD",
                                 "holder never acquired the probe lockfile")
                mdir = os.path.join(d, "markers")
                os.makedirs(mdir)
                env, pid, done = _sentinels(mdir)
                r = subprocess.run([str(script)], cwd=str(repo), env=env,
                                   capture_output=True, text=True, timeout=120)
                self.assertEqual(
                    r.returncode, 0,
                    "a held lock must abort CLEANLY (exit 0); stderr=%r"
                    % r.stderr[-400:])
                self.assertIn("another gate is running", r.stderr)
                self.assertSentinelsUntouched(
                    pid, done, "the held-lock abort",
                    evidence="\n  stderr=%r" % r.stderr[-400:])
            finally:
                holder.kill()
                holder.wait(timeout=10)


class GatingInvocationPublishesTest(unittest.TestCase):
    """THE MECHANISM CHECK. Scrubbing markers is easy to overdo: a script that
    never publishes also never clobbers, and every test above would still pass
    while the documented `wait_pid.sh "$BEZEL_GATE_PIDFILE"` protocol was dead.
    So pin the positive half — a real gating run publishes, the pidfile holds
    THIS process's pid while the cycle runs, and the donefile gets the rc.
    """

    def _repo_with_stub_cycle(self, d, cycle_body):
        repo = _make_repo(os.path.join(d, "gating"))
        _write_script(repo / "scripts" / "gate_full_cycle.sh", cycle_body)
        _git(repo, "commit", "-q", "-am", "stub cycle")
        return repo

    def test_a_real_gate_publishes_its_pid_and_writes_its_status(self):
        with tempfile.TemporaryDirectory() as d:
            # The stub cycle reports what the markers looked like WHILE the gate
            # was running, and its own PPID — which is the gate_and_push process.
            # Comparing the two is what proves the pid in the file is the gate's
            # own, rather than merely that a file exists.
            repo = self._repo_with_stub_cycle(d, (
                'echo "STUB_PPID=$PPID"\n'
                'echo "STUB_PIDFILE_SAYS=$(cat "$BEZEL_GATE_PIDFILE" 2>&1)"\n'
                'echo "STUB_DONEFILE_EXISTS=$([ -e "$BEZEL_GATE_DONEFILE" ] '
                '&& echo yes || echo no)"\n'
                'exit 64\n'))
            mdir = os.path.join(d, "markers")
            os.makedirs(mdir)
            env = marker_env(GIT_ENV, mdir)
            env["GATE_SKIP_LOCK"] = "1"
            pid = pathlib.Path(env["BEZEL_GATE_PIDFILE"])
            done = pathlib.Path(env["BEZEL_GATE_DONEFILE"])
            # A stale donefile from a previous gate must be cleared at
            # publication, or a waiter could read the pidfile and the OLD
            # verdict in the same instant.
            done.write_bytes(b"STALE\n")

            r = subprocess.run([str(repo / "scripts" / "gate_and_push.sh")],
                               cwd=str(repo), env=env, capture_output=True,
                               text=True, timeout=120)
            out = r.stdout + r.stderr

            m_ppid = re.search(r"STUB_PPID=(\d+)", out)
            m_says = re.search(r"STUB_PIDFILE_SAYS=(.*)", out)
            self.assertIsNotNone(m_ppid, "stub cycle never ran: %r" % out[:800])
            self.assertIsNotNone(m_says, "stub cycle never ran: %r" % out[:800])
            self.assertEqual(
                m_says.group(1).strip(), m_ppid.group(1),
                "the pidfile must hold the gate_and_push pid WHILE the cycle "
                "runs — that is what wait_pid.sh signals on. Got %r for a gate "
                "at pid %s (#3463).\n%s"
                % (m_says.group(1), m_ppid.group(1), out[:800]))
            self.assertIn(
                "STUB_DONEFILE_EXISTS=no", out,
                "publication must clear a stale donefile, or a waiter can see "
                "a live pidfile beside a previous gate's verdict (#3463)")
            self.assertFalse(
                pid.exists(),
                "the pidfile must be removed when the gate exits (#3158)")
            self.assertEqual(
                done.read_text().strip(), "1",
                "the donefile must carry the gate's own exit status; a failing "
                "cycle makes gate_and_push exit 1 (#3158). Got %r"
                % done.read_text())


class SelfTestSuiteIsolationTest(_MarkerAssertions):
    """The #3415 phase must not write into the markers of the gate running it.

    A source ratchet plus a behavioural confirmation. The ratchet is what makes
    the fix survive: a NEW test module here that drives the real script would
    otherwise reintroduce the defect silently, and nothing would notice until a
    gate reported a verdict for itself again.
    """

    # Every module in this directory whose source names the script, and how it
    # is kept off the production markers. Adding a module that spawns the real
    # script means adding it here as "isolated" and importing marker_isolation.
    KNOWN = {
        "test_integration_bash_python.py": "isolated",
        "test_repo_identity.py": "isolated",
        # Reads the script's TEXT only (the one-EXIT-trap pin) — never runs it.
        "test_gate_and_push_trap.py": "source-only",
        # Spawns a FAKE gate_and_push.sh it writes into a temp dir, to exercise
        # the /proc/<pid>/cmdline sibling detection. Never the real one.
        "test_gate_serialization.py": "fake-in-tempdir",
        # Reads gate.sh / gate_full_cycle.sh TEXT and sources EXTRACTED
        # function bodies against a stub gate.sh in a temp dir (#3666).
        # Never the real gate_and_push.sh.
        "test_gate_full_cycle_pins.py": "source-only",
        # Reads gate_and_push.sh TEXT and sources the EXTRACTED smart-push
        # block against a stub git remote in a temp dir (smart-push design,
        # user directive 2026-09-18). Never the real gate_and_push.sh.
        "test_gate_and_push_smart_push.py": "source-only",
        # This file.
        "test_gate_markers.py": "isolated",
    }

    def _modules_naming_the_script(self):
        found = set()
        for f in sorted(HERE.glob("test_*.py")):
            for ln in f.read_text().splitlines():
                s = ln.strip()
                if s.startswith("#") or "gate_and_push.sh" not in s:
                    continue
                found.add(f.name)
                break
        return found

    def test_every_module_touching_the_script_is_classified(self):
        found = self._modules_naming_the_script()
        self.assertTrue(found, "guard: no module names gate_and_push.sh at all "
                               "— this ratchet would pass vacuously")
        unclassified = sorted(found - set(self.KNOWN))
        self.assertEqual(
            unclassified, [],
            "new self-test module(s) reference scripts/gate_and_push.sh: %s.\n"
            "This directory runs INSIDE every gate (#3415), and the script "
            "publishes the gate's own pid/exit-status markers. If the module "
            "SPAWNS the real script it must import marker_isolation and pass an "
            "isolated env, then be listed in KNOWN as 'isolated'; if it only "
            "reads the source or spawns a fake, say so there. Skipping this is "
            "how #3463 made a live gate report its own failure."
            % ", ".join(unclassified))
        stale = sorted(set(self.KNOWN) - found)
        self.assertEqual(stale, [],
                         "KNOWN lists module(s) that no longer mention the "
                         "script: %s — drop them so the list stays evidence."
                         % ", ".join(stale))

    def test_spawning_modules_import_the_isolation_helper(self):
        for name, kind in sorted(self.KNOWN.items()):
            if kind != "isolated":
                continue
            text = (HERE / name).read_text()
            self.assertIn(
                "marker_isolation", text,
                "%s spawns the real gate_and_push.sh but does not import "
                "marker_isolation, so its probe writes to whatever markers it "
                "inherits — a live gate's, inside the #3415 phase (#3463)."
                % name)

    def test_running_them_leaves_a_sentinel_pair_untouched(self):
        """The behavioural half, run against the modules that actually drive the
        script. This is the measurement the issue reports, inverted: on the
        unfixed tree these two rewrote the sentinel donefile to "1" and "0" and
        deleted the sentinel pidfile.
        """
        modules = sorted(n[:-3] for n, k in self.KNOWN.items()
                         if k == "isolated" and n != pathlib.Path(__file__).name)
        self.assertTrue(modules, "guard: nothing to run — vacuous")
        with tempfile.TemporaryDirectory() as d:
            env, pid, done = _sentinels(d)
            r = subprocess.run(
                [sys.executable, "-m", "unittest"] + modules,
                cwd=str(HERE), env=env, capture_output=True, text=True,
                timeout=600)
            self.assertSentinelsUntouched(
                pid, done, "the gate self-tests (%s)" % ", ".join(modules),
                evidence="\n  rc=%d\n  tail=%r" % (r.returncode, r.stderr[-400:]))


class PhaseRedirectsMarkersTest(unittest.TestCase):
    """Defence in depth: the phase redirects the markers for EVERY child.

    The two layers above are per-script and per-test. This one is per-phase, and
    it is the only one that also covers a module nobody classified yet — a
    gate's own markers should not be reachable from inside its self-test phase
    at all.
    """

    def test_the_selftest_phase_overrides_both_marker_paths(self):
        text = FULL_CYCLE.read_text()
        m = re.search(r"^[^\n#]*BEZEL_GATE_PIDFILE=.*\n(?:.*\n){0,4}?"
                      r".*python3 -m unittest discover -s scripts/gate/tests",
                      text, re.M)
        self.assertIsNotNone(
            m,
            "the gate-selftest phase must run unittest with "
            "BEZEL_GATE_PIDFILE/BEZEL_GATE_DONEFILE pointed somewhere "
            "throwaway, so no test in that directory can reach the running "
            "gate's own markers (#3463)")
        self.assertIn("BEZEL_GATE_DONEFILE=", m.group(0),
                      "only the pidfile is redirected; the donefile is the one "
                      "that publishes a false VERDICT (#3463)")


class WaitPidLaunchRaceTest(unittest.TestCase):
    """#3463 moved publication later, which widens the launch race the
    documented protocol runs into:

        setsid nohup scripts/gate_and_push.sh > LOG 2>&1 < /dev/null &
        scripts/wait_pid.sh "$BEZEL_GATE_PIDFILE"

    A fork+exec is ~5ms; the gate now needs ~50ms to reach its publication
    point. Exiting 3 instantly on a not-yet-published pidfile is the same false
    signal, so wait_pid.sh waits a bounded moment for it to appear.
    """

    def test_waits_for_a_pidfile_that_appears_late(self):
        with tempfile.TemporaryDirectory() as d:
            pidfile = os.path.join(d, "gate.pid")
            # The worker must be DETACHED, exactly as the documented protocol
            # detaches the gate (`setsid nohup … &`). A plain Popen would leave
            # it a child of this process; unreaped, it becomes a zombie, and
            # `kill -0` on a zombie SUCCEEDS — wait_pid would then spin to its
            # timeout over a process that had exited. That is a property of the
            # probe, not of the script, and it cost a false failure here.
            subprocess.run(
                ["bash", "-c",
                 "setsid bash -c 'sleep 0.6; echo $$ > %s; sleep 1.2; "
                 "rm -f %s' >/dev/null 2>&1 </dev/null &" % (pidfile, pidfile)],
                check=True, timeout=30)
            r = subprocess.run(
                [str(WAIT_PID), pidfile, "--poll", "1", "--timeout", "30"],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(
                r.returncode, 0,
                "wait_pid must wait for a pidfile published a moment after "
                "launch, not exit 3 on the race (#3463). stdout=%r "
                "stderr=%r" % (r.stdout, r.stderr))
            self.assertIn("waiting up to", r.stdout,
                          "guard: the pidfile was already there, so the grace "
                          "path never ran and this proves nothing")

    def test_grace_zero_keeps_the_old_immediate_refusal(self):
        with tempfile.TemporaryDirectory() as d:
            r = subprocess.run(
                [str(WAIT_PID), os.path.join(d, "never"),
                 "--pidfile-grace", "0"],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(r.returncode, 3)
            self.assertIn("cannot read pidfile", r.stderr)


class PushTest(unittest.TestCase):
    """#3464 — the push must land the GATED commit, on main, from any branch.

    Every probe here pushes to a throwaway bare repository created in a temp
    dir. Nothing in this class can reach the project's real `origin`: the
    remote is added by the fixture and named after that bare path.
    """

    def _fixture(self, d, branch="r7-c1-integration", upstream="main",
                 trend_body='echo "1 records scanned"\n',
                 cycle_body='echo "STUBCYCLE ok"\n'):
        """A bare remote + a work repo whose current branch tracks it.

        `branch` is deliberately NOT the upstream's name — that mismatch is the
        whole defect: git's default push.default=simple refuses a bare `git
        push` for it, which is what an integration worktree looks like.
        """
        bare = pathlib.Path(d) / "remote.git"
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main", str(bare)],
                       env=GIT_ENV, check=True, timeout=60)
        repo = _make_repo(os.path.join(d, "work"))
        # ⚠ SEED THE REMOTE FIRST, THEN COMMIT. Ordering these the other way
        # round leaves the remote already at HEAD, so "the gated commit reached
        # main" is satisfied by a push that transferred nothing — the assertion
        # passes without the push having done anything. Seeding first leaves the
        # remote one commit behind, so only a real push can satisfy it.
        _git(repo, "remote", "add", "origin", str(bare))
        _git(repo, "push", "-q", "origin", "main")
        if upstream not in (None, "main"):
            _git(repo, "push", "-q", "origin", "main:" + upstream)
            _git(repo, "fetch", "-q", "origin")
        # Stub the two things the push path runs, so no gate and no telemetry
        # scan happens: a cycle that reports success, and a trend check that
        # produces the "records scanned" line the push boundary requires.
        _write_script(repo / "scripts" / "gate_full_cycle.sh", cycle_body)
        _write_script(repo / "scripts" / "gate_trend.sh", trend_body)
        _git(repo, "commit", "-q", "-am", "stub cycle + trend")
        _git(repo, "branch", "-m", branch)
        if upstream is not None:
            _git(repo, "branch", "--set-upstream-to=origin/" + upstream, branch)
        return bare, repo

    def _run_push(self, d, repo, extra_env=None):
        mdir = os.path.join(d, "markers")
        os.makedirs(mdir, exist_ok=True)
        env = marker_env(GIT_ENV, mdir)
        env["GATE_SKIP_LOCK"] = "1"
        env["BEZEL_NO_GC"] = "1"
        # Point the verdict at a path that does not exist: the reads then take
        # the documented "writer is best-effort" fallback instead of consulting
        # whatever /var/tmp/bezel-scratch/gate-verdict.json a real gate left.
        env["GATE_VERDICT_FILE"] = os.path.join(d, "no-such-verdict.json")
        env.update(extra_env or {})
        return subprocess.run(
            [str(repo / "scripts" / "gate_and_push.sh")],
            cwd=str(repo), env=env, capture_output=True, text=True, timeout=180)

    def _remote_ref(self, bare, ref="refs/heads/main"):
        r = subprocess.run(["git", "--git-dir", str(bare), "rev-parse", ref],
                           env=GIT_ENV, capture_output=True, text=True,
                           timeout=60)
        return r.stdout.strip() if r.returncode == 0 else None

    def test_pushes_from_a_branch_whose_name_differs_from_its_upstream(self):
        """THE REGRESSION. On the unfixed script this fails at `git push` with
        'The upstream branch of your current branch does not match the name of
        your current branch', after a green gate — the exact Run 7 Cycle 1
        transcript."""
        with tempfile.TemporaryDirectory() as d:
            bare, repo = self._fixture(d)
            head = _git(repo, "rev-parse", "HEAD").stdout.strip()
            self.assertNotEqual(
                self._remote_ref(bare), head,
                "guard: the remote already holds the gated commit, so the push "
                "would be a no-op and this test could not fail")
            r = self._run_push(d, repo)
            out = r.stdout + r.stderr
            self.assertEqual(
                r.returncode, 0,
                "a green gate on an integration branch must push (#3464).\n%s"
                % out[-1500:])
            self.assertEqual(
                self._remote_ref(bare), head,
                "the gated commit must be the one that reached main (#3464)")
            self.assertIn(
                "PUSHED origin/main now", out,
                "scripts/gate_wait.sh matches this line as its ONLY accept "
                "marker — nothing in the gate prints 'GATE PASSED' — so a "
                "reworded success line leaves every waiter spinning")

    def test_pushes_nothing_but_the_destination_ref(self):
        """An explicit refspec must not drag the local branch name along."""
        with tempfile.TemporaryDirectory() as d:
            bare, repo = self._fixture(d, branch="r7-c1-integration")
            r = self._run_push(d, repo)
            self.assertEqual(r.returncode, 0, (r.stdout + r.stderr)[-1500:])
            refs = subprocess.run(
                ["git", "--git-dir", str(bare), "for-each-ref",
                 "--format=%(refname)"],
                env=GIT_ENV, capture_output=True, text=True, timeout=60)
            self.assertEqual(
                sorted(refs.stdout.split()), ["refs/heads/main"],
                "the push created a ref other than the upstream's: %r"
                % refs.stdout)

    def test_refuses_when_head_moved_after_the_verdict(self):
        """#3413's point is that the verdict and the pushed commit are the same
        tree. The trend stub commits, which is the only hook between the two
        HEAD reads, so this pins the guard rather than the happy path."""
        with tempfile.TemporaryDirectory() as d:
            bare, repo = self._fixture(
                d, trend_body=('git commit -q --allow-empty -m "moved" \n'
                               'echo "1 records scanned"\n'))
            before = self._remote_ref(bare)
            self.assertIsNotNone(before, "fixture: the remote must be seeded")
            r = self._run_push(d, repo)
            out = r.stdout + r.stderr
            self.assertNotEqual(r.returncode, 0,
                                "HEAD moved mid-run; that must not push")
            self.assertIn("HEAD moved after the gate's verdict", out)
            self.assertEqual(
                self._remote_ref(bare), before,
                "nothing may reach the remote when the guard fires")

    def test_refuses_when_the_branch_has_no_upstream(self):
        with tempfile.TemporaryDirectory() as d:
            bare, repo = self._fixture(d, upstream=None)
            r = self._run_push(d, repo)
            out = r.stdout + r.stderr
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("no upstream configured", out,
                          "with no upstream the script must say so, not guess "
                          "a destination (#3464).\n%s" % out[-1200:])

    def test_refuses_a_destination_that_is_not_main(self):
        """'Must not silently push some other branch' — the risk of widening a
        bare `git push` into an explicit refspec."""
        with tempfile.TemporaryDirectory() as d:
            bare, repo = self._fixture(d, branch="side", upstream="scratch")
            before = self._remote_ref(bare, "refs/heads/scratch")
            r = self._run_push(d, repo)
            out = r.stdout + r.stderr
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("upstream is not main", out, out[-1200:])
            self.assertEqual(self._remote_ref(bare, "refs/heads/scratch"), before,
                             "the refused push must not have happened")

    def test_non_main_destination_is_reachable_only_deliberately(self):
        """The escape hatch works, and does NOT claim origin/main — a waiter
        must not accept on a marker naming a destination this run never wrote.
        """
        with tempfile.TemporaryDirectory() as d:
            bare, repo = self._fixture(d, branch="side", upstream="scratch")
            head = _git(repo, "rev-parse", "HEAD").stdout.strip()
            self.assertNotEqual(
                self._remote_ref(bare, "refs/heads/scratch"), head,
                "guard: the remote already holds the gated commit — vacuous")
            r = self._run_push(d, repo, {"BEZEL_GATE_PUSH_DEST_OK": "1"})
            out = r.stdout + r.stderr
            self.assertEqual(r.returncode, 0, out[-1500:])
            self.assertEqual(self._remote_ref(bare, "refs/heads/scratch"), head)
            self.assertNotIn(
                "PUSHED origin/main now", out,
                "a non-main push must not emit gate_wait.sh's accept marker "
                "for origin/main — that would be a false terminal verdict")


if __name__ == "__main__":
    unittest.main()
