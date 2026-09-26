"""Worktree ownership: scripts/wt_owner.sh, scripts/hooks/*, scripts/install_hooks.sh.

Each test installs the hooks into a THROWAWAY repo's own hooks dir (the temp
repo's common git dir), never the real one.
"""
import tempfile
import unittest

from orch_tools_support import SCRIPTS, WANT, Repo, git_env, run


class OwnershipTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="wt-owner-")
        self.env = git_env(self._tmp.name)
        self.repo = Repo(self._tmp.name + "/repo", self.env)
        self.repo.copy_scripts("wt_owner.sh", "hooks/pre-commit", "hooks/post-checkout")
        r = run("install_hooks.sh", cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.hooks = self.repo.root / ".git" / "hooks"

    def tearDown(self):
        self._tmp.cleanup()

    def claim(self, owner="coord-a"):
        r = run("wt_owner.sh", "claim", owner, cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)

    def try_commit(self, name, env=None):
        self.repo.write(name, name + "\n")
        return self.repo.commit("c %s\n\n%s" % (name, WANT), [name], check=False, env=env)

    def test_owner_file_lives_in_the_per_worktree_gitdir(self):
        self.claim()
        text = (self.repo.root / ".git" / "bezel-owner").read_text()
        self.assertIn("owner: coord-a", text)
        self.assertIn("branch: main", text)
        wt = self.repo.root.parent / "wt2"
        self.repo.git("worktree", "add", "-q", "-b", "other", str(wt))
        r = run("wt_owner.sh", "show", cwd=wt, env=self.env)
        self.assertEqual(r.returncode, 1, "a linked worktree must not see main's binding")

    def test_commit_on_the_claimed_branch_is_allowed(self):
        self.claim()
        r = self.try_commit("a")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_commit_after_switching_branch_is_refused(self):
        self.claim()
        sw = self.repo.git("switch", "-c", "intruder")
        self.assertIn("OWNED by 'coord-a'", sw.stderr)  # post-checkout warning
        r = self.try_commit("b")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("REFUSED", r.stderr)
        self.assertIn("'coord-a'", r.stderr)
        self.assertIn("'main'", r.stderr)
        self.assertIn("'intruder'", r.stderr)

    def test_amend_after_switching_branch_is_refused(self):
        self.claim()
        self.repo.git("switch", "-q", "-c", "intruder")
        r = self.repo.git("commit", "--amend", "-q", "-m", "x\n\n" + WANT, check=False)
        self.assertNotEqual(r.returncode, 0)

    def test_override_allows_the_commit(self):
        self.claim()
        self.repo.git("switch", "-q", "-c", "intruder")
        r = self.try_commit("c", env=dict(self.env, BEZEL_OWNER_OVERRIDE="1"))
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_no_owner_file_is_a_noop(self):
        self.repo.git("switch", "-q", "-c", "anything")
        r = self.try_commit("d")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_release_removes_the_binding(self):
        self.claim()
        run("wt_owner.sh", "release", cwd=self.repo.root, env=self.env)
        self.repo.git("switch", "-q", "-c", "free")
        self.assertEqual(self.try_commit("e").returncode, 0)

    def test_pick_stage_works_in_a_worktree_owned_for_its_branch(self):
        self.repo.git("switch", "-q", "-c", "lane")
        self.repo.write("f", "f\n")
        self.repo.commit("fix #4300\n\n" + WANT, ["f"])
        lane_tip = self.repo.out("rev-parse", "HEAD")
        self.repo.git("switch", "-q", "-c", "staging", "main")
        self.claim()
        r = run("pick_stage.sh", "--branch", "staging", "--pool", "p", "--lane", "lane",
                "--reviewer", "r", "--verdict", "ACCEPT", "--kind", "fix", lane_tip,
                cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr + r.stdout)

    def test_install_is_idempotent(self):
        before = (self.hooks / "pre-commit").read_text()
        r = run("install_hooks.sh", cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("already current", r.stdout)
        self.assertEqual((self.hooks / "pre-commit").read_text(), before)

    def test_install_refuses_to_clobber_a_foreign_hook(self):
        run("install_hooks.sh", "--uninstall", cwd=self.repo.root, env=self.env)
        foreign = "#!/bin/sh\necho mine\n"
        (self.hooks / "post-checkout").write_text(foreign)
        r = run("install_hooks.sh", cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 1)
        self.assertIn("REFUSING", r.stderr)
        self.assertEqual((self.hooks / "post-checkout").read_text(), foreign)
        self.assertFalse((self.hooks / "pre-commit").exists(),
                         "a refusal must not leave a half-installed set")

    def test_wrapper_is_a_noop_on_a_branch_without_the_hook_scripts(self):
        self.claim()
        # An old branch that predates scripts/hooks/: the wrapper must do nothing.
        self.repo.git("switch", "-q", "--orphan", "old")
        r = self.try_commit("g")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_lane_setup_owner_flag_claims_and_default_does_not(self):
        # A fast root that is not a mount point: lane_setup leaves build/ alone
        # and touches nothing outside the temp dir.
        env = dict(self.env, BEZEL_LANE_BUILD_ROOT=self._tmp.name + "/nofast/lane-builds",
                   BEZEL_AOT_FAST_ROOT=self._tmp.name + "/nofast")
        owner = self.repo.root / ".git" / "bezel-owner"
        r = run("lane_setup.sh", cwd=self.repo.root, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(owner.exists(), "lane_setup without --owner must not claim")
        r = run("lane_setup.sh", "--owner", "lane-x", cwd=self.repo.root, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("owner: lane-x", owner.read_text())

    def test_hooks_are_tracked_in_this_checkout(self):
        for h in ("pre-commit", "post-checkout"):
            self.assertTrue((SCRIPTS / "hooks" / h).is_file())


if __name__ == "__main__":
    unittest.main()
