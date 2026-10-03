# docs/ops: how the work is run

These files describe how agents work on this repository, not the Bezel language. None of them is
normative for the language. `CLAUDE.md` holds the rules and wins over every file here.

| File | Who reads it | What it holds |
|---|---|---|
| [`lane-contract.md`](lane-contract.md) | every fix lane, before its first command | the standing rules for a lane: build, evidence, authority, committing, reporting |
| [`peer-contract.md`](peer-contract.md) | a peer coordinator, before its first dispatch | the work loop, handback fields, messaging, gate drain and limits for a pool run under an integrator |
| [`coordinator-runbook.md`](coordinator-runbook.md) | the integrator before a gate or a pool; anyone who needs a rule's reason | the procedures, and the incident evidence behind each rule |
| [`templates/peer-brief.md`](templates/peer-brief.md) | the integrator, when briefing a peer | the run-specific slots of a peer brief; everything else is in `peer-contract.md` |

A brief, for a lane or a peer, carries only what is specific to its assignment and points to its
contract for the rest. When a rule is learned during a run, add it to the contract it belongs in,
with its reason in the runbook, so the next run's briefs need not repeat it.
