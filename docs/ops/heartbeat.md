# The heartbeat — its standing rule, and how to recreate it

*Added 2026-11-03. This file is the COMMITTED COPY of the heartbeat's contract and of the
exact schedule that implements it, so that the NEXT run recreates the heartbeat with the new
behaviour instead of inheriting the old one.*

A heartbeat is a session artifact: it lives in the orchestrator's scheduler, not in the
repository, so it dies with the session. **This file is what makes it survive.** The next
coordinator reads it, re-creates the schedule with the same name, cadence and prompt, and
inherits the standing rule below. A run that starts without a heartbeat is a run that can
go silent for hours; a run that starts with the OLD heartbeat is a run that goes silent in
a different way — see *Why the rule changed* below.

## The standing rule

**The heartbeat STATUSES and then REKINDLES. A partial pool is a REFILL, not a
report-and-stop.**

1. Run `scripts/run_status.sh --staging <staging-wt> --claims <claims.tsv> --pool 7` and
   report the table (gate, jobserver, builders, memory, disk, staging, lanes, pool, stale).
2. **If the `pool` row reads fewer than 7 live lanes, DISPATCH lanes in the SAME turn to
   fill every empty slot.** **The pool target is 7** (owner directive 2026-11-02 ~23:55 set
   it at 7; the owner's directive of 2026-11-03 ~23:05 — *"reduce the pool to 5 agents.
   record this durably"* — **LOWERED IT TO 5**, because the coordinator over-launched the
   pool to 16–17 lanes, the box ran out of build budget, and **every lane died at once**;
   **⇒ ⇒ ⇒ AND THE OWNER'S DIRECTIVE OF 2026-11-04 ~13:55 — *"Raise the pool to 7,
   review-weighted"* — RAISED IT BACK TO 7, because the box was then measured at ~20 % CPU
   with 19–20 of 20 jobserver tokens FREE and 6 GB of 31 GB RAM used: the limit had never
   been CPU/RAM/DISK, and the real bottleneck was the REVIEW QUEUE**). **⇒ ⇒ ⇒ ⇒ AT 7 LANES
   THE POOL IS WEIGHTED TOWARD REVIEW — a producer only deepens the queue a reviewer must
   drain — AND A MARGINAL REVIEWER LANE HAS NOTHING TO REVIEW (every R35/R38 lane branch is
   already in `origin/main` by patch-id), so a marginal lane's work is ISSUE VERIFICATION:
   reproduce an OPEN issue on the shipping tree and return a DISPOSITION (still reproduces /
   already fixed / the repro is wrong / it needs an owner ruling).** **⇒ WHAT DID NOT CHANGE:
   at most TWO lanes cold-build `--area codegen` at once; every lane brief carries the build
   gate (`available` ≥ 6, then `-j 4`, never `-j 20`); dispatch in PAIRS WITH A STAGGER, never
   the whole pool in one burst.** `CLAUDE.md`'s *Pools* bullet says 7. The `pool` row counts
   the DISTINCT lanes holding a live claim in the claims file. **A lane that dies counts as a
   slot that freed** — refill it in that same turn, do not wait for the next heartbeat.
3. **PICK landed commits.** If a lane has delivered a commit and a NON-AUTHOR review has
   recorded a verdict, pick it onto the staging branch with `scripts/pick_stage.sh`. Never
   pick a commit whose review says REJECT. A `narrow` commit is recorded with kind `fix`
   but its issue stays OPEN on GitHub.
4. Keep lanes file-disjoint (no two on the same hot dispatch function: `typeCall`,
   `lowerBinary`, the `tryLower*Call` family). At most **two** lanes may cold-build
   `--area codegen` at once — the pool size is not the build budget; the rest queue.
5. Append every new lane to the claims ledger BEFORE dispatching it. The ledger is
   append-only, its columns are
   `issue, lane, branch, worktree, hot_files, start, status`, and **the status is the LAST
   field** — a row whose `start` and `status` are swapped is invisible to the pool count.

### What the heartbeat must NEVER do

- **Never launch a gate.** One gate per run, near the end, carrying 100+ issue closures
  (owner directive 2026-11-03). The heartbeat must not spend a ~50-minute cycle on a haul
  of 20, and it must not drain the pool to gate.
