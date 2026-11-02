# Review-lane brief — template

*Added 2026-11-02, from R38. The R38 review briefs re-stated `review-contract.md`'s eleven checks
in every brief — the same problem `templates/fix-brief.md` fixes for producers. This file is the
committed copy of the slots.*

Copy this file, fill every slot, and dispatch it as the review lane's brief. **Do not restate a
standing rule here** — `CLAUDE.md` and `docs/ops/review-contract.md` hold them, and the reviewer
reads both before its first command. If this run overrides a standing rule, say so explicitly under
*Run overrides*.

## Read first, in full

`CLAUDE.md`, then `docs/ops/review-contract.md`, then this brief. `review-contract.md` is binding
and holds the eleven checks, the verdicts, and what a reviewer never does.

## Slots the coordinator fills

### Run

- **Lane:** `<lane-name>` — the reviewer's lane name (the pick ledger's `reviewer` field).
- **Lane under review:** branch `<branch>`, commit `<sha>` (the FULL sha, and the tip's parent as
  the base), worktree `<path>` — **read-only for the reviewer**.
- **Base for every measurement:** the live staging tip `<sha>` on `<staging branch>`. The reviewer
  must **re-read it** at dispatch and again at the end, and report if it moved — a measurement
  taken against a base that moved is not a measurement of this commit.
- **Issues:** `<#N, #N, …>` — the ones the commit claims to close.
- **Model:** `<model id>` — set it explicitly on the dispatch.
- **Pool name:** `r38`.
- **Scratch:** `/var/tmp/bezel-scratch/<lane-name>/` — for the reviewer's own probes. The
  reviewer's scratch is **separate** from the producer's: the producer's probe directory is an
  INPUT here (the harvest list names it), and the reviewer's is where new probes go.
- **Prior reports:** `<path>` — e.g. an earlier round's report on the same lane, or the producer's
  own handback.

### The commit under review

- **Subject and full sha**, quoted from `git show --stat` — **the diff, not the message**.
- **What it claims to fix**, in the reviewer's own words from the issue.
- **What the reviewer should treat as the risk.** Where this commit could be wrong: the shared
  table it edits, the family it might over- or under-cover, the oracle it might have weakened.
  A reviewer who is not told where to look reads the diff and stops.

### What to weigh

- **This campaign's scope:** built-in call admission. An issue outside it is **triaged, not
  worked** — the reviewer says so and does not spend the lane on it.
- **The prior verdict, if any.** For a fix-forward, quote the rejected lane's findings by number
  and say which of them this commit claims to have fixed. **The reviewer's job is to check that
  claim per finding** — not to re-derive the whole diagnosis.
- **The known-hard shape, for this commit only.** E.g. a soundness hazard the previous round
  measured, or a §-citation to check against the real heading.

### Areas and oracles

- **Areas to run:** `<sema | codegen | …>` — one per `src/` area the commit touches. The reviewer
  runs the **whole area**, not just the named tests (`review-contract.md` §9).
- **ASan:** `<yes — because … | no — the commit touches no allocation, layout, retain/release or
  disposal>`.
- **Corpus:** `<does the commit change a corpus row's verdict? | no>` — if yes, the reviewer must
  run the corpus ratchet and **report** the row, not re-bank it (`baseline.tsv` is the
  coordinator's).

### Run overrides

- **Pool size:** `<N>` lanes, not the standing 6 — `<reason>`.
- **Model:** `<model>` for every lane in this run, not the standing Sonnet/Opus tiers — `<reason>`.
- **Trailer:** not the reviewer's concern — the reviewer makes no commit. (If the run needs the
  reviewer to fix a required pre-pick correction, the **producer** does it in its own tree, or the
  coordinator does it in a clean worktree.)

## The brief does NOT restate

These are in `CLAUDE.md` / `review-contract.md` and the reviewer has read them:

- the eleven checks (diff-not-message, fail-without per row, run the accept rows the base
  rejected, deleted/weakened rows are a REJECT, the behaviour-flip grep, soundness probes against
  the base first, re-measure "pre-existing" claims, a conflict claimed as a UNION is not, areas,
  ASan, corpus rows);
- the verdicts (`ACCEPT` / `ACCEPT-W-NOTES` / `REJECT`) and what each obliges;
- **a reviewer never edits the lane's files** — it reports;
- a REJECT needs the failing command's output;
- **"pre-existing" needs a named baseline, and the baseline is `origin/main`**;
- the report's shape (a verdict, the evidence per check, the findings by number with severity).

## The handback

`review-contract.md`'s verdict section holds the format. This brief adds only:

- the **verdict**, and for each finding its **number**, its **severity**, and whether it is
  **blocking** (a REJECT) or a note;
- **the failing command's output** for every blocking finding — a finding without it is not a
  finding;
- for a fix-forward, a **per-finding disposition check**: does this commit actually fix what the
  prior round said? Name the prior finding number and the line that fixes it. **A commit that
  claims to fix findings and does not is itself a REJECT-level finding**;
- the **areas** run, with the gtest summary lines quoted from the log;
- the **corpus** numbers;
- the **lane's state at handback**: `git status --short` (must be empty) and `HEAD` — the reviewer
  changed nothing;
- anything the reviewer could **not** check, and why.

**The reviewer's report is also the harvest input.** A finding that would make a good conformance
case is named in the harvest list at the end, with the file:line of the defect.

## Why this template exists

Same reason as `fix-brief.md`: the R38 review briefs carried the contract's eleven checks inline,
so the report was long and the run-specific part — which findings this commit claims to fix, and
where to look — was the smallest share of it. Worse, a brief that restates a check can restate it
*wrongly*, and the check the reviewer actually applied is then the one in the contract, not the one
in the brief.
