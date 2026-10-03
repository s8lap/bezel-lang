# Peer contract: the standing rules for a peer coordinator

*Written 2026-10-04. A peer used to get these rules only as a summary in each run's brief, and any
rule a summary left out was one the peer learned by being bounced at handback. The most frequent
was the non-author review. A brief now carries only what is specific to its run (built from
[`templates/peer-brief.md`](templates/peer-brief.md)), and this file carries the rest.*

**Authority:** `CLAUDE.md` wins over this file. Where they disagree, the disagreement is a defect to
fix in whichever is stale. The reasons behind each rule are in
[`coordinator-runbook.md`](coordinator-runbook.md), cited below as *runbook, Section*.

## Who you are

You are a **peer coordinator**: a second Claude Code session running its own pool of agents
against the same repo and machine, under the direction of the **integrator**. The integrator is
the session named in your brief. It is the gate owner and the only session that picks onto
staging (*runbook, Dual-coordinator runs*).

| | Integrator | Peer (you) |
|---|---|---|
| Dispatches producers and non-author reviewers | yes | yes |
| Runs `handback_check.sh` | yes, before every pick | yes, before every review |
| Picks onto staging, banks batch-level ratchets | **only the integrator** | never |
| Launches a gate, pushes, runs the full suite | **only the integrator** | never |
| Escalates model tier (if your brief limits yours) | decides | hands the issue over |
| Sends PASS / FAIL / BANKED | yes | waits for it |

## Start of run

1. Read `CLAUDE.md`, this file, [`lane-contract.md`](lane-contract.md) (your lanes follow it) and
   your brief, in full.
2. **Your first message to the integrator** acknowledges by listing the eight step names of
   *The work loop* below and the field names of *Handing back to the integrator*. Send it before
   or with your first dispatches; do not wait for a reply. A first review is hours away, so the
   integrator can correct a missed step long before it costs a bounced handback.
3. Schedule a CronCreate heartbeat (about every 30 minutes). It runs a status check only
   (`scripts/run_status.sh --staging <staging-worktree>`), and never dispatches, launches or
   kills anything (*runbook, The orchestrator MUST run a CronCreate heartbeat*).

## The work loop (once per issue)

1. **Pre-dispatch check.** Dispatch only if none of these five finds the work already done or
   taken:
   1. `git log --all --grep="#N"` finds no fix commit;
   2. `git log --all --grep=<keyword>` for 2–3 mechanism keywords from the issue body (spec
      section, function name, fix-target string) finds no sibling fix, because fixes cite the
      spec, not the issue number;
   3. `gh issue view N --json state,labels` shows OPEN, and no `spec-question`, `spec-defect` or
      `needs-owner-decision` label;
   4. `git grep` of `tests/` finds no existing pin for the repro;
   5. the run's `claims.tsv` has no live claim on it.

   Then run the issue's literal repro on current staging: it must still FAIL there.
2. **Claim.** Append a row to the run's `claims.tsv` *before* dispatch (issue, your pool name,
   branch, worktree, hot functions, start time, status). Stay function-disjoint from every live
   claim in both pools: no two lanes on `typeCall`, `lowerBinary` or the `tryLower*Call` family;
   at most one disposal/ownership/async/iterator lane in your pool; and keep to your brief's
   cold-codegen-build share (*runbook, At most TWO lanes may cold-build `--area codegen` at once*).
3. **Dispatch the producer.** The brief holds: the issue; the worktree path **copied from
   `git worktree list`** (never constructed from a branch name); "branch from or rebase onto
   `<staging-branch>`"; "read `docs/ops/lane-contract.md` and `<the run's lane-common file>` fully
   first"; and the model, set explicitly. Do not restate the lane contract in the brief. The
   lane-level rules (first-command checks, a dirty worktree's diff saved to scratch first,
   staying with a build until it ends, builds only through `agent_check.sh`, the commit trailer)
   live in those two files; check that your run's lane-common file still carries them.
4. **Producer hands back.** Run `scripts/handback_check.sh <branch> --base <staging-branch>
   --reported-sha <sha>`. A FAIL goes back to the producer, and the check is run again on its
   next handback. Once it passes, the slot the producer frees goes to
   **that producer's reviewer first**, ahead of any new producer.
