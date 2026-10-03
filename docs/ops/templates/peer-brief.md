# <RUN> peer brief: <peer session name>, from <integrator session name>

<!-- Template. Copy to the run's scratch dir (/var/tmp/bezel-scratch/<run>/peer-<name>-brief.md),
     fill every <placeholder>, and delete these comments. A brief holds ONLY what is specific to
     this run: every standing rule lives in docs/ops/peer-contract.md, so do not restate one here.
     If a run needs to override a standing rule, say so explicitly under "Run overrides". -->

**Read first, in full:** `CLAUDE.md`, `docs/ops/peer-contract.md`, `docs/ops/lane-contract.md`
(paths are in any worktree on `<staging-branch>`), then this brief. Your first message to me is
the acknowledgement that `peer-contract.md` *Start of run* asks for, sent with your first
dispatches.

## Run

- **Window:** <start, with timezone> to **<wind-down time>**.
- **Integrator:** `<integrator session name>`. It is the gate owner, the only session that picks
  onto staging, and the one you message.
- **Staging:** branch `<staging-branch>` in `<staging worktree>` (tip `<sha>` at brief time;
  check `git log -1 <staging-branch>` because it advances as I pick). Base and rebase on it.
- **Gate:** launched when staging holds <N> fix issues, or <other trigger>.

## Your pool

- **Size:** <6> agents (producers and reviewers), refilled in the same turn a slot frees.
- **Models:** <e.g. "Sonnet only, set `model: "sonnet"` explicitly on every dispatch; hand
  refuted or HALTed issues to me with MEASURED evidence">.
- **Resource shares:** at most <1> cold `--area codegen` build and <1>
  disposal/ownership/async/iterator lane in your pool at a time.
- **Pool name** for `claims.tsv` and `pick_stage.sh --pool`: `<pool>`.

## Files

- **Scratch:** `/var/tmp/bezel-scratch/<run>/<lane>/` per lane.
- **Claims:** `/var/tmp/bezel-scratch/<run>/claims.tsv`.
- **Lane-common file** (every lane brief embeds it): `/var/tmp/bezel-scratch/<run>/lane-common.md`.

## Worktrees

<!-- Copy paths from `git worktree list`, never construct them from branch names. -->
- **Yours:** <list, or "every worktree in /mnt/fast/worktrees except the ones below">.
- **Not yours:** `<staging worktree>`, the main checkout `<path>`, and the integrator's:
  <list>.

## Queue

- **Integrator-owned (do not touch):** <issues>.
- **Yours, in order:** <issue: one-line hint (existing branch? known trap?)>.
- **Disposal-slot queue (one at a time):** <issues>.
- **Skip:** <issues with a pending ruling>.
- **When empty:** pick open bugs that pass the five-source check, and ask me about anything
  ambiguous.

## Run overrides

<!-- Only deviations from docs/ops/peer-contract.md, each with its reason. Write "None." if
     there are none. -->
None.
