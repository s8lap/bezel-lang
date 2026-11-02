# docs/ops: how the work is run

These files describe how agents work on this repository, not the Bezel language. None of them is
normative for the language. `CLAUDE.md` holds the rules and wins over every file here.

| File | Who reads it | What it holds |
|---|---|---|
| [`lane-contract.md`](lane-contract.md) | every fix lane, before its first command | the standing rules for a lane: build, evidence, authority, committing, reporting |
| [`review-contract.md`](review-contract.md) | every non-author reviewer, before its first command | the checks a review runs, the verdicts, and what a reviewer never does |
| [`peer-contract.md`](peer-contract.md) | a peer coordinator, before its first dispatch | the work loop, handback fields, messaging, gate drain and limits for a pool run under an integrator |
| [`coordinator-runbook.md`](coordinator-runbook.md) | the integrator before a gate or a pool; anyone who needs a rule's reason | the procedures, and the incident evidence behind each rule |
| [`templates/peer-brief.md`](templates/peer-brief.md) | the integrator, when briefing a peer | the run-specific slots of a peer brief; everything else is in `peer-contract.md` |
| [`templates/fix-brief.md`](templates/fix-brief.md) | the integrator, when briefing a fix lane (a new fix or a fix-forward) | the run-specific slots of a fix lane's brief; everything else is in `lane-contract.md` |
| [`templates/harvest-brief.md`](templates/harvest-brief.md) | the integrator, when briefing a harvest lane | the slots of a harvest brief and the eight-point per-case self-check; everything else is in the runbook |
| [`templates/review-brief.md`](templates/review-brief.md) | the integrator, when briefing a non-author reviewer | the run-specific slots of a review brief; everything else is in `review-contract.md` |

A brief, for a lane, a reviewer or a peer, carries only what is specific to its assignment and points to its
contract for the rest. When a rule is learned during a run, add it to the contract it belongs in,
with its reason in the runbook, so the next run's briefs need not repeat it.

## The templates

There is one per kind of lane, and each points at the contract that holds its standing rules:

| Lane kind | Template | Contract it points at |
|---|---|---|
| fix (a new fix, or a fix-forward) | [`templates/fix-brief.md`](templates/fix-brief.md) | [`lane-contract.md`](lane-contract.md) |
| review (a non-author reviewer) | [`templates/review-brief.md`](templates/review-brief.md) | [`review-contract.md`](review-contract.md) |
| harvest (conformance cases) | [`templates/harvest-brief.md`](templates/harvest-brief.md) | the runbook's *Harvest review probes into the corpus* |
| peer (a second coordinator) | [`templates/peer-brief.md`](templates/peer-brief.md) | [`peer-contract.md`](peer-contract.md) |

**A brief names the files it is about, never the rules.** The rules live in the contract; a brief
that restates one is a brief that can restate it wrongly, and the reviewer then checks against the
brief instead of the contract. When a rule is learned mid-run, it goes into the contract it belongs
in — with its reason in the runbook — and the briefs that repeat it get shorter.