- **Never push to `origin`.** The push is what closes the GitHub issues, and it belongs to
  the one deliberate gate at the end of the run.
- **Never edit source in the staging worktree.** The staging worktree is read-only to
  everyone but the coordinator, and even the coordinator parks edits outside it.

The Cycle-9 scar still holds for the *gate* half of the rule: a misfiring cron once hit the
hard-stop early and left the ASan oracle INCOMPLETE. The heartbeat is a **backstop, not a
scheduler**; the pool refill it now performs is a dispatch of *lanes*, which is what the
heartbeat is for, not a gate.

### Why the rule changed

Measured 2026-11-03, on the R38 `builtin-call-admission` run: the pool fell to **1 live
lane of 7** and STAYED there. The old heartbeat prompt said *"If the table shows a problem
(a stale builder, a failed gate, a full disk, a partial pool), report it and stop."* — so
the heartbeat dutifully reported the partial pool and stopped, **twenty minutes later**,
reporting the same thing again, while the box sat **completely idle** (0 lane build
processes, 0 heavy processes box-wide) with six of seven lane slots empty and the banked
count pinned at 20. A heartbeat that only observes a stalled pool watches the stall. The
owner's directive (2026-11-03) was explicit: *"REKINDLE THE POOL TO 7"*.

## Recreating the schedule — verbatim

