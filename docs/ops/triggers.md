# Triggers: before you do X, read Y

The SessionStart hook (`~/.claude/hooks/bezel_session_start.sh`) prints this file, from the
staging ref, at every start, resume and compaction. Keep it under 80 lines: the hook shows
only the first 80. Each line names an action and the place its rule lives. If a rule bit
you mid-run and was not reachable from here, add a line.

## Every session, first
- **Read `CLAUDE.md` from the STAGING ref, not the main checkout.** The main checkout is often
  days behind (`git -C <repo> show <staging>:CLAUDE.md`). The hook warns when they differ.
- Read the memory index (`MEMORY.md`), then the open run cursor's LAST block.
- A coordinator: `docs/ops/coordinator-runbook.md`. A lane: `lane-contract.md`. A reviewer:
  `review-contract.md`. A peer: `peer-contract.md`.

## Before you dispatch
- **Any lane** → CLAUDE.md *Pools* (pool target, cold-build cap of two, build gate
  `available ≥ 6` then `-j 4`, stagger in pairs) and the five-source claim check.
- **Choose the model**:
  - OpenCode (MiniMax) lanes are **producers only**; every review, verification, ruling
    and filing is a Claude lane (owner 2026-11-04);
  - a fix **refuted twice** goes to **Fable at effort high**, not a third Opus (owner
    2026-11-05);
  - Opus for the cases CLAUDE.md lists.
- **The effort** → vary it per dispatch (CLAUDE.md *Vary the EFFORT*). Never one effort for
  the whole pool.
- **A worktree for the brief** → confirm procs = 0 in it and no live claim on it
  (runbook *Reaping*, step 2). A busy tree reused for a new lane is a clobber.
- **A brief** → the template for its kind (`docs/ops/templates/`). A brief names files,
  never rules.
- **A producer handed back** → its slot goes to **its reviewer first**.

## Before you pick
- `scripts/handback_check.sh <branch> --base <staging>`; verify the REF, not the SHA you
  were handed. Pick only with `scripts/pick_stage.sh`, in authored order (the ancestor
  first). Every staging commit needs a pick note.
- An accepted review → queue its probes for a harvest lane in the same turn.

## Before you file, close or comment
- File via `gh api` (`gh issue create` is denied), with exactly one `campaign:<slug>` or
  `parking-lot` label. Re-search with mechanism words first: "no issue covers this" is a
  claim.
- A hand-close needs the pinning test named and a full fix (not `narrow #N`).

## When the disk or the process table is full
- → runbook *Reaping stale processes, worktrees and build dirs*. You may reap anything
  stale (owner 2026-11-05); commits and live lanes are protected. **Never kill a live
  agent's waiter shell**, or that agent never wakes.

## When an agent "completed" but its work is not done
- It is waiting on its own background run. Check procs in its worktree. If the run has
  finished and the agent was not woken, `SendMessage` it to finish. Do not re-dispatch.

## Before a gate (the gate owner only)
- `scripts/pregate.sh` must say READY; zero agents and zero builders; the tree is frozen.
  One gate per run, near its end, carrying 100+ closures. Runbook *Gates*.

## When a rule had to be repeated to you
- Add it to the contract it belongs in, with its reason in the runbook, and a trigger line
  here, so the next session meets it at startup instead of on correction.
