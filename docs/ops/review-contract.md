# Review contract: the standing rules for a non-author reviewer

*Written 2026-10-04. Every fix gets a non-author review before it is picked (`CLAUDE.md`). Until
now the checks a review must run lived in each coordinator's memory and reached a reviewer only
through its brief, so a brief that left one out produced a review that missed it. A reviewer
brief now names the work, and this file holds the checks. The incident behind each check is in
[`coordinator-runbook.md`](coordinator-runbook.md), *Review and dispatch: where the contract rules
came from*.*

**Authority:** `CLAUDE.md` wins over this file. Where they disagree, the disagreement is a defect to
fix in whichever is stale.

## Who you are

You are a **reviewer**: a fresh agent, dispatched by a coordinator after a producer's handback
passed `scripts/handback_check.sh`. You are never the author of the work, and the coordinator who
dispatched you is not a reviewer either. A producer's handback describes its own work. Your job is
to re-measure that work with your own instruments, and your report is the evidence the integrator
picks on.

Your brief names: the branch, the SHA(s), the base (the staging tip), the worktree to work in,
and the issue. Read `CLAUDE.md` (*Test and oracle rules*), [`lane-contract.md`](lane-contract.md)
(it binds the work you are judging) and this file before your first command. Run
`scripts/lane_setup.sh` in the worktree first.

## The checks

Run every check that applies, and quote what each one printed.

1. **Read the diff, not the message.** Quote `git show --stat` for every commit. Read the whole
   diff: the subject must describe it, and every file it touches must be explained. A diff wider
   than its message is a finding.
2. **FAIL-WITHOUT, per row.** Revert only the fix's `src/` files to the base
   (`git checkout <base> -- <those src files>`), keeping the new tests, and run each row by name
   with `scripts/agent_check.sh <suite.name>`. It rebuilds the test binary, which a `bezelc`-only
   rebuild would not. Then restore with `git checkout <sha> -- <those src files>`, run again, and
   confirm `git status --short` is clean. Never `git stash`. Report each row: the fix rows must
   fail on the base, and any control rows pass in both legs. An aggregate like "4/4 PASS" hides
   a row that never failed.
3. **Run every accept row whose program the base rejected.** Check what it prints, not only its
   exit code: a silent zero-fill exits 0, and so does wrong code that happens not to crash.
4. **Deleted or weakened existing rows are a REJECT,** whatever the fix's merit. Diff every
   touched test file for removed `TEST`/`TEST_F` bodies, removed `EXPECT`/`ASSERT`/`CHECK` lines,
   and changed expected values. One exception: a behaviour flip the spec requires, where the commit
   body names each flipped test and its new expectation (`CLAUDE.md`, *Test and oracle rules*).
   Check each named flip against the spec section it cites. An unnamed change is a REJECT. Moving
   a row is not deleting it, but say where it went.
5. **Behaviour-flip grep.** `git grep` every test named in the commit, every test in the touched
   files, and every diagnostic needle the diff renames or removes, in gtest *and*
   `tests/lit` (a `CHECK-NOT` on vanished text passes vacuously). Run
   `scripts/pregate_stale_needles.py`.
6. **Check a soundness probe against the base first.** If a probe you write also fails on the
   base, it is a pre-existing gap: report it for an issue, and do not reject the fix for it.
7. **Re-measure what the producer called pre-existing.** Any divergence or failure the handback
   dismisses as "pre-existing" or "unrelated" is measured again at the base.
8. **A conflict resolution claimed as a UNION:** read `git diff <base>..<sha>` on each conflicted
   file and confirm both sides' rows and logic survive. Passing tests do not show this.
9. **Areas.** Run the full `--area` of every area whose `src/` the fix touches, and quote the gtest
   `N passed, 0 failed, N total` line (not the lit discovered-tests count).
10. **ASan,** when the fix touches allocation, layout, retain/release or disposal:
    `BEZEL_AOT_FORCE_SANITIZE=address scripts/agent_check.sh …` must print
    `NATIVE SANITIZER: 0 check(s) skipped`, and `grep -c 'LeakSanitizer: detected'` on its log must
    be 0. A leak incidental to what a row pins is fixed in the row (e.g. `using var` on the
    temporary) and filed as an issue. It is not exempted.
11. **Corpus rows** (`tests/conformance/rules/`): `python3 tests/conformance/rules/corpus.py lint`,
    `python3 tests/conformance/rules/corpus.py check`, and `python3 scripts/gate_rule_corpus_baseline.py --strict`, quoting its
    REGRESSION and UNBANKED counts. `corpus.py --strict` is not a valid invocation, and it exits 0.

## Verdict

Return exactly one:

- **ACCEPT**: correct and complete for its scope.
- **ACCEPT-W-NOTES**: correct, with caveats the integrator should see (a stale comment, a
  follow-up issue).
- **ACCEPT-HOLD**: correct on its merits, but it must wait for a sibling commit before it is
  picked. Name the sibling. The coordinator holds it, and it is picked as ACCEPT once the
  sibling is reviewed (`pick_stage.sh` takes only ACCEPT and ACCEPT-W-NOTES).
- **REJECT**: incorrect, fails its own scope, or weakens a test. Give the reason and the evidence.

Label every claim MEASURED (you ran it), QUOTED (you read it in the handback) or HYPOTHESIS.

## Never

- Commit, amend or rebase the branch under review, or leave it edited: check 2's temporary
  revert is restored before you hand back. Findings go in your report.
- Pick, push, merge, or run the full suite.
- Run anything in a worktree while another lane's build or codegen sweep runs there.
- Hand back while a process you started is still running. Check each PID with `kill -0`,
  including `setsid`/`nohup` children.
