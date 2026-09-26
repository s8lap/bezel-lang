"""scripts/pick_stage.sh and scripts/pick_ledger.sh, against throwaway repos.

pick_stage records who authored and reviewed each picked commit as a note under
refs/notes/bezel-picks; pick_ledger reads the notes back as the batch ledger.
"""
import tempfile
import unittest

from orch_tools_support import WANT, Repo, git_env, run


class _Base(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="pick-stage-")
        tmp = self._tmp.name
        self.env = git_env(tmp)
        self.repo = Repo(tmp + "/repo", self.env)
        # A lane branch with two well-formed commits, and a staging branch.
        self.repo.git("switch", "-q", "-c", "lane-a")
        self.c1 = self.repo.change("a.txt", "fix #4001: first\n\nbody cites #4002\n\n" + WANT)
        self.c2 = self.repo.change("b.txt", "fix #4003: second\n\n" + WANT)
        self.repo.git("switch", "-q", "-c", "staging", "main")

    def tearDown(self):
        self._tmp.cleanup()

    def pick(self, *shas, **kw):
        args = ["--branch", kw.get("branch", "staging"), "--pool", kw.get("pool", "p1"),
                "--lane", kw.get("lane", "lane-a"), "--reviewer", "rev-1",
                "--verdict", kw.get("verdict", "ACCEPT")] + list(shas)
        return run("pick_stage.sh", *args, cwd=self.repo.root, env=self.env)

    def note(self, rev="HEAD"):
        return self.repo.out("notes", "--ref=bezel-picks", "show", rev)


class PickStageTest(_Base):
    def test_pick_writes_attribution_note_and_strips_x_line(self):
        r = self.pick(self.c1)
        self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
        note = self.note()
        for line in ("pool: p1", "lane: lane-a", "orig: " + self.c1, "reviewer: rev-1",
                     "verdict: ACCEPT", "issues: #4001\n"):
            self.assertIn(line, note + "\n")
        # #4002 appears only in the body: a cross-reference, not a fix.
        self.assertNotIn("#4002", note)
        self.assertIn("picked-at: ", note)
        msg = self.repo.out("log", "-1", "--format=%B")
        self.assertNotIn("cherry picked from", msg)
        self.assertEqual(msg.splitlines()[-1], WANT)
        self.assertIn("trailers=1", r.stdout)

    def test_wrong_branch_is_refused(self):
        r = self.pick(self.c1, branch="other")
        self.assertEqual(r.returncode, 64)
        self.assertIn("expected 'other'", r.stderr)
        self.assertEqual(self.repo.out("rev-list", "--count", "main..staging"), "0")

    def test_missing_flag_is_refused(self):
        r = run("pick_stage.sh", "--branch", "staging", "--pool", "p1", "--lane", "l",
                "--verdict", "ACCEPT", self.c1, cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 64)
        self.assertIn("--reviewer is required", r.stderr)

    def test_bad_verdict_is_refused(self):
        r = self.pick(self.c1, verdict="LGTM")
        self.assertEqual(r.returncode, 64)
        self.assertIn("--verdict must be", r.stderr)

    def test_two_trailer_commit_fails_with_exit_3_and_no_note(self):
        self.repo.git("switch", "-q", "lane-a")
        bad = self.repo.change("c.txt", "fix #4004: bad\n\n" + WANT + "\nClaude-Session: https://x")
        self.repo.git("switch", "-q", "staging")
        r = self.pick(bad)
        self.assertEqual(r.returncode, 3, r.stderr + r.stdout)
        self.assertIn("TRAILER VIOLATION", r.stderr)
        # The commit is left in place for a human, without a note.
        self.assertEqual(self.repo.out("rev-list", "--count", "main..staging"), "1")
        self.assertNotEqual(self.repo.git("notes", "--ref=bezel-picks", "show", "HEAD",
                                          check=False).returncode, 0)


class PickLedgerTest(_Base):
    def test_counts_earliest_pool_credit_and_unattributed(self):
        self.assertEqual(self.pick(self.c1, pool="p1").returncode, 0)
        # Pool p2 follows up on #4001 (already credited to p1) and fixes #4003.
        self.repo.git("switch", "-q", "lane-a")
        c3 = self.repo.change("d.txt", "follow-up #4001\n\n" + WANT)
        self.repo.git("switch", "-q", "staging")
        self.assertEqual(self.pick(c3, self.c2, pool="p2", lane="lane-b").returncode, 0)
        # One commit made by hand, with no note.
        self.repo.change("e.txt", "manual #4005\n\n" + WANT)

        r = run("pick_ledger.sh", "main..HEAD", cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        rows = {ln.split()[0]: ln.split() for ln in r.stdout.splitlines() if ln.strip()}
        self.assertEqual(rows["p1"][1:], ["1", "1"])            # #4001 (not body #4002)
        self.assertEqual(rows["p2"][1:], ["2", "1"])            # #4003 only
        self.assertEqual(rows["UNATTRIBUTED"][1:], ["1", "1"])  # #4005
        self.assertIn("distinct issues: 3", r.stdout)
        self.assertIn("UNATTRIBUTED: 1 commit(s)", r.stdout)

    def test_all_attributed_range_reports_zero_unattributed(self):
        self.assertEqual(self.pick(self.c1, self.c2).returncode, 0)
        r = run("pick_ledger.sh", "main..staging", cwd=self.repo.root, env=self.env)
        self.assertIn("UNATTRIBUTED: 0", r.stdout)
        self.assertIn("distinct issues: 2", r.stdout)  # #4001 #4003; body #4002 not counted



class SubjectOnlyIssueRuleTest(_Base):
    """scripts/pick_issues.sh: a commit's issues are the #NNN in its SUBJECT."""

    def ledger(self):
        r = run("pick_ledger.sh", "main..HEAD", cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def test_two_issue_subjects_count_both(self):
        self.repo.change("f", "fix #4807/#4808: slash pair\n\n" + WANT)
        self.repo.change("g", "fix #5023+#3904: plus pair\n\n" + WANT)
        self.assertIn("distinct issues: 4", self.ledger())

    def test_body_only_cross_reference_does_not_count(self):
        self.repo.change("h", "bank conformance rows\n\nRegressed by #4501, see #3243.\n\n" + WANT)
        out = self.ledger()
        self.assertIn("distinct issues: 0", out)
        self.assertNotIn("#4501", out)

    def test_note_issues_field_is_subject_only_for_a_two_issue_subject(self):
        self.repo.git("switch", "-q", "lane-a")
        c = self.repo.change("i", "fix #4807/#4808: pair\n\nAlso touches #3243.\n\n" + WANT)
        self.repo.git("switch", "-q", "staging")
        self.assertEqual(self.pick(c).returncode, 0)
        self.assertIn("issues: #4807 #4808\n", self.note() + "\n")

    def test_script_reads_a_rev_or_stdin(self):
        import subprocess
        from orch_tools_support import SCRIPTS
        r = subprocess.run([str(SCRIPTS / "pick_issues.sh")], input="fix #1234 and #1234+#77\nbody #9999\n",
                           capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "#1234")  # dedup; #77 is too short; body ignored


if __name__ == "__main__":
    unittest.main()
