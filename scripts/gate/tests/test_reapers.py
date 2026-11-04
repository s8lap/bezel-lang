"""scripts/gc_scratch.sh and scripts/reap_stale.sh do what the coordinator did by hand.

WHY (2026-11-05, measured). /mnt/fast was 89% full with 160 worktrees.
`gc_scratch.sh --worktrees` would reap ONE of them: a run banks onto a staging
branch, so nothing is "in origin/main" until the end-of-run gate. And
`reap_stale.sh` listed 36 stale processes, ~20 of them the idle PARENT shells of
test suites live lanes were still running, plus the coordinator's own live
waiter loops: `--kill` would have orphaned live work. The coordinator reaped by
hand (back a detached HEAD with keep/<name>-<sha8>, remove landed trees, delete
only build/ of dirty ones). These tests pin that behaviour:

  gc_scratch.sh  landed on staging by patch-id; a detached HEAD no ref contains
                 is backed by keep/ before removal; --idle-builds deletes only
                 build/ and keeps the WIP; --protect FILE is honoured; untracked
                 files keep a tree; a live cwd is still honoured (a regression
                 guard: the old script already did that).
  reap_stale.sh  an idle parent of a working child is exempt; a self-matching
                 `while pgrep -f X` waiter is STALE tier=W at any age; a process
                 cwd'd in a --protect path is exempt.

Every repo is a throwaway; gc_scratch.sh only ever gets worktree scopes here
(never --builds or --root, which act on machine-wide paths), and reap_stale.sh
runs as a DRY-RUN (it lists; it never signals).
"""
import os
import pathlib
import signal
import subprocess
import tempfile
import time
import unittest

from orch_tools_support import SCRIPTS, Repo, git_env

GC = SCRIPTS / "gc_scratch.sh"
REAP = SCRIPTS / "reap_stale.sh"


def _old(path, hours):
    t = time.time() - hours * 3600
    os.utime(path, (t, t))


class _GcBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="gc-reaper-")
        self.tmp = pathlib.Path(self._tmp.name).resolve()
        self.env = git_env(self.tmp)
        self.repo = Repo(self.tmp / "repo", self.env)
        self.repo.git("update-ref", "refs/remotes/origin/main", "main")
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            p.kill(); p.wait()
        self._tmp.cleanup()

    def lane(self, name, rel, text):
        """A branch off main with one commit; returns its sha."""
        self.repo.git("switch", "-q", "-c", name, "main")
        sha = self.repo.change(rel, "lane %s\n" % name, text)
        self.repo.git("switch", "-q", "main")
        return sha

    def pick_onto(self, branch, sha, base="main"):
        """Cherry-pick `sha` onto `branch` (created from `base` if missing):
        a different sha with the same patch-id."""
        if self.repo.git("rev-parse", "-q", "--verify", branch, check=False).returncode:
            self.repo.git("branch", branch, base)
        self.repo.git("switch", "-q", branch)
        # a later committer date: same patch, guaranteed different sha
        env = dict(self.env, GIT_COMMITTER_DATE="@%d +0000" % (int(time.time()) + 3600))
        self.repo.git("cherry-pick", sha, env=env)
        self.repo.git("switch", "-q", "main")
        return self.repo.out("rev-parse", branch)

    def add_wt(self, name, ref, detach=False):
        path = self.tmp / "wt" / name
        args = ["worktree", "add", "-q"] + (["--detach"] if detach else []) + [str(path), ref]
        self.repo.git(*args)
        return path

    def gc(self, *args, idle_min="0"):
        env = dict(self.env, BEZEL_GC_WT_IDLE_MIN=idle_min)
        env.pop("BEZEL_STAGING_REF", None)
        return subprocess.run(["bash", str(GC)] + list(args), cwd=str(self.repo.root),
                              env=env, capture_output=True, text=True)

    def wt_paths(self):
        out = self.repo.out("worktree", "list", "--porcelain")
        return [l[len("worktree "):] for l in out.splitlines() if l.startswith("worktree ")]


