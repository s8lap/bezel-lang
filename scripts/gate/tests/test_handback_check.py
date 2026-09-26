"""scripts/handback_check.sh, against throwaway repos (read-only by design)."""
import pathlib
import re
import tempfile
import unittest

from orch_tools_support import WANT, Repo, git_env, run


class HandbackCheckTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="handback-")
        self.env = git_env(self._tmp.name)
        self.repo = Repo(self._tmp.name + "/repo", self.env)
        self.repo.git("switch", "-q", "-c", "lane")
        self.repo.change("src/sema/a.cpp", "fix #4100: a\n\n" + WANT)
        self.tip = self.repo.change("tests/sema/a_4100_test.cpp",
                                    "test #4100\n\nPins tests/sema/a_4100_test.cpp.\n\n" + WANT,
                                    text="TEST(SemaA, rejects_thing_4100) {}\n")

    def tearDown(self):
        self._tmp.cleanup()

    def check(self, *args, branch="lane"):
        return run("handback_check.sh", branch, "--base", "main",
                   "--repo", str(self.repo.root), *args, cwd=self.repo.root, env=self.env)

    def row(self, out, name):
        m = re.search(r"^(PASS|WARN|FAIL|SKIP)\s+%s\b.*$" % re.escape(name), out, re.M)
        self.assertIsNotNone(m, "no %s row in:\n%s" % (name, out))
        return m.group(0)

    def test_clean_branch_passes(self):
        r = self.check("--reported-sha", self.tip, "--grant", "src/sema/*,tests/sema/*")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("VERDICT: PASS", r.stdout)
        for name in ("branch", "reported-sha", "base", "commits", "trailers",
                     "author", "grant", "scripts", "named-tests"):
            self.assertTrue(self.row(r.stdout, name).startswith("PASS"), self.row(r.stdout, name))

    def test_reported_sha_mismatch_fails_and_detects_dangling(self):
        # Amend the tip: the old sha is then in no branch.
        old = self.tip
        self.repo.git("commit", "-q", "--amend", "-m", "test #4100 amended\n\n" + WANT)
        r = self.check("--reported-sha", old)
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAIL", self.row(r.stdout, "reported-sha"))
        self.assertIn("DANGLING", self.row(r.stdout, "reported-sha"))

    def test_reported_sha_inside_branch_is_named(self):
        r = self.check("--reported-sha", self.repo.out("rev-parse", "lane~1"))
        self.assertEqual(r.returncode, 1)
        self.assertIn("IN the branch, 1 commit(s) behind", self.row(r.stdout, "reported-sha"))

    def test_base_not_ancestor_fails(self):
        self.repo.git("switch", "-q", "main")
        self.repo.change("other.txt", "main moved\n\n" + WANT)
        r = self.check()
        self.assertEqual(r.returncode, 1)
        base = self.row(r.stdout, "base")
        self.assertTrue(base.startswith("FAIL"), base)
        self.assertIn("behind base by 1", base)

    def test_bad_trailer_fails(self):
        self.repo.change("src/sema/b.cpp", "fix #4101: b\n\nCo-Authored-By: Claude Sonnet 5 <x>")
        r = self.check()
        self.assertEqual(r.returncode, 1)
        self.assertTrue(self.row(r.stdout, "trailers").startswith("FAIL"))

    def test_outside_grant_warns(self):
        self.repo.change("src/codegen/x.cpp", "fix #4102: x\n\n" + WANT)
        r = self.check("--grant", "src/sema/*,tests/sema/*")
        self.assertEqual(r.returncode, 0, r.stdout)
        grant = self.row(r.stdout, "grant")
        self.assertTrue(grant.startswith("WARN"), grant)
        self.assertIn("src/codegen/x.cpp", r.stdout)

    def test_claude_author_is_a_config_leak(self):
        leak = dict(self.env, GIT_AUTHOR_NAME="Claude Opus 5",
                    GIT_AUTHOR_EMAIL="noreply@anthropic.com")
        self.repo.write("src/sema/c.cpp", "c\n")
        self.repo.commit("fix #4103: c\n\n" + WANT, ["src/sema/c.cpp"], env=leak)
        r = self.check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("git-config leak", self.row(r.stdout, "author"))

    def test_hardcoded_home_path_in_new_script_fails(self):
        self.repo.change("scripts/x.sh", "add x\n\n" + WANT,
                         text="#!/bin/sh\nREPO=/home/someone/checkout\n")
        r = self.check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("scripts/x.sh", self.row(r.stdout, "scripts"))

    def test_named_test_missing_at_tip_warns(self):
        self.repo.change("src/sema/d.cpp", "fix #4104\n\nFlips SemaA.gone_row_4104.\n\n" + WANT)
        r = self.check()
        self.assertIn("SemaA.gone_row_4104", self.row(r.stdout, "named-tests"))
        self.assertTrue(self.row(r.stdout, "named-tests").startswith("WARN"))

    def test_missing_branch_fails(self):
        r = self.check(branch="no-such-branch")
        self.assertEqual(r.returncode, 1)
        self.assertIn("VERDICT: FAIL", r.stdout)


    def test_new_hook_file_with_home_path_fails(self):
        self.repo.change("scripts/hooks/pre-push", "add hook\n\n" + WANT,
                         text="#!/bin/sh\nexec /home/someone/checkout/x\n")
        r = self.check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("scripts/hooks/pre-push", self.row(r.stdout, "scripts"))

    def test_base_defaults_to_bezel_staging_ref(self):
        # The temp repo has no origin/main: only the env default can make base PASS.
        env = dict(self.env, BEZEL_STAGING_REF="main")
        r = run("handback_check.sh", "lane", "--repo", str(self.repo.root),
                cwd=self.repo.root, env=env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("main (", self.row(r.stdout, "base"))
        self.assertTrue(self.row(r.stdout, "base").startswith("PASS"))

    def test_value_flag_with_no_value_exits_64_instead_of_looping(self):
        import subprocess
        from orch_tools_support import SCRIPTS
        for flag in ("--base", "--reported-sha", "--grant", "--author", "--repo"):
            r = subprocess.run([str(SCRIPTS / "handback_check.sh"), "lane", flag],
                               cwd=str(self.repo.root), env=self.env,
                               capture_output=True, text=True, timeout=20)
            self.assertEqual(r.returncode, 64, flag)
            self.assertIn("needs a value", r.stderr)

    def _snapshot(self, root):
        gd = pathlib.Path(self.repo.out("-C", str(root), "rev-parse", "--absolute-git-dir"))
        return {
            "refs": self.repo.out("-C", str(root), "for-each-ref", "--format=%(refname) %(objectname)"),
            "head": (gd / "HEAD").read_text(),
            "index": (gd / "index").read_bytes() if (gd / "index").exists() else b"",
            "status": self.repo.out("-C", str(root), "status", "--porcelain", "--untracked-files=all"),
            "worktrees": self.repo.out("worktree", "list", "--porcelain"),
        }

    def test_is_read_only(self):
        # The lane branch is checked out in its own worktree with an untracked
        # script, as in real use; the check runs from the main checkout.
        self.repo.git("switch", "-q", "main")
        wt = self.repo.root.parent / "lane-wt"
        self.repo.git("worktree", "add", "-q", str(wt), "lane")
        (wt / "scripts").mkdir()
        (wt / "scripts" / "u.sh").write_text("#!/bin/sh\n")
        before = [self._snapshot(self.repo.root), self._snapshot(wt)]
        r = self.check("--reported-sha", self.tip, "--grant", "src/*")
        self.assertIn("VERDICT:", r.stdout)
        self.assertEqual([self._snapshot(self.repo.root), self._snapshot(wt)], before)

if __name__ == "__main__":
    unittest.main()
