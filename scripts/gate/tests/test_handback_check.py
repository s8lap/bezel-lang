"""scripts/handback_check.sh, against throwaway repos (read-only by design)."""
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


if __name__ == "__main__":
    unittest.main()