class GcScratchTest(_GcBase):
    def test_landed_on_staging_by_patch_id_is_reaped(self):
        sha = self.lane("lane-a", "a.txt", "a\n")
        picked = self.pick_onto("staging", sha)
        self.assertNotEqual(sha, picked)  # picked = same patch, different commit
        wt = self.add_wt("lane-a", "lane-a")
        r = self.gc("--worktrees", "--staging-ref", "staging")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("would reap landed worktree %s" % wt, r.stdout)
        self.assertIn("landed in staging by patch-id", r.stdout)
        self.assertIn("landed-staging=1", r.stdout)
        r = self.gc("--worktrees", "--staging-ref", "staging", "--apply")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(wt.exists(), r.stdout)
        self.assertEqual(self.repo.out("rev-parse", "lane-a"), sha)  # branch kept

    def test_a_commit_not_on_staging_keeps_the_worktree(self):
        sha = self.lane("lane-b", "b.txt", "b\n")
        self.pick_onto("staging", sha)
        self.repo.git("switch", "-q", "lane-b")
        self.repo.change("b2.txt", "second, never picked\n")
        self.repo.git("switch", "-q", "main")
        wt = self.add_wt("lane-b", "lane-b")
        r = self.gc("--worktrees", "--staging-ref", "staging", "--apply")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("KEEP (unmerged", r.stdout)
        self.assertTrue(wt.exists())

    def test_landed_only_ignores_the_staging_env_default(self):
        # gate_and_push.sh's on-push GC must keep its origin/main-only meaning.
        sha = self.lane("lane-c", "c.txt", "c\n")
        self.pick_onto("staging", sha)
        wt = self.add_wt("lane-c", "lane-c")
        env = dict(self.env, BEZEL_GC_WT_IDLE_MIN="0", BEZEL_STAGING_REF="staging")
        r = subprocess.run(["bash", str(GC), "--landed-only"], cwd=str(self.repo.root),
                           env=env, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn("would reap", r.stdout)
        self.assertTrue(wt.exists())

    def test_detached_unreachable_head_is_backed_by_a_keep_branch(self):
        # A detached worktree at a commit no ref contains, landed in origin/main
        # by patch-id (cherry-picked there). Removing it must not lose the commit.
        sha = self.lane("tmp-d", "d.txt", "d\n")
        self.pick_onto("main", sha)
        self.repo.git("update-ref", "refs/remotes/origin/main", "main")
        wt = self.add_wt("rev-d", sha, detach=True)
        self.repo.git("branch", "-D", "tmp-d")  # now nothing contains sha
        self.assertEqual(self.repo.out("for-each-ref", "--contains", sha), "")
        r = self.gc("--worktrees", "--apply")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(wt.exists(), r.stdout)
        keep = "keep/rev-d-" + sha[:8]
        self.assertEqual(self.repo.out("rev-parse", "refs/heads/" + keep), sha, r.stdout)

    def test_idle_builds_deletes_only_build_and_keeps_the_wip(self):
        self.lane("lane-w", "w.txt", "w\n")
        wt = self.add_wt("lane-w", "lane-w")
        (wt / "w.txt").write_text("uncommitted edit\n")       # WIP (tracked)
        (wt / "build" / "obj").mkdir(parents=True)
        (wt / "build" / "CMakeCache.txt").write_text("# cache\n")
        (wt / "build" / "obj" / "x.o").write_text("o" * 4096)
        for p in [wt / "build" / "obj" / "x.o", wt / "build" / "obj", wt / "build" / "CMakeCache.txt",
                  wt / "build"]:
            _old(p, 5)
        gitdir = pathlib.Path(self.repo.out("-C", str(wt), "rev-parse", "--absolute-git-dir"))
        _old(gitdir / "index", 5)
        r = self.gc("--worktrees", "--idle-builds", "--apply", idle_min="120")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("KEEP (WIP", r.stdout)
        self.assertFalse((wt / "build").exists(), r.stdout)
        self.assertTrue(wt.exists())
        self.assertEqual((wt / "w.txt").read_text(), "uncommitted edit\n")
        self.assertIn("build-only=1", r.stdout)

    def test_idle_builds_spares_a_recently_written_build(self):
        self.lane("lane-r", "r.txt", "r\n")
        wt = self.add_wt("lane-r", "lane-r")
        (wt / "r.txt").write_text("wip\n")
        (wt / "build").mkdir()
        (wt / "build" / "CMakeCache.txt").write_text("# cache\n")  # fresh mtime
        gitdir = pathlib.Path(self.repo.out("-C", str(wt), "rev-parse", "--absolute-git-dir"))
        _old(gitdir / "index", 5)
        r = self.gc("--worktrees", "--idle-builds", "--apply", idle_min="120")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue((wt / "build" / "CMakeCache.txt").exists(), r.stdout)

    def test_protect_file_is_honoured(self):
        sha = self.lane("lane-p", "p.txt", "p\n")
        self.pick_onto("main", sha)
        self.repo.git("update-ref", "refs/remotes/origin/main", "main")
        wt = self.add_wt("lane-p", "lane-p")
        pf = self.tmp / "protect.txt"
        pf.write_text("# live lanes\n%s\n" % wt)
        r = self.gc("--worktrees", "--protect", str(pf), "--apply")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("PROTECTED by --protect", r.stdout)
        self.assertTrue(wt.exists())
        r = self.gc("--worktrees", "--protect", str(self.tmp / "missing.txt"))
        self.assertEqual(r.returncode, 2, "a missing --protect file must not silently unprotect")

    def test_live_cwd_in_a_subdirectory_is_honoured(self):
        # A REGRESSION GUARD, not a new behaviour: the pre-change script already
        # kept a worktree a live process is cwd'd in (top or beneath), and this
        # test passes on it too (measured). It pins that the reworked loop still
        # checks cwd BEFORE the landed check, on a tree that is otherwise
        # reapable. (git canonicalizes a symlinked `worktree add` path itself,
        # so a symlink adds nothing here.)
        sha = self.lane("lane-l", "l.txt", "l\n")
        self.pick_onto("main", sha)
        self.repo.git("update-ref", "refs/remotes/origin/main", "main")
        wt = self.add_wt("lane-l", "lane-l")
        (wt / "sub").mkdir()
        self.procs.append(subprocess.Popen(["sleep", "120"], cwd=str(wt / "sub")))
        time.sleep(0.2)
        r = self.gc("--worktrees", "--apply")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("IN USE", r.stdout)
        self.assertTrue(wt.exists())

    def test_untracked_files_keep_a_landed_worktree(self):
        sha = self.lane("lane-u", "u.txt", "u\n")
        self.pick_onto("main", sha)
        self.repo.git("update-ref", "refs/remotes/origin/main", "main")
        wt = self.add_wt("lane-u", "lane-u")
        (wt / "new_test.cpp").write_text("// not yet added\n")
        r = self.gc("--worktrees", "--apply")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("KEEP (untracked files)", r.stdout)
        self.assertTrue((wt / "new_test.cpp").exists())

    def _backdate_index(self, wt, hours=5):
        gitdir = pathlib.Path(self.repo.out("-C", str(wt), "rev-parse", "--absolute-git-dir"))
        _old(gitdir / "index", hours)

    def _old_build(self, root):
        (root / "obj").mkdir(parents=True)
        (root / "CMakeCache.txt").write_text("# cache\n")
        (root / "obj" / "x.o").write_text("o" * 4096)
        for q in [root / "obj" / "x.o", root / "obj", root / "CMakeCache.txt", root]:
            _old(q, 5)

    def test_a_commit_only_in_the_head_reflog_gets_a_keep_branch(self):
        # r39-rev-reaper G2: a detached lane commits Y, then switches back to
        # main. HEAD is landed, but Y lives only in the per-worktree HEAD reflog,
        # which `worktree remove` deletes with the tree.
        wt = self.add_wt("rev-g2", "main", detach=True)
        (wt / "g2.txt").write_text("only in the reflog\n")
        self.repo.git("-C", str(wt), "add", "g2.txt")
        self.repo.git("-C", str(wt), "commit", "-q", "-m", "g2")
        y = self.repo.out("-C", str(wt), "rev-parse", "HEAD")
        self.repo.git("-C", str(wt), "switch", "-q", "--detach", "main")
        self.assertEqual(self.repo.out("for-each-ref", "--contains", y), "")
        r = self.gc("--worktrees", "--apply")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.repo.out("rev-parse", "-q", "--verify", "refs/heads/keep/rev-g2-" + y[:8]),
                         y, r.stdout)
        self.assertIn("back reflog-only commit", r.stdout)

    def test_idle_builds_spares_a_build_a_live_process_runs_from(self):
        # r39-rev-reaper IB1: a test binary running out of build/ from another cwd.
        self.lane("lane-x", "x.txt", "x\n")
        wt = self.add_wt("lane-x", "lane-x")
        (wt / "x.txt").write_text("wip\n")
        self._old_build(wt / "build")
        exe = wt / "build" / "obj" / "fake_tests"
        exe.write_bytes(pathlib.Path("/usr/bin/sleep").read_bytes())
        exe.chmod(0o755)
        _old(exe, 5); _old(wt / "build" / "obj", 5)
        self.procs.append(subprocess.Popen([str(exe), "120"], cwd=str(self.tmp)))
        time.sleep(0.3)
        self._backdate_index(wt)
        r = self.gc("--worktrees", "--idle-builds", "--apply", idle_min="120")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue((wt / "build" / "CMakeCache.txt").exists(), r.stdout)
        self.assertIn("a live process runs from or is cwd'd in it", r.stdout)

    def test_idle_builds_never_follows_a_symlinked_build(self):
        # r39-rev-reaper IB2: a symlinked build/ with no backref may be SHARED.
        self.lane("lane-y", "y.txt", "y\n")
        wt = self.add_wt("lane-y", "lane-y")
        (wt / "y.txt").write_text("wip\n")
        shared = self.tmp / "shared-build"
        self._old_build(shared)
        (wt / "build").symlink_to(shared)
        self._backdate_index(wt)
        r = self.gc("--worktrees", "--idle-builds", "--apply", idle_min="120")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue((shared / "CMakeCache.txt").exists(), r.stdout)
        self.assertTrue((wt / "build").is_symlink())

    def test_a_failing_git_status_keeps_the_worktree(self):
        # r39-rev-reaper G3: a corrupt index makes `git status` fail; that must
        # not read as "clean".
        sha = self.lane("lane-z", "z.txt", "z\n")
        self.pick_onto("main", sha)
        self.repo.git("update-ref", "refs/remotes/origin/main", "main")
        wt = self.add_wt("lane-z", "lane-z")
        (wt / "z.txt").write_text("tracked edit\n")
        gitdir = pathlib.Path(self.repo.out("-C", str(wt), "rev-parse", "--absolute-git-dir"))
        (gitdir / "index").write_bytes(b"DIRC garbage, not an index")
        r = self.gc("--worktrees", "--apply")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue((wt / "z.txt").exists(), r.stdout)
        self.assertEqual((wt / "z.txt").read_text(), "tracked edit\n")

    def test_summary_line_counts(self):
        sha = self.lane("lane-s", "s.txt", "s\n")
        self.pick_onto("staging", sha)
        self.add_wt("lane-s", "lane-s")
        r = self.gc("--worktrees", "--staging-ref", "staging")
        self.assertIn("[gc] summary (dry-run): worktrees seen=2 reaped=1", r.stdout)
        self.assertIn("[gc] summary (dry-run): reclaim would be ", r.stdout)


