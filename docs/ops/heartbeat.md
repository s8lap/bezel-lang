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
   fill every empty slot.** The pool target is 7 (owner directive 2026-11-02 ~23:55;
   `CLAUDE.md`'s *Pools* bullet says 7). The `pool` row counts the DISTINCT lanes holding a
   live claim in the claims file. **A lane that dies counts as a slot that freed** — refill
   it in that same turn, do not wait for the next heartbeat.
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
  ends: { at: "<the run's end, e.g. 2026-11-04T02:00:00.000+13:00>" },
  prompt: "<the prompt below, VERBATIM>",
})
```

The 2026-11-03 instance was `sch_19d81bf5d001RX3ZhdilKdMqk6`, replacing
`sch_19be9316a0011lGl6KSAqQku3Q` (the old report-and-stop one). The cadence is coarse on
purpose — tens of minutes; a tight interval burns budget re-reading state that has not
moved. The `ends` bound is the run's own deadline, so the schedule cannot fire past it.

### The prompt, verbatim

```
HEARTBEAT for the R38 campaign run. STATUS CHECK, THEN REKINDLE THE POOL.

1. Run: scripts/run_status.sh --staging /mnt/fast/worktrees/r28-staging --claims /var/tmp/bezel-scratch/r38-coord/claims.tsv --pool 7
2. Report the table (gate, jobserver, builders, memory, disk, staging, lanes, pool, stale). The pool target is 7 lanes (owner directive 2026-11-02 ~23:55; the standing rule in CLAUDE.md is 7). The `pool` row counts the DISTINCT lanes holding a live claim in the claims file, FLAGs a PARTIAL pool, and reads INFO for a drained one. The `builders` row carries a 5-second window as well as the instantaneous verdict, so a CLEAN instant next to a busy window means the box is building, not idle.
3. REKINDLE THE POOL TO 7 (owner directive 2026-11-03). If the pool row reads fewer than 7 live lanes, DISPATCH lanes IN THE SAME TURN to fill every empty slot. A PARTIAL POOL IS A REFILL, NOT A REPORT-AND-STOP. A lane that dies counts as a slot that freed; refill it in the same turn. Keep lanes file-disjoint (no two on the same hot dispatch function: typeCall, lowerBinary, the tryLower*Call family). At most TWO lanes may cold-build --area codegen at once; the rest queue behind them. Append each new lane to /var/tmp/bezel-scratch/r38-coord/claims.tsv (issue, lane, branch, worktree, hot_files, start, status; status is the LAST field) BEFORE dispatching it.
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
