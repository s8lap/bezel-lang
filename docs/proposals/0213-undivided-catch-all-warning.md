# Proposal 0213 — A warning for an undivided `Exception` catch-all

| | |
|---|---|
| **Status** | under-review — draft (deferred from 0211 Q11, 2026-11-10); its exemption is to be re-read against v0.358's narrowed marker at graduation ([proposal 0214](0214-narrowed-failure-marker.md) §5) |
| **Tracking issue** | [#8407](https://github.com/s8lap/bezel-lang/issues/8407) (proposal 0211's tracker; this proposal takes its own issue if its trigger is met) |
| **Target documents** | **spec** — §31.2.3 (one paragraph: the warning), §31.2.5 (one sentence: the suppression idiom's divided spellings), Appendix C.7 (one bullet). **code-generation reference** — §6 (one sentence). No grammar change, no new keyword. |
| **Written against** | spec, stdlib and rationale **v0.356** (proposal 0211 graduated); measurements carried from proposal 0211 §12, taken on the v0.355 corpora |
| **Origin** | Proposal [0211](0211-failure-marker-and-boundary-arm.md) §5.4 and §12, deferred by the owner-delegated ruling on 0211's Q11 (2026-11-10, [0211 §16](0211-failure-marker-and-boundary-arm.md#16-rulings-owner-delegated-2026-11-10)). |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2): a new compile-time diagnostic. Warn-only; a promotion to an error would be a further proposal, not a ratchet of this one. |

*Conventions. An unprefixed § is a language-spec section at v0.356. Evidence labels as in proposal 0211: **MEASURED** — produced by a script under [`docs/research/error-model/probes-0211/data/`](../research/error-model/probes-0211/data/) and re-runnable; **QUOTED** — text of a repository document; **HYPOTHESIS** — a judgement or prediction.*

## 0. Summary

Since v0.358 an exception whose class implements `IFailure` is a *failure* — the environment or a peer produced it — and a class without the marker makes no statement (spec §31.4, *The failure marker*). The documents teach that a boundary names the classes it expects and `IFailure` for the outside world's, with an `Exception` arm after them only in a supervisor that reports the rest (Guide 2 §4). An arm on bare `Exception` that handles both kinds in one body is "the habit to drop: it answers a bug as if it were bad input" (QUOTED, Guide 2 §4). This proposal would make that habit visible at compile time: a warning on an unguarded `Exception` arm that no earlier arm of its `catch` divides from the failures.

The design was drafted in proposal 0211, corrected after that proposal's critique, and deferred by its Q11 ruling. **This proposal is parked until its trigger is met (§4).** It is recorded now so that the corrected design and its measurements are not lost.

## 1. Why it was deferred

*Note, v0.358 ([proposal 0214](0214-narrowed-failure-marker.md) §5).* The marker now covers only the environment and peer classes, so the exemption below covers less. `IFailure => …, Exception => …` passes the exemption and still takes bad input (`FormatException`, `SerializationException`) and bugs in the same `Exception` arm. The substance is left to this proposal's graduation, which re-reads the exemption against that.

From 0211 §5.4, adopted as the Q11 ruling:

- its evidence is test-corpus oracles and the documents' own examples; the ergonomics study's 40 everyday programs have no catch-all at all, and this project's gates decline to act on that class of evidence (proposal 0061 §2; proposal 0076 §0);
- nothing in 0211 depends on it;
- it is non-breaking to add later, and once the marker exists, whether programs keep writing the undivided arm becomes observable;
- its first design had a hole (it exempted `Exception => ()`, the widest swallow, while warning on an arm that logs), which says the rule needs its own review.

## 2. The rule

An unguarded arm whose pattern names `Exception` alone — bare or binding, not in an alternative — draws a warning unless

- (a) an earlier unguarded arm of the same `catch` block lists `IFailure`, alone or in an alternative; or
- (b) the arm's body contains a bare re-raise (`throw` with no operand, §31.1).

Its consequences, each stated so that a reviewer can attack it:

- **`Exception => ()` is not exempt.** Revision 1 of 0211 exempted the unit body, which exempted the widest swallow while warning on an arm that logs. Under this rule §31.2.5's suppression idiom warns unless it follows an `IFailure` arm, and the two spellings that say what is meant are `IFailure => ()` (failures only; spec §31.2.5 since v0.356) and `IFailure => (), Exception => ()` (everything, with the division written out).
- **What satisfies (a).** An earlier unguarded arm that lists `IFailure` itself. An interface that extends `IFailure` does not: it takes only some failures. A guarded `IFailure` arm does not: its guard may decline.
- **Both kinds in one body, on purpose:** `var ex: IFailure | Exception => …` is not an arm naming `Exception` alone, so it does not warn (it compiles and runs on r40 with a program-declared marker; 0211 probe f19, case 3).
- **Known bypasses.** Any guard on the `Exception` arm (`when true` included) takes it out of the rule. A conversion (`Exception => throw new Wrapped(…)`) is *not* a bypass: it has no bare re-raise, and a wrapper may be a failure class.
- **Cancellation.** `OperationCanceledException => throw` before the `Exception` arm does not satisfy (a) by itself; a supervisor that gives cancellation its own arm (Guide 2 §4) still writes the `IFailure` arm to divide the rest.
- **Locality** (proposal 0105 §1): **local** — one `catch` block's arm patterns and one arm body. No raise and no arm selection is modelled, so the decline of modelling which arm a raise can reach by exception type (proposal 0198 §7) is not engaged.

## 3. Reach

MEASURED (0211's `data/catch_arms.py` → `catch_arms_summary.json`, `undivided_lit_files.txt`; four corpora of test programs and document examples, about 36,900 programs, at v0.355):

| Fact | Count |
|---|---|
| arms the rule would warn on | 572 |
| … that handle (log, record, answer) | 545 |
| … that convert (`throw new …(…, ex)`) | 21 |
| … that suppress (`Exception => ()`) | 6 |
| rule-corpus cases among them with `expect: quiet` | 0 |
| lit files containing one that also assert the absence of diagnostics | 12 |
| document-example corpus cases that assert silence | 0 (its classes are `compiles`, `diagnose` and `skip`) |
| the 40 everyday programs of the ergonomics study | 0 |

Document sites it would touch (MEASURED, the *catchall* group of 0211's `data/doc_sites.tsv`, re-read against v0.356): spec §31.1's closure example and §31.2.5's `Exception => ()`; stdlib §6.5's handler example; rationale §13.5's per-handler swallow. The two teaching examples 0211 rewrote to the marker (spec §31.2.4; Guide 2 §5) no longer draw it. Two more sites are compile-error examples and are unaffected. Each touched site would either gain the `IFailure` division or be left, warning, with a stated reason (§31.1's example is about the closure; rationale §13.5 isolates a handler on purpose).

## 4. The trigger

**Evidence of the habit outside the test corpora, after the marker exists:** programs written against a compiler that has `IFailure` — the ergonomics study rerun, or code-generation transcripts — in which a boundary handles everything in one `Exception` arm. Absent that evidence, the deferral stands. The measurement to run when it exists is §3's table over the new population, with the handle/convert/suppress split, and a read of each hit for whether the arm was a boundary (the rule's target) or a test oracle (its false positive).

## 5. Proposed text (draft, to be re-anchored when the trigger is met)

- **§31.2.3**, a new paragraph after the unreachable-arm warning: *An unguarded arm whose pattern names `Exception` alone, bare or in binding form, is a warning unless an earlier unguarded arm of the same `catch` lists `IFailure` ([§31.4](Bezel_Language_Spec.md#sec-the-failure-marker)) or the arm's body contains a bare re-raise. Such an arm takes, in one body, what the environment or a peer raised and every other exception; name `IFailure` ahead of it, or list both in one alternative (`IFailure | Exception`), to state the division.*
- **§31.2.5**, end of the last paragraph: *To suppress everything, with the division written out: `IFailure => (), Exception => ()`.*
- **Appendix C.7**, one bullet stating the warning.
- **Code-generation reference §6**: one sentence under *Catching at a boundary*.

## 6. Open questions

**Q1 — Warn-only.** The warning is a lint on an idiom, and §31.2.3's `Exception` arm keeps its meaning. **Recommendation: warn-only**, with any promotion its own proposal.

**Q2 — The suppression idiom.** `Exception => ()` warns under §2 unless divided. The alternative exempts the unit body again, which 0211's critique showed exempts the widest swallow. **Recommendation: no exemption.**

**Q3 — The test-oracle population.** Most of the 572 hits are test oracles. A per-site suppression spelling is not proposed (the decline index's *Per-site suppression spellings for the statically-certain-hazard lints* row); a test that means both kinds writes `IFailure | Exception`. **Recommendation: no suppression spelling.**

## 7. Limits

- The reach figures are from v0.355's corpora, before any program was written against the marker; they measure the cost of the warning, not the habit it targets.
- r40 has no built-in marker, so nothing in §2 has been probed against a compiler that implements v0.356; 0211's f19 shows only that the divided spellings run.
- The rule reads arm patterns syntactically. A type alias for `Exception`, if the language admitted one in an arm, would need a sentence; §31.2.1's arm-type rule names classes and interfaces only.
