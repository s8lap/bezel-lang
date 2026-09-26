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
                "--lane", kw.get("lane", "lane-a"), "--reviewer", kw.get("reviewer", "rev-1"),
                "--verdict", kw.get("verdict", "ACCEPT"),
                "--kind", kw.get("kind", "fix")] + list(shas)
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
        # "manual #4005" has no note and does not look like a fix: 0 fix issues.
        self.assertEqual(rows["UNATTRIBUTED"][1:], ["1", "0"])
        self.assertIn("distinct fix issues: 2 (0 from inferred kinds)", r.stdout)
        self.assertIn("distinct issues named (all kinds): 3", r.stdout)
        self.assertIn("UNATTRIBUTED: 1 commit(s)", r.stdout)

    def test_all_attributed_range_reports_zero_unattributed(self):
        self.assertEqual(self.pick(self.c1, self.c2).returncode, 0)
        r = run("pick_ledger.sh", "main..staging", cwd=self.repo.root, env=self.env)
        self.assertIn("UNATTRIBUTED: 0", r.stdout)
        self.assertIn("distinct fix issues: 2 ", r.stdout)  # #4001 #4003; body #4002 not counted



class SubjectOnlyIssueRuleTest(_Base):
    """scripts/pick_issues.sh: a commit's issues are the #NNN in its SUBJECT."""

    def ledger(self):
        r = run("pick_ledger.sh", "main..HEAD", cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def test_two_issue_subjects_count_both(self):
        self.repo.change("f", "fix #4807/#4808: slash pair\n\n" + WANT)
        self.repo.change("g", "fix #5023+#3904: plus pair\n\n" + WANT)
        self.assertIn("distinct fix issues: 4 ", self.ledger())

    def test_body_only_cross_reference_does_not_count(self):
        self.repo.change("h", "bank conformance rows\n\nRegressed by #4501, see #3243.\n\n" + WANT)
        out = self.ledger()
        self.assertIn("distinct fix issues: 0 ", out)
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



class KindTest(_Base):
    """--kind is recorded by pick_stage; only kind=fix commits count as banked."""

    def ledger(self):
        r = run("pick_ledger.sh", "main..HEAD", cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def test_kind_is_required(self):
        r = run("pick_stage.sh", "--branch", "staging", "--pool", "p", "--lane", "l",
                "--reviewer", "r", "--verdict", "ACCEPT", self.c1,
                cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 64)
        self.assertIn("--kind is required", r.stderr)

    def test_unknown_kind_is_refused(self):
        r = self.pick(self.c1, kind="feature")
        self.assertEqual(r.returncode, 64)
        self.assertIn("--kind must be", r.stderr)
        self.assertEqual(self.repo.out("rev-list", "--count", "main..staging"), "0")

    def test_kind_is_written_into_the_note(self):
        self.assertEqual(self.pick(self.c1, kind="infra").returncode, 0)
        self.assertIn("kind: infra", self.note())

    def test_recorded_test_kind_is_not_banked_despite_a_fix_subject(self):
        # c1's subject is "fix #4001: first", but the reviewer recorded kind=test.
        self.assertEqual(self.pick(self.c1, kind="test").returncode, 0)
        out = self.ledger()
        self.assertIn("distinct fix issues: 0 ", out)
        self.assertIn("distinct issues named (all kinds): 1", out)
        self.assertRegex(out, r"\btest +#4001\b")      # recorded: no `*`

    def test_fallback_fix_counts_and_bank_subject_does_not(self):
        self.repo.change("j", "sema: reject Object into record targets (#4500)\n\n" + WANT)
        self.repo.change("k", "Bank internal_residual rise 81 -> 83 from #4501\n\n" + WANT)
        self.repo.change("l", "bank conformance row now passing (#4502)\n\n" + WANT)
        out = self.ledger()
        self.assertIn("distinct fix issues: 1 (1 from inferred kinds)", out)
        self.assertRegex(out, r"fix\* +#4500")
        self.assertRegex(out, r"other\* +#4501")
        self.assertRegex(out, r"other\* +#4502")


class NoteRecoveryTest(_Base):
    """Review round 4, BLOCKER 2: a staging rebase without notes.rewriteRef
    leaves every picked commit without a note by SHA; the ledger recovers the
    note by patch-id and marks the row `~`."""

    def test_ledger_survives_a_rebase_without_rewrite_ref(self):
        self.assertEqual(self.pick(self.c1, kind="fix").returncode, 0)
        self.assertEqual(self.pick(self.c2, kind="test", pool="p2").returncode, 0)
        self.repo.git("switch", "-q", "main")
        self.repo.change("z.txt", "unrelated on main\n\n" + WANT)
        self.repo.git("switch", "-q", "staging")
        self.repo.git("rebase", "-q", "main", env=dict(self.env, GIT_EDITOR="true"))
        self.assertNotEqual(self.repo.git("notes", "--ref=bezel-picks", "show", "HEAD",
                                          check=False).returncode, 0,
                            "precondition: the rebase dropped the note by SHA")
        r = run("pick_ledger.sh", "main..staging", cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stdout, r"\bp1 +lane-a +ACCEPT +fix~ +#4001\b")
        self.assertRegex(r.stdout, r"\bp2 +lane-a +ACCEPT +test~ +#4003\b")
        # #4003's subject says "fix", but the RECOVERED kind is test: 1 banked, not 2.
        self.assertIn("distinct fix issues: 1 (0 from inferred kinds)", r.stdout)
        self.assertIn("2 row(s)", r.stdout)
        self.assertIn("UNATTRIBUTED: 0", r.stdout)

    def test_commit_with_no_note_by_sha_or_patch_id_is_unattributed(self):
        self.repo.change("m.txt", "manual #4600\n\n" + WANT)
        r = run("pick_ledger.sh", "main..staging", cwd=self.repo.root, env=self.env)
        self.assertIn("UNATTRIBUTED: 1 commit(s): no refs/notes/bezel-picks note found "
                      "by SHA or patch-id", r.stdout)


class DuplicatePatchIdTest(_Base):
    """Re-review NEW-1/NEW-2/NEW-4.1: identical diffs under different subjects
    (a fix, its revert, a re-add) must never borrow each other's note; a pruned
    note target must not disable recovery; a changed patch stays UNATTRIBUTED."""

    ROW = "row\n"

    def _lane_commit(self, subject, text):
        self.repo.git("switch", "-q", "lane-d")
        self.repo.write("f.txt", text)
        self.repo.commit(subject + "\n\n" + WANT, ["f.txt"])
        sha = self.repo.out("rev-parse", "HEAD")
        self.repo.git("switch", "-q", "staging")
        return sha

    def _start(self):
        # f.txt exists (empty) on staging; lane-d branches from there.
        self.repo.write("f.txt", "")
        self.repo.commit("empty f\n\n" + WANT, ["f.txt"])
        self.repo.git("branch", "lane-d")

    def _rebase_staging(self):
        self.repo.git("switch", "-q", "main")
        self.repo.change("z.txt", "unrelated on main\n\n" + WANT)
        self.repo.git("switch", "-q", "staging")
        self.repo.git("rebase", "-q", "main", env=dict(self.env, GIT_EDITOR="true"))

    def ledger(self):
        r = run("pick_ledger.sh", "main..staging", cwd=self.repo.root, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def row(self, out, subject):
        for ln in out.splitlines():
            if ln.endswith(subject):
                return ln.split()
        self.fail("no row for %r in:\n%s" % (subject, out))

    def _scenario(self, order):
        # f.txt: "" -> row (A or B), -> "" (revert), -> row (B or A): A and B
        # have IDENTICAL diffs, so identical patch-ids.
        self._start()
        spec = {"A": ("fix #5001: add row", "p1", "fix"),
                "B": ("Bank baseline row (#5002)", "p2", "bank")}
        first, second = order
        s1 = self._lane_commit(spec[first][0], self.ROW)
        self.assertEqual(self.pick(s1, pool=spec[first][1], kind=spec[first][2]).returncode, 0)
        rv = self._lane_commit("infra: revert row", "")
        self.assertEqual(self.pick(rv, pool="p3", kind="infra").returncode, 0)
        s2 = self._lane_commit(spec[second][0], self.ROW)
        self.assertEqual(self.pick(s2, pool=spec[second][1], kind=spec[second][2]).returncode, 0)
        self._rebase_staging()
        out = self.ledger()
        self.assertEqual(self.row(out, "fix #5001: add row")[1:5], ["p1", "lane-a", "ACCEPT", "fix~"])
        self.assertEqual(self.row(out, "Bank baseline row (#5002)")[1:5],
                         ["p2", "lane-a", "ACCEPT", "bank~"])
        self.assertEqual(self.row(out, "infra: revert row")[1:5], ["p3", "lane-a", "ACCEPT", "infra~"])
        self.assertIn("distinct fix issues: 1 (0 from inferred kinds)", out)
        return out

    def test_fix_then_bank_with_identical_diffs_keep_their_own_notes(self):
        self._scenario("AB")

    def test_bank_then_fix_with_identical_diffs_keep_their_own_notes(self):
        self._scenario("BA")

    def test_duplicate_diff_under_a_new_subject_is_ambiguous_not_borrowed(self):
        self._scenario("AB")
        # A hand-made commit with the same diff again, no note: it shares the
        # patch-id of A and B but neither subject.
        self.repo.write("f.txt", "")
        self.repo.commit("manual clear\n\n" + WANT, ["f.txt"])
        self.repo.write("f.txt", self.ROW)
        self.repo.commit("manual re-add of row\n\n" + WANT, ["f.txt"])
        out = self.ledger()
        r = self.row(out, "manual re-add of row")
        self.assertEqual(r[1:3], ["UNATTRIBUTED", "(ambiguous)"])
        self.assertTrue(r[4].endswith("*"), r)
        # "manual clear" shares the revert's patch-id but not its subject.
        self.assertEqual(self.row(out, "manual clear")[1:3], ["UNATTRIBUTED", "(ambiguous)"])
        # UNATTRIBUTED: the two manual commits plus staging's own "empty f".
        self.assertRegex(out, r"UNATTRIBUTED: 3 commit\(s\).*\(2 ambiguous")

    def test_same_subject_and_diff_twice_is_ambiguous(self):
        # The same fix picked twice (after a revert) under ONE subject: two
        # candidates match the subject, so neither rebased row may guess.
        self._start()
        a1 = self._lane_commit("fix #5001: add row", self.ROW)
        self.assertEqual(self.pick(a1, pool="p1").returncode, 0)
        rv = self._lane_commit("infra: revert row", "")
        self.assertEqual(self.pick(rv, pool="p3", kind="infra").returncode, 0)
        a2 = self._lane_commit("fix #5001: add row", self.ROW)
        self.assertEqual(self.pick(a2, pool="p9").returncode, 0)
        self._rebase_staging()
        out = self.ledger()
        rows = [ln.split() for ln in out.splitlines() if ln.endswith("fix #5001: add row")]
        self.assertEqual(len(rows), 2)
        for r in rows:
            self.assertEqual(r[1:3], ["UNATTRIBUTED", "(ambiguous)"])

    def test_a_pruned_note_target_does_not_disable_recovery(self):
        self.assertEqual(self.pick(self.c1).returncode, 0)
        # Note a commit object that nothing references, then prune it: its note
        # survives in refs/notes/bezel-picks, its object does not.
        tree = self.repo.out("rev-parse", "HEAD^{tree}")
        ghost = self.repo.out("commit-tree", tree, "-p", "HEAD", "-m", "ghost")
        self.repo.git("notes", "--ref=bezel-picks", "add", "-m", "pool: zz", ghost)
        self.repo.git("prune", "--expire=now")
        self.assertNotEqual(self.repo.git("cat-file", "-e", ghost, check=False).returncode, 0,
                            "precondition: the ghost commit was pruned")
        self._rebase_staging()
        out = self.ledger()
        self.assertEqual(self.row(out, "fix #4001: first")[1:5], ["p1", "lane-a", "ACCEPT", "fix~"])
        self.assertIn("UNATTRIBUTED: 0", out)

    def test_a_patch_changed_by_the_rebase_stays_unattributed(self):
        self.assertEqual(self.pick(self.c1).returncode, 0)     # adds a.txt
        self.repo.git("switch", "-q", "main")
        self.repo.change("a.txt", "main a\n\n" + WANT, text="main\n")
        self.repo.git("switch", "-q", "staging")
        env = dict(self.env, GIT_EDITOR="true")
        self.assertNotEqual(self.repo.git("rebase", "main", check=False, env=env).returncode, 0)
        self.repo.write("a.txt", "resolved\n")
        self.repo.git("add", "--", "a.txt")
        self.repo.git("rebase", "--continue", env=env)
        out = self.ledger()
        r = self.row(out, "fix #4001: first")
        self.assertEqual(r[1:3], ["UNATTRIBUTED", "-"])
        self.assertEqual(r[4], "fix*")


class PickFlowTest(_Base):
    """Review round 4, SHOULD-FIX 3-5: conflict + --continue, empty picks,
    dirty-tree refusal, trailer-violation advice, value flags at the end."""

    def conflict_setup(self):
        # staging already has a different a.txt, so picking c1 (adds a.txt) conflicts.
        self.repo.change("a.txt", "staging a\n\n" + WANT, text="staging\n")

    def test_conflict_prints_recipe_and_remaining_shas(self):
        self.conflict_setup()
        r = self.pick(self.c1, self.c2)
        self.assertEqual(r.returncode, 2, r.stderr + r.stdout)
        self.assertIn("CONFLICT picking " + self.c1[:10], r.stderr)
        self.assertIn("Not yet picked: " + self.c2, r.stderr)
        self.assertRegex(r.stderr, r"pick_stage\.sh --continue --branch staging --pool p1 "
                         r"--lane lane-a --reviewer rev-1 --verdict ACCEPT --kind fix " + self.c2)

    def _resolve(self):
        self.repo.write("a.txt", "resolved\n")
        self.repo.git("add", "--", "a.txt")

    def _assert_finalized(self, rev, orig):
        msg = self.repo.out("log", "-1", "--format=%B", rev)
        self.assertNotIn("cherry picked from", msg)
        self.assertEqual(msg.splitlines()[-1], WANT)
        self.assertIn("orig: " + orig, self.note(rev))

    def test_printed_continue_recipe_is_shell_quoted_and_runs(self):
        import re
        import subprocess
        self.conflict_setup()
        r = self.pick(self.c1, self.c2, reviewer="rev one")
        self.assertEqual(r.returncode, 2, r.stderr)
        m = re.search(r"^\s*(\S*pick_stage\.sh --continue .*)$", r.stderr, re.M)
        self.assertIsNotNone(m, r.stderr)
        self.assertIn("--reviewer rev\\ one", m.group(1))
        self._resolve()
        c = subprocess.run(["bash", "-c", m.group(1)], cwd=str(self.repo.root), env=self.env,
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(c.returncode, 0, c.stderr + c.stdout)
        self.assertIn("reviewer: rev one\n", self.note("HEAD~1"))
        self.assertIn("orig: " + self.c2, self.note("HEAD"))

    def test_continue_finalizes_the_conflicted_pick_and_picks_the_rest(self):
        self.conflict_setup()
        self.assertEqual(self.pick(self.c1, self.c2).returncode, 2)
        self._resolve()
        r = self.pick("--continue", self.c2)
        self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
        self._assert_finalized("HEAD~1", self.c1)
        self._assert_finalized("HEAD", self.c2)
        self.assertIn("2 commit(s) picked", r.stdout)

    def test_continue_after_a_manual_cherry_pick_continue(self):
        self.conflict_setup()
        self.assertEqual(self.pick(self.c1).returncode, 2)
        self._resolve()
        self.repo.git("-c", "core.editor=true", "cherry-pick", "--continue")
        r = self.pick("--continue")
        self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
        self._assert_finalized("HEAD", self.c1)

    def test_continue_with_nothing_to_continue_is_refused(self):
        r = self.pick("--continue")
        self.assertEqual(r.returncode, 64)
        self.assertIn("nothing to continue", r.stderr)

    def test_a_new_run_during_an_unfinished_pick_is_refused(self):
        self.conflict_setup()
        self.assertEqual(self.pick(self.c1).returncode, 2)
        r = self.pick(self.c2)
        self.assertEqual(r.returncode, 64)
        self.assertIn("cherry-pick is in progress", r.stderr)

    def test_empty_pick_is_skipped_not_a_conflict(self):
        self.assertEqual(self.pick(self.c1).returncode, 0)
        head = self.repo.out("rev-parse", "HEAD")
        r = self.pick(self.c1, self.c2)
        self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
        self.assertIn(self.c1[:10] + " already applied, skipped", r.stdout)
        self.assertNotIn("CONFLICT", r.stderr)
        self.assertEqual(self.repo.out("rev-parse", "HEAD~1"), head)
        self.assertIn("orig: " + self.c2, self.note())
        self.assertNotEqual(self.repo.git("rev-parse", "-q", "--verify", "CHERRY_PICK_HEAD",
                                          check=False).returncode, 0)

    def test_dirty_tree_is_refused(self):
        self.repo.write("README", "dirty\n")
        r = self.pick(self.c1)
        self.assertEqual(r.returncode, 64)
        self.assertIn("dirty", r.stderr)
        self.assertEqual(self.repo.out("rev-list", "--count", "main..staging"), "0")

    def test_trailer_violation_advice_names_the_lane_branch(self):
        self.repo.git("switch", "-q", "lane-a")
        bad = self.repo.change("c.txt", "fix #4004: bad\n\nno trailer")
        self.repo.git("switch", "-q", "staging")
        r = self.pick(bad)
        self.assertEqual(r.returncode, 3)
        self.assertIn("ON THE LANE BRANCH", r.stderr)
        self.assertIn("git reset --hard HEAD~1", r.stderr)

    def test_value_flag_with_no_value_errors_instead_of_looping(self):
        import subprocess
        from orch_tools_support import SCRIPTS
        for argv in (["--kind"], ["--branch", "staging", "--pool"],
                     ["--branch", "staging", "--kind", "--pool", "p"]):
            r = subprocess.run([str(SCRIPTS / "pick_stage.sh")] + argv, cwd=str(self.repo.root),
                               env=self.env, capture_output=True, text=True, timeout=20)
            self.assertEqual(r.returncode, 64, argv)
            self.assertIn("needs a value", r.stderr)


class InferKindTest(unittest.TestCase):
    CASES = [
        ("fix #4807/#4808: stop batch checks", "fix"),
        ("Fix-forward #4981: gate the retain", "fix"),
        ("narrow #4977: gate the bare-Task reject", "fix"),
        ("close #5014: not a vacuous-pass hole", "fix"),
        ("sema: narrow #4833: consolidate lists", "fix"),
        ("codegen test: fix #1234 row", "fix"),
        ("sema: reject overloaded new T (#4996)", "fix"),
        ("regression pin for #4995 (fixed by #4825 0526620e)", "other"),
        ("test #5035 T11: make the fixture conform", "other"),
        ("lit #5032: pin the two new diagnostics (#5032)", "other"),
        ("Bank internal_residual drop 82 -> 81 from #5035", "other"),
        ("fixes a typo in the runbook", "other"),
        ("jobserver: add a dry-run seam", "other"),
    ]

    def test_cases(self):
        import subprocess
        from orch_tools_support import SCRIPTS
        for subj, want in self.CASES:
            r = subprocess.run([str(SCRIPTS / "pick_issues.sh"), "--infer-kind"],
                               input=subj + "\n\nbody\n", capture_output=True, text=True)
            self.assertEqual(r.stdout.strip(), want, subj)


if __name__ == "__main__":
    unittest.main()