The tool is `schedule_create` (the docs' older name for it is *CronCreate*). There is no
`schedule_update`, so **changing the heartbeat means deleting the old schedule and
creating the new one** — do the two back to back so there is no gap with no heartbeat:

```
schedule_delete({ id: "<the old schedule id>" })
schedule_create({
  name: "r38-heartbeat",
  when: { every: "20 minutes", tz: "Pacific/Auckland" },
  ends: { at: "<the run's end, e.g. 2026-11-05T02:00:00.000+13:00>" },
  prompt: "<the prompt below, VERBATIM>",
})
```

The 2026-11-03 instance was `sch_19eff064a001uPPxyMG3Muoa2P`, replacing
`sch_19d81bf5d001RX3ZhdilKdMqk6` (which replaced `sch_19be9316a0011lGl6KSAqQku3Q`, the old
report-and-stop one). The cadence is coarse on purpose — tens of minutes; a tight interval
burns budget re-reading state that has not moved. The `ends` bound is the run's own deadline,
so the schedule cannot fire past it.

### Extending the run — the rule, so the next extension is mechanical

**The run's end is the schedule's `ends` bound AND a line in TWO committed files. An
extension that changes only the schedule is not durable** — the next session reads the
files, not the scheduler, and would inherit the old deadline.

An owner extension of the run's end is therefore THREE steps, in this order:

1. **Delete the old schedule and create the new one, back to back** (there is no
   `schedule_update`), with the SAME name, the SAME cadence, the SAME prompt **verbatim**,
   and `ends.at` = the new end. **⇒ Do the two back to back: a gap with no heartbeat is
   worse than a stale end.** The new schedule's id is printed by `schedule_create` — record
   it in this file.
2. **Update the `ends` example above in this file** (it is the committed copy of the
   instance) and add the new instance id to the paragraph above.
3. **Record the extension in `docs/proposals/papers/master-plan.md` §6** (the plan's
   handback section) and in the campaign's coordinator block
   (`docs/proposals/papers/campaigns/builtin-call-admission.md`), with the date, the old end,
   the new end, and the directive that caused it.

**⇒ The extension does NOT change the gate cadence.** The run still ends with **ONE** gate,
near the end, carrying 100+ issue closures, and the pool is still refilled to 7 every time
the agent count goes down. A longer run means MORE haul before that one gate — it does not
mean more gates, and it does not mean a drained pool.

**MEASURED 2026-11-03 (the R38 run's first extension).** The owner extended the R38 run by
**24 hours**: the end moved from **2026-11-04 02:00 NZDT** to **2026-11-05 02:00 NZDT**.
The schedule was recreated as `sch_19eff064a001uPPxyMG3Muoa2P` with the identical prompt
and cadence and `ends.at = 2026-11-05T02:00:00.000+13:00`.

**MEASURED 2026-11-03 ~23:05 (the R38 run's pool reduction).** The pool target was lowered
from **7 to 5**, so the schedule's prompt changed and **the schedule was recreated** — there is
no `schedule_update` ⇒ the only way to change a prompt is delete + create. **⇒ THE CREATION
CALL'S END FIELD IS `ends: { at: "<iso>" }`, NOT `endsAt` — MEASURED: `endsAt` IS SILENTLY
IGNORED AND THE NEW SCHEDULE IS BORN WITH NO END.** The current instance is
**`sch_1a0df553a001e742yLwXJrJm7b`**, `ends.at = 2026-11-05T02:00:00.000+13:00`, prompt carrying
`--pool 5`. **⇒ A heartbeat survives a
session restart** — the schedule lives in the orchestrator's server, not in the session
(measured: the R38 heartbeat fired 17 times across a restart and kept its cadence).

### The prompt, verbatim

```
HEARTBEAT for the R38 campaign run. STATUS CHECK, THEN REKINDLE THE POOL.

1. Run: scripts/run_status.sh --staging /mnt/fast/worktrees/r28-staging --claims /var/tmp/bezel-scratch/r38-coord/claims.tsv --pool 5
2. Report the table (gate, jobserver, builders, memory, disk, staging, lanes, pool, stale). The pool target is 7 lanes (owner directive 2026-11-02 ~23:55 set it at 7; the owner's directive of 2026-11-03 ~23:05 LOWERED IT TO 5 — "reduce the pool to 5 agents. record this durably" — because the pool had been over-launched to 16-17 lanes and the box ran out of build budget; the standing rule in CLAUDE.md is 5). The `pool` row counts the DISTINCT lanes holding a live claim in the claims file, FLAGs a PARTIAL pool, and reads INFO for a drained one. The `builders` row carries a 5-second window as well as the instantaneous verdict, so a CLEAN instant next to a busy window means the box is building, not idle.
3. REKINDLE THE POOL TO 5 (owner directive 2026-11-03 ~23:05). If the pool row reads fewer than 5 live lanes, DISPATCH lanes IN THE SAME TURN to fill every empty slot. A PARTIAL POOL IS A REFILL, NOT A REPORT-AND-STOP. A lane that dies counts as a slot that freed; refill it in the same turn. Keep lanes file-disjoint (no two on the same hot dispatch function: typeCall, lowerBinary, the tryLower*Call family). At most TWO lanes may cold-build --area codegen at once; the rest queue behind them. Append each new lane to /var/tmp/bezel-scratch/r38-coord/claims.tsv (issue, lane, branch, worktree, hot_files, start, status; status is the LAST field) BEFORE dispatching it.
4. PICK LANDED COMMITS. If a lane has delivered a commit and a NON-AUTHOR review has recorded a verdict, pick it onto the staging branch: scripts/pick_stage.sh --branch r30-c1 --pool r38 --lane <branch> --reviewer <rev> --verdict ACCEPT|ACCEPT-W-NOTES --kind fix|test<sha>. A `narrow` commit is recorded with kind `fix` but its issue stays OPEN on GitHub. Never pick a commit whose review says REJECT.
5. DO NOT launch a gate. DO NOT push to origin — the one gate near the end of the run is a separate, deliberate act. DO NOT edit source in the staging worktree.
6. ONE GATE PER RUN, NEAR THE END, CARRYING 100+ ISSUE CLOSURES (owner directive 2026-11-03) => DO NOT DRAIN THE POOL TO GATE. Bank the haul; gate when the run ends.
7. If the table shows a failed gate, a stale builder, or a full disk, report it loudly and stop that part; the pool refill still happens.
```

The prompt names concrete paths (the staging worktree, the claims ledger). **The next run
must point those at ITS OWN paths** — the text above is the R38 instance. What must survive
the edit is the *rule*: status, then refill to 7, then pick landed commits; never gate,
never push.

## Where the rule also lives

- `CLAUDE.md` → *Gates* and *Pools and multiple coordinators* — the one-line standing rules.
- `docs/ops/coordinator-runbook.md` → *The orchestrator MUST run a CronCreate heartbeat* —
  the narrative and the Cycle-9 scar.
- `docs/proposals/papers/master-plan.md` → the gate cadence, restated by the owner's
  directive of 2026-11-03: **one** gate, near the run's end, carrying 100+ closures.
- `docs/proposals/papers/campaigns/builtin-call-admission.md` → the campaign's coordinator
  blocks, where the call is recorded with its date and its measurement.

**MEASURED 2026-11-04 ~17:25 (R38-ext: the coordinator moves to Claude Code; the run ends SATURDAY).**
The owner moved coordination from the OpenCode session to a Claude Code session. The OpenCode
coordinator deleted its schedules (`sch_1a0df553a001e742yLwXJrJm7b` and the end-of-run one-shot
`sch_1a2070f7e0019l8OjIgnYtkfdG`) on handover. The owner then set the run's end to
**2026-11-07 02:00 NZDT (Saturday)**. The Claude Code coordinator's heartbeat is a CronCreate job
in its own session (cadence `13,43 * * * *`). It is not an OpenCode schedule, so it does NOT
survive the end of that session: a successor coordinator recreates it from the standing rule
above. The rule itself is unchanged: STATUS then REKINDLE, one gate near the end, never gate or
push from the heartbeat.

**MEASURED 2026-11-11 02:11 NZDT (the R41 run's chain, with ONE gate still to come).** The staging
branch `r30-c1` stands at **`8e50676fa`** — **86 commits over `origin/main`** (`714ba5490`) — and
`scripts/run_status.sh --staging /mnt/fast/worktrees/r28-staging --claims
/var/tmp/bezel-scratch/r41-coord/claims.tsv --pool 6 --target 100` prints, on its `staging` row,
**`r30-c1 @ 8e50676fa | +86 over origin/main | closable 100/100 | fix-named 98`** — **⇒ THE OWNER's
100-CLOSURE FLOOR IS REACHED, AND THE ROW READS `FLAG` AND NAMES `scripts/pregate.sh`.** The
ledger's own summary (`scripts/pick_ledger.sh origin/main..r30-c1`) reads **86 commit(s)** ·
**`distinct fix issues: 98`** · **`distinct issues GitHub would close (keyword rule, subject+body,
any kind): 100`** · **`UNATTRIBUTED: 2 commit(s)`** — the two `bank:` commits, whose diffs touch only
`scripts/internal_residual.baseline` and `tests/conformance/rules/baseline.tsv`, so there was no
source change in them for a reviewer to read. **The 100 are 36 `campaign:using-heads`, 61
`campaign:builtin-call-admission` and 3 unlabelled (#3480, #8361, #8363); 97 of them are still OPEN
on GitHub and 3 (#3480, #4126, #7915) are already closed, so the push is a no-op re-close for those
three.** Both ratchets are **GREEN ON THIS TREE**: the corpus ratchet reads **`OK (5781 cases, 3742
ok, 210 known-divergent, 1829 unclassified)`** and the internal-residual ratchet reads **`OK
(total_internal_lines=69)`** — **69 IS THE BANKED BASELINE IN THIS TREE, SO THE COUNT IS EXACT, NOT
MERELY UNDER IT** (the corpus runner's own summary: `5781 cases: 3742 ok, 210 known-divergent, 1829
unclassified, 0 REGRESSION, 0 UNBANKED`). They were measured at `84e36307f6`
(`reviews/r41-final-ratchet.md`), **whose TREE IS BYTE-IDENTICAL TO THIS TIP's** —
`git rev-parse 84e36307f6^{tree}` = `git rev-parse r30-c1^{tree}` =
`ab591f5804a24f4756abd5a54a39d1cdc8053245` — because the three message surgeries that followed
changed commit MESSAGES ONLY, and a message cannot move a corpus row or an `internal:` emission.
**⇒ THE CONSEQUENCE: THE HEARTBEAT's REMAINING JOB IS TO HOLD THE POOL AND TO **NOT** GATE; THE GATE
IS THE RUN's LAST ACT.** The R41 gate has **NOT** run — the `gate` row still reads `pid: none | done:
0` and names `a30bb11d_cycle.log`, whose last line is `[gate_full_cycle 23:03:30] cycle COMPLETE
(/mnt/fast/worktrees/r28-staging @ a30bb11d): both oracles trustworthy-green + conformance green`,
and **`a30bb11d` IS R40's TIP — THE `origin/main` THIS RUN OPENED ON — NOT R41's**. **⇒ THE GATE's
VERDICT IS NOT YET MEASURED; IT WILL BE RECORDED HERE WHEN IT IS.**