class ReapStaleTest(unittest.TestCase):
    """End to end against the live process table, as a DRY-RUN. The processes
    are this test's own, tagged with a unique marker; anything else the box is
    running is listed too and ignored."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="reap-stale-")
        cls.tmp = pathlib.Path(cls._tmp.name).resolve()
        mk = "reaptest%d" % os.getpid()
        cls.mk = mk
        (cls.tmp / "lanewt").mkdir()
        # A cwd outside every worktree: reap_stale.sh protects the worktree it
        # runs from, and this test file lives in one.
        (cls.tmp / "elsewhere").mkdir()
        cls.protect = cls.tmp / "protect.txt"
        cls.protect.write_text(str(cls.tmp / "lanewt") + "\n")

        def spawn(script, cwd=None):
            return subprocess.Popen(["bash", "-c", script], cwd=cwd or str(cls.tmp / "elsewhere"),
                                    start_new_session=True,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # argv carries `agent_check.sh`, so each one matches the HEAVY pattern.
        cls.busy_parent = spawn(": agent_check.sh %s-busy; while :; do :; done & wait" % mk)
        cls.idle_parent = spawn(": agent_check.sh %s-idle; sleep 300 & wait" % mk)
        cls.protected = spawn(": agent_check.sh %s-prot; sleep 300 & wait" % mk,
                              cwd=str(cls.tmp / "lanewt"))
        # Self-matching waiter: its own `pgrep -f` sees its own argv.
        cls.blind = spawn("while pgrep -f %s-target >/dev/null; do sleep 100; done" % mk)
        # r39-rev-reaper B1: waiters whose target is ALIVE. Older than --age 30,
        # neither may be STALE: `while kill -0` is a probe-only shape, and the
        # `until test -f` one (an aged shape) is decided WAITING, hence exempt.
        cls.live_target = spawn("exec sleep 600")
        cls.kill0 = spawn("while kill -0 %d 2>/dev/null; do sleep 100; done" % cls.live_target.pid)
        cls.until_missing = spawn("until test -f %s/never-%s; do sleep 100; done" % (cls.tmp, mk))
        cls.procs = [cls.busy_parent, cls.idle_parent, cls.protected, cls.blind,
                     cls.live_target, cls.kill0, cls.until_missing]
        time.sleep(32)  # every candidate must be >= 30 s old
        cls.env = dict(os.environ, BEZEL_GATE_PIDFILE=str(cls.tmp / "no-gate.pid"))

    @classmethod
    def tearDownClass(cls):
        for p in cls.procs:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.wait()
        cls._tmp.cleanup()

    def reap(self, *args):
        return subprocess.run(["bash", str(REAP), "--settle", "3"] + list(args),
                              env=self.env, capture_output=True, text=True, timeout=300)

    @staticmethod
    def stale_line(out, pid):
        for l in out.splitlines():
            if l.startswith("STALE ") and (" pid=%d " % pid) in l:
                return l
        return None

    def test_tiers_with_a_low_age_bar(self):
        r = self.reap("--age", "30", "--orphan-age", "30", "--no-exclude-working",
                      "--protect", str(self.protect))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        # control: an idle parent of an idle child IS stale (the run saw our pids)
        self.assertIsNotNone(self.stale_line(r.stdout, self.idle_parent.pid), r.stdout)
        # an idle parent of a WORKING child is exempt (measured 2026-11-05)
        self.assertIsNone(self.stale_line(r.stdout, self.busy_parent.pid), r.stdout)
        self.assertIn("working-tree pid=%d " % self.busy_parent.pid, r.stdout)
        # cwd inside a --protect path: exempt
        self.assertIsNone(self.stale_line(r.stdout, self.protected.pid), r.stdout)
        self.assertIn("protected pid=%d " % self.protected.pid, r.stdout)

    def test_a_waiter_on_a_live_target_is_never_aged_stale(self):
        r = self.reap("--age", "30", "--orphan-age", "30", "--no-exclude-working")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIsNotNone(self.stale_line(r.stdout, self.idle_parent.pid), r.stdout)  # control
        self.assertIsNone(self.stale_line(r.stdout, self.kill0.pid), r.stdout)
        self.assertIsNone(self.stale_line(r.stdout, self.until_missing.pid), r.stdout)
        self.assertIn("waiting pid=%d " % self.until_missing.pid, r.stdout)

    def test_self_matching_waiter_is_tier_w_at_any_age(self):
        r = self.reap("--age", "3600", "--orphan-age", "3600")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        line = self.stale_line(r.stdout, self.blind.pid)
        self.assertIsNotNone(line, r.stdout)
        self.assertIn("tier=W ", line)
        self.assertIn("reason=waiter-target-satisfied", line)
        # under a 3600 s bar, nothing of ours is tier A/B
        self.assertIsNone(self.stale_line(r.stdout, self.idle_parent.pid), r.stdout)

    def test_self_test_passes(self):
        r = subprocess.run(["bash", str(REAP), "--self-test"], env=self.env,
                           capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("self-test: PASS", r.stdout)


class WaiterTargetTest(unittest.TestCase):
    """scripts/reap_waiter_target.py on live synthetic waiters. Tier W must be
    PROVABLE: any process other than the waiter itself and its own pgrep/grep
    poll that matches the pattern is a live target, so the verdict is WAITING
    (r39-rev-reaper B2)."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory(prefix="reap-wt-")
        cls.tmp = pathlib.Path(cls._tmp.name).resolve()
        mk = "wtgt%d" % os.getpid()

        def spawn(script):
            return subprocess.Popen(["bash", "-c", script], cwd=str(cls.tmp), start_new_session=True,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # control: blind, nothing else matches -> SATISFIED
        cls.blind = spawn("while pgrep -f %s-gone >/dev/null; do sleep 100; done" % mk)
        # W1: the target is the waiter's OWN background child (and the waiter
        # itself does not match): it exits once the child dies.
        cls.w1 = spawn('( exec -a %s-w1 sleep 600 ) & while pgrep -f "^%s-w[1]" >/dev/null; '
                       'do sleep 100; done' % (mk, mk))
        # W2: a live target whose argv mentions `pgrep` and the pattern.
        cls.w2_target = spawn("sleep 600; : pgrep %s-t2" % mk)
        cls.w2 = spawn("while pgrep -f %s-t2 >/dev/null; do sleep 100; done" % mk)
        # W4: the target is the waiter's ANCESTOR (a script polling for itself).
        cls.w4_outer = spawn(": %s-a4; bash -c 'while pgrep -f %s-a4 >/dev/null; do sleep 100; done' & wait"
                             % (mk, mk))
        cls.procs = [cls.blind, cls.w1, cls.w2_target, cls.w2, cls.w4_outer]
        time.sleep(1.5)
        kids = subprocess.run(["pgrep", "-P", str(cls.w4_outer.pid)], capture_output=True,
                              text=True).stdout.split()
        cls.w4 = int(kids[0]) if kids else None

    @classmethod
    def tearDownClass(cls):
        for p in cls.procs:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            p.wait()
        cls._tmp.cleanup()

    def verdict(self, pid):
        r = subprocess.run(["python3", str(SCRIPTS / "reap_waiter_target.py"), str(pid)],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.split("\t")[1], r.stdout

    def test_blind_waiter_with_no_target_is_satisfied(self):
        v, out = self.verdict(self.blind.pid)
        self.assertEqual(v, "SATISFIED", out)

    def test_target_is_the_waiters_own_background_child(self):
        v, out = self.verdict(self.w1.pid)
        self.assertEqual(v, "WAITING", out)

    def test_target_argv_mentions_pgrep_and_the_pattern(self):
        v, out = self.verdict(self.w2.pid)
        self.assertEqual(v, "WAITING", out)

    def test_target_is_the_waiters_ancestor(self):
        self.assertIsNotNone(self.w4)
        v, out = self.verdict(self.w4)
        self.assertEqual(v, "WAITING", out)


if __name__ == "__main__":
    unittest.main()