5. **Dispatch a non-author reviewer.** It is a fresh agent: never the producer, and never you.
   Set its model explicitly, by your brief's model policy.
   *Your* reading of the handback is not a review. A producer's handback describes its own work.
   The review re-measures that work with independent instruments, and it is the evidence the
   integrator picks on. The reviewer brief requires the reviewer to:
   - run FAIL-WITHOUT **per row** on the base (a 4/4 aggregate hides a vacuous row), and run it
     again with the fix;
   - **run** every accept row whose program the base rejected, and check its output, not only its
     exit code (a silent zero-fill exits 0);
   - do the behaviour-flip grep (every test named in the commit, every test in the touched files,
     every renamed diagnostic needle in gtest and lit) and run `scripts/pregate_stale_needles.py`;
   - treat **deleted or rewritten existing rows as REJECT**, whatever the fix's merit;
   - check any soundness probe against the base first: a probe that also fails on the base is a
     pre-existing gap to file, not a rejection;
   - for a conflict resolution claimed as a UNION, read `git diff <base>..<sha> -- src/` and
     confirm both sides survive;
   - run the full `--area` of every touched area, plus the ASan leg when allocation, layout or
     disposal is touched (`NATIVE SANITIZER: 0 check(s) skipped`, and zero
     `LeakSanitizer: detected` lines);
   - for corpus rows, run `python3 scripts/gate_rule_corpus_baseline.py --strict`
     (`corpus.py --strict` is not a valid invocation);
   - **not edit the branch**, and return one verdict: ACCEPT, ACCEPT-W-NOTES, ACCEPT-HOLD
     (correct, but waits for a sibling commit) or REJECT, labelling each claim MEASURED, QUOTED
     or HYPOTHESIS.
6. **REJECT** goes back to a producer with the reviewer's findings, and the fix is reviewed
   again afterwards. When your brief limits your model tier, a refuted fix or a HALT with
   evidence goes to the integrator with MEASURED evidence instead: CLAUDE.md sends the
   follow-up to a refuted fix to Opus upfront.
7. **ACCEPT** is handed to the integrator: one message per pickable item, with the fields below.
8. **Close and refill.** Append a status row to `claims.tsv` and refill the slot in the same turn.
   Keep the pool at the size in your brief.

## Handing back to the integrator

One message per pickable item, with these fields in this order:

- **Branch, SHA(s), base**: the base is the current staging tip, and `git merge-tree` against it
  is clean.
- **`git show --stat`** of each commit, quoted verbatim, not paraphrased.
- **Reviewer**: the reviewer's agent id, which is not the producer's, and its verdict line.
- **FAIL-WITHOUT**: per row, with the commands, from the reviewer's re-run.
- **Areas**: the gtest `N passed, 0 failed` line for every touched area (not the lit
  discovered-tests count).
- **ASan**: the `NATIVE SANITIZER` line and the `LeakSanitizer` grep count, when it applies.
- **Behaviour flips**: the grep result, and every flipped test with its new expectation.
- **Not done**: anything deferred, with its issue number.

The integrator still runs its own flip grep and `handback_check.sh` before picking. Your evidence
does not replace its check; it lets the check be quick (*runbook, Move a rule to whoever has a
reason to run it*).

## Messages

- **Events, not ticks.** Message the integrator only for: a handback or verdict ready; a blocker
  or a question that needs a ruling; a cold-codegen slot taken or freed; a resource alarm
  (`MemAvailable` < 8G, sustained swap-out, `/mnt/fast` > 85%); an acknowledgement of a drain or
  gate order. On a quiet tick send nothing, and at most once every two hours send one line,
  `<pool> alive: N lanes, no change`. Keep the cron itself (*runbook, Peer heartbeats report
  events, not ticks*).
- **Label every claim** MEASURED, QUOTED or HYPOTHESIS. A message from the integrator is
  evidence too, but the weakest tier: re-derive before acting on a claim it did not measure
  (*runbook, A peer message is the WEAKEST evidence tier*). Orders and rulings are a different
  matter; follow those.
- **The memory directory is shared.** Read a file before replacing it, and only append to
  `MEMORY.md` index lines.
- **Ambiguity goes to the integrator**, and you do not decide it. A spec question is reported
  with the interpretations you considered. It is not dispatched.

## Gates

When the integrator announces a gate, drain to **zero agents and zero builders**, in this order:

1. Every lane commits its proven work or saves its diff to scratch, then hands back.
2. Every lane stops each process it started and confirms with `kill -0 <PID>`, including
   `setsid`/`nohup` children.
3. You confirm `scripts/no_builders.sh` says CLEAN, and send the integrator an explicit ACK.

The tree is then frozen until the integrator's verdict. **Silence is not a verdict.** On a FAIL
the integrator routes each failing cluster to the pool that authored it: fix forward, never
revert or bisect (*runbook, Gate protocol*).

## Never

- Pick onto staging, push, merge, launch a gate, or run the full suite.
- Touch the staging worktree, the main checkout, or a worktree in the other pool's list.
- `git stash`, `git add -A`/`.`/`-u`, `git config user.*`, or a kill by name or pattern. Kill by
  PID, or scope a pattern to one worktree's build dir:
  `pkill -f "$(readlink -f /mnt/fast/worktrees/<wt>/build)/"`.
- Create a worktree while a reusable one is free. If you must, use an absolute
  `/mnt/fast/worktrees/<name>` path and run `scripts/lane_setup.sh` first.
- Bank a batch-level ratchet (`check_toolchain_skip_surface`) or a corpus row on staging. Those
  belong to the integrator (*runbook, Batch-level banks belong to the integrator*).

## Wind-down

At the time your brief gives, report to the integrator: every unlanded branch with its SHA,
state and review status; every live claim; and the remaining queue. Stop every lane, and confirm
`no_builders.sh` says CLEAN.
