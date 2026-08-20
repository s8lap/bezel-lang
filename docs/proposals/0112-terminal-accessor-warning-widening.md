# Proposal 0112 — The terminal-accessor provenance warning, widened to enumerator-owned yields

| | |
|---|---|
| **Status** | under-review — draft (2026-08-20) — authored by the v0.266 ruling lane for [#3285](https://github.com/s8lap/bezel-lang/issues/3285), which v0.265 routed out of its #2557 train on the ground that widening a normative must-warn set is a design change under `spec-change-process.md` §2 |
| **Tracking issue** | [#3285](https://github.com/s8lap/bezel-lang/issues/3285) (parent: [#2557](https://github.com/s8lap/bezel-lang/issues/2557), [#3047](https://github.com/s8lap/bezel-lang/issues/3047)) |
| **Target module(s)** | **spec** — §29.11's *terminal-accessor provenance warning* paragraph (the predicate clause and the silent-root list) and Appendix C's one-line summary; **stdlib** — §5's must-warn summary sentence. No new member, no signature change, no grammar change, no acceptance change. |
| **Written against** | spec/stdlib/rationale **v0.265** (HEAD `03c3ffc`), plus the v0.266 clarification train this proposal is separated from (stdlib §5's hazard-class rename to *enumerator-owned-element source*; §29.11's adopting-arm silent-list entry; §29.2's invalidation-scope fix). **Quoted:** spec §29.1, §29.2, §29.4, §29.7, §29.8, §29.11, §28.3, §28.6, §28.11, §2.8.2; stdlib §1, §4.4, §5. Re-verify before any ruling: spec §29.2's three bullets, §29.4's frame-resident-source sentence, §29.11's whole warning paragraph, Appendix C's §29.11 line; stdlib §5's class-element paragraph and its `GroupBy` paragraph. |
| **Currency** | v0.265. If the v0.266 clarification train has landed at ruling time, the hazard-class term this proposal uses (*enumerator-owned-element source*) is already normative and §29.11 already carries the adopting-arm under-approximation note; if it has not, this proposal's design is unchanged but its §2 must also carry that renaming. |
| **Origin** | #3285's item 2 (*the predicate*), with its "related and near-identical in kind" paragraph (materializers, `IGrouping` receivers) taken in rather than left: #3285 itself observes that *"a single proposal could reasonably take all of it."* Item 1 (*the term*) is **not** here — it is separable, is a clarification, and is proposed to land at v0.266 without this proposal. |

*Unprefixed §-references are **language-spec** references at v0.265; **stdlib**, **rationale** and **process** references are marked. References to this document's own sections are written "this proposal's §N". ▲ marks proposed text, landed nowhere.*

---

## 1. Motivation

**The warning names the wrong thing.** §29.11's v0.243 predicate warns when the traced iterator has *"at least one … reachable `yield return` site [that] classifies as an enumerator-owned **fresh** yield (§29.2's **first** owned-class-typed bullet: a fresh producer or `move` result yielded from a borrowing iterator)"*. But the reason the borrow is dead has nothing to do with freshness. §29.11 states the reason itself, in the next sentence: *"the element is **owned by the terminal's internal enumerator**, which the terminal disposes before returning"*. Ownership by the enumerator's frame is the hazard; minting is one of **two** ways §29.2 admits of getting there.

**The other way is the one the spec recommends.** §29.2's first bullet closes with: *"To hand the enumerator a value already bound to a named owned local, yield a producer expression instead, or **adopt-then-yield** — `using var l := move line; yield return l;` — which pins the value in the frame and **yields a warning-free borrow**"*. §29.2's third bullet then says what happens to it: *"The binding stays owned in the frame and is discharged by the ordinary §28.3 rules … and on early abandonment disposed by §29.7's pass"*. So the frame owns it; §29.7 disposes it when the terminal disposes the enumerator; the accessor's result is dead on arrival — and the predicate, anchored to the *first* bullet, does not see it.

The shape, minimal and complete (`Doc`/`Parse`/`Title` illustrative):

```bezel
public static method Load(paths: IEnumerable<string>): IEnumerable<Doc>
  for var p in paths do
    using var d := Parse(p);          // Parse(p: string): owned Doc
    yield return d;
  end;
end Load;

public static method FirstTitle(paths: IEnumerable<string>): string
  var first := Load(paths).First();   // silent at v0.265
  return first.Title;                 // DisposedAccessException in checked mode (§28.2)
end FirstTitle;
```

Change `using var d :=` to `var d := new Doc(p);` and the identical program warns. The two differ in the producer's *diagnostics* (the `using var` form is warning-free on the producer side, the plain-local form draws §28.3's must-use auto-drop warning), not in the consumer's fault. **The silent shape is the one with fewer producer-side warnings — so the idiom the spec teaches is the idiom the warning misses.** That is the motivation in one sentence.

**Two adjacent silences, same structural cause.** §29.11 already records the materializer stop as *"an under-approximation, not a soundness claim (v0.265)"*, and stdlib §5's `GroupBy` paragraph records that *"a grouping over a fresh-yield source carries dead borrows exactly as a materializer does"*. Both are provenance stops at a node the trace could pass through:

```bezel
using var docs := Load(paths).ToArray();      // every slot a dead borrow (§28.11's borrow-slot row)
var a := docs.First();                         // silent

for var g in Load(paths).GroupBy(lambda(d) => d.Kind) do
  var rep := g.First();                        // silent
end;
```
The `GroupBy` case is the sharper of the two: stdlib §5 says `GroupBy` *"materializes fully"* by exhausting the source, so every group's every element borrow is dead **before the first group is handed out**.

**Why the loop-variable framing in #3285 overstates the obstacle.** #3285 attributes the `IGrouping` silence to *"an interface-typed loop variable defeat[ing] provenance tracing"*. §29.11's silent list actually names *"an interface-typed **parameter**, an interface-typed **field read**, or a **call result the trace cannot resolve**"* — a loop variable bound to a *traced source expression* is none of those, and §29.11 says of the parameter case that *"the affine tracking is **by provenance, not by type**"*. The trace is intra-body flow; the interface type is irrelevant to it. So the `IGrouping` case is reachable by one added propagation rule, not blocked by a type-system limitation. This proposal takes it.

**Why now, and why a proposal.** v0.265 removed the *false justification* for the materializer stop but deliberately left the predicate alone, recording the reason in its changelog: *"widening a normative must-warn set is a design change, and the two widenings this train identified are routed out as #3285 rather than taken here."* This is that record.

## 2. The design ▲

### D1 — The fact the predicate reads: **enumerator-owned yield**, with two arms

▲ A reachable `yield return` site of a **borrowing** iterator (return type `IEnumerable<T>`/`IEnumerator<T>`/`IAsyncEnumerable<T>`/`IAsyncEnumerator<T>`) classifies as an **enumerator-owned yield** iff the yielded element is owned by the iterator's own state-machine frame while the consumer holds it. Exactly two arms, both of them §29.2's existing bullets read for their ownership consequence:

- **Minting arm** *(v0.243's, unchanged)* — the operand is a fresh producer (a `new` expression, an `owned`-returning call, any unbound owned temporary) or a `move` result: §29.2's first owned-class-typed bullet. *"The enumerator owns the most-recently-yielded value; the next `MoveNext()` … disposes the previous value before producing the next."*
- **Adopting arm ▲** — the operand is an **unmarked read of a name** denoting a **frame-resident owned binding** of the iterator body: §29.4's own list, verbatim — *"an owned local, a `using var`, or an `owned` parameter of the iterator body"* — classified by §29.2's borrowed-expression bullet. *"The binding stays owned in the frame and is discharged by the ordinary §28.3 rules … and on early abandonment disposed by §29.7's pass."*

Per iterator body, the fact is a tri-value over its reachable yields: **none** / **some** / **all** are enumerator-owned. This is the same shape of fact v0.243 already reads (it drives the certain-vs-may-be-dead diagnostic wording); only the arm set widens.

**Anything else is not an enumerator-owned yield** — in particular a re-borrow of a parameter-sourced collection's element, which is the dominant filter/project iterator shape and stays silent (this proposal's §5 attack A1).

### D2 — The predicate

▲ §29.11's clause changes in exactly one phrase. Old (v0.265):

> … roots at a **directly-called iterator method, compiled in the same compilation unit, at least one of whose reachable `yield return` sites classifies as an enumerator-owned fresh yield** ([§29.2]'s first owned-class-typed bullet: a fresh producer or `move` result yielded from a borrowing iterator).

New ▲:

> … roots at a **directly-called iterator method, compiled in the same compilation unit, at least one of whose reachable `yield return` sites classifies as an enumerator-owned yield** — one whose element the enumerator's own frame owns while the consumer holds it, in either of §29.2's two arms: **minting** it (a fresh producer or a `move` result, §29.2's first owned-class-typed bullet, disposed on the next advance) or **adopting** it (an unmarked read of a **name** denoting a frame-resident owned binding — an owned local, a `using var`, or an `owned` parameter, [§29.4]'s list — §29.2's borrowed-expression bullet, disposed by [§29.7]'s pass when the enumerator is disposed).

Everything else in the paragraph is unchanged: the member set, the `Aggregate` receiver-leg qualifier, the `directly-called` bound, the same-compilation-unit bound, the deferred-operator trace (*"callback-free and callback-taking alike"*), the remedy list, and the closing *"The warning changes no acceptance."*

The **mixed-yield wording generalizes unchanged**: *"When every reachable yield of the traced iterator is fresh, the diagnostic states the fault as certain; when yields are mixed …, it states that the element may not survive the call"* becomes *"when every reachable yield … is enumerator-owned"* / *"when yields are mixed (some re-borrows of parameter-sourced collections, some enumerator-owned)"*.

### D3 — Two pass-through nodes: the materializer and the grouping

▲ **At a class or interface element type only**, the provenance trace does not stop at:

**(a) a materializer result** — `ToArray()` / `ToSet()` / `ToDictionary(…)`. Ground, already normative: at a class or interface element type these build **borrowing** collections whose slots are borrows of the enumerated elements (§28.11's borrow-slot row; stdlib §4.4; stdlib §5's *"Materialization gives the result a new **container**, never a new **owner** for the elements"*). The container's ownership says nothing about the elements', so the trace follows the elements.

**(b) an `IGrouping<TKey, T>` value obtained as the iteration variable of a `for` whose source expression is itself traced.** Ground, already normative: stdlib §5's `GroupBy` paragraph — *"At a class or interface element type that buffer holds **borrows** of the source's elements, as every borrowing collection does (§28.11): the *grouping* survives the outer sequence, its *elements* only as long as their own owner does."*

▲ **They are pass-through nodes, never roots.** The verdict is always the **root's**. A materializer or a grouping over a concrete-collection root stays silent. This is the whole false-positive guard and it is stated as a rule, not left to an implementation: *the presence of a materializer or a grouping in the chain is never itself evidence of anything.*

At **value**, **`string`**, or **runtime-managed** element types both nodes remain **stops** — no borrow is produced, as §29.11's existing element-type clause already says.

The loop-variable propagation in (b) is the minimal general rule that reaches it: ▲ *the iteration variable of a `for` whose source expression's provenance the trace resolves carries that provenance, where the variable's static type is an enumeration-family type (`IEnumerable<T>`, `IGrouping<TKey, T>`, `IOrderedEnumerable<T>`).* It is intra-body flow of exactly the kind §29.11 already performs.

### D4 — The under-approximation bounds, stated the way v0.243 stated its own

▲ The widened predicate is **still an under-approximation, and this is the exhaustive list of what stays silent.** Every entry says *why*, and says whether the silence is forced or chosen.

1. **A cross-module iterator.** *Chosen, unchanged from v0.243.* §2.8.2's artifact interface carries no yield-classification fact; carrying it remains the recorded liftable, not taken here (this proposal's §3 and Q4). Consequence stated plainly: **a same-unit adopt-then-yield iterator now warns while its cross-module twin does not**, so the observable same-unit/cross-module gap **widens** with this proposal. That is a real cost (this proposal's §4), not a neutral fact.
2. **A yield that reaches a frame-resident owned binding only through a field, an indexer, or an iteration variable** — `yield return seen[i];`, `yield return doc.Header;`. *Chosen, and the reason is a false positive:* see this proposal's §5, attack A2. Recorded liftable, explicitly not taken.
3. **An indirectly-called iterator** — through a function value or virtual dispatch. *Forced by the `directly-called` bound, unchanged.* (This is §22.13's own bound for the analogous graph: *"Calls through function values, virtual dispatch, and code outside the compilation unit contribute **no** edges."*)
4. **A receiver reached from an interface-typed parameter, an interface-typed field read, or a call result the trace cannot resolve to a same-unit iterator.** *Unchanged; the runtime backstop remains the guard.*
5. **An `IGrouping` or other enumerable-typed value reached from a parameter, a field, or an unresolvable call** — D3(b) propagates only from a *traced* source expression.
6. **A concrete collection type root** — sound, and must stay silent (§5, attack A4).
7. **Any receiver whose element type is value, `string`, or runtime-managed.** *Forced — no borrow is produced.* Unchanged.
8. **A materializer or grouping at a value / `string` / runtime-managed element type.** Stop, per D3.

▲ Uniform closing sentence, in v0.265's own idiom: *the silences above are under-approximations, not soundness claims; the checked-disposal backstop (§28.2) remains the guard everywhere the warning is silent.*

### D5 — Why the adopting arm keys on a **name read**, not on a source chain

This is the design's load-bearing restriction and it exists to buy a zero-false-positive claim. The tempting generalization — *"the yielded borrow's derived source chain roots at a frame-resident owned binding"* — is **wrong**, because §28.6's receiver-sourced notion routes a borrow reached through a field or an indexer to the *receiver*, regardless of whether the receiver owns the value:

```bezel
public static method Index(docs: IEnumerable<Doc>): IEnumerable<Doc>
  using var seen := new Array<Doc>();     // frame-resident owned binding
  for var d in docs do
    seen.Add(d);
    yield return seen[seen.Count - 1];    // borrow reached through an indexer
  end;
end Index;
```

`seen` is frame-resident and frame-owned, but `Array<Doc>` is a **borrowing** collection: its slots are borrows of `docs`'s elements, which outlive the enumerator. `Index(src).First()` is **sound**, and a source-chain predicate warns on it. Keying on a *direct read of a name* excludes the whole class: when the operand is the binding's own name, the yielded value **is** the value the frame owns, and the frame disposes it. Cost: D4 item 2. Recorded as a choice per `CLAUDE.md` rule 4, and put to the owner as Q1.

### D6 — What does not change

Acceptance (nothing newly compiles or fails to compile); the diagnostic's attachment site (the accessor call); the member set; the remedy list; §29.2's classification of any yield; the runtime `InvalidOperationException` backstop; every stdlib signature.

## 3. What is **NOT** proposed

- **The term fix (#3285's item 1).** Separable, a clarification, and proposed to land at v0.266 independently of this proposal. Nothing here depends on it beyond vocabulary.
- **Any rejection.** The hazard is **provenance-dependent, not type-dependent** — v0.265's own ground for declining alternative (b) of #2557 (*"`ToArray()` over an `OwningArray<Connection>` is sound and pervasive"*). Nothing here rejects a program.
- **Warning at the materializer itself.** `Load(p).ToArray()` produces a collection **all** of whose slots are dead even with no accessor in sight — including `for var x in Load(p).ToArray() do … end`. That is arguably a worse hazard than the one this proposal fixes, but it is a different member set, a different attachment site, and it fires on the whole materialize-then-loop idiom whose remedy is a different remedy. Out of scope; put to the owner as **Q3** rather than left silent.
- **Lifting the same-unit bound.** D4 item 1; **Q4**.
- **Source-chain tracing through fields, indexers, and iteration variables.** D5; **Q1**.
- **Anything about owning iterators.** stdlib §5 declares `public extension IEnumerable<T>` only, so no §5 accessor exists on `IOwningEnumerable<T>`/`IOwningEnumerator<T>` and the owning arm of §29.2 cannot reach this predicate at all. Stated so no reviewer looks for it.
- **Any change to §29.11's single-consumption rule**, its consumption-point list, or its runtime guard.

## 4. Costs

**Compiler work — the only substantial one, and it is small because it reuses two existing machines.** (i) The per-iterator yield classification already exists for v0.243's fact; the adopting arm adds one test — *is the operand a bare name resolving to an owned local, a `using var`, or an `owned` parameter of this body?* — decidable in the iterator's own body check. (ii) D3's pass-through requires the trace not to terminate at three named stdlib members and to propagate through one loop-variable binding; both are the same intra-body flow §29.11 already runs for single-consumption.

**Diagnostic churn — the real risk, and it must be measured, not asserted.** Any same-unit adopt-then-yield iterator whose consumer calls a class-element terminal accessor becomes newly-warned. The corpus check is a landing obligation, not a proposal claim: grep `tests/conformance/` and the doc examples for `yield return <name>;` in a borrowing iterator, and for accessor calls over same-unit iterator receivers. **A widened must-warn set whose false-positive rate is unmeasured is worse than the gap it closes** — this proposal's §5 argues the FP rate is zero *by construction*, and the corpus check is what would falsify that.

**The same-unit/cross-module gap widens** (D4 item 1). Moving a warned iterator into a library still silences the warning — already true at v0.243, now true of more shapes.

**A pedagogical seam to close in the same landing.** §29.2 calls adopt-then-yield *"the warning-free form"*. After this proposal, the *producer* is still warning-free and the *consumer's accessor* warns. One sentence at §29.2 must say so, or the two sections read as contradicting each other. (This is a landing obligation of the graduation, listed here so it is not discovered by a reader.)

**Zero on the other axes:** no language change, no grammar change, no new §3.6 property, no new stdlib member, no signature change, no kernel contact (§28/§30.5 untouched — the ruling reasons *from* §29.2's existing classification and §28.11's existing slot policy without changing either).

## 5. Interactions

- **#2557 / v0.265 (stdlib §5, materializing is not a remedy).** The direct parent. This proposal takes the two widenings v0.265 identified and routed out. Its declined alternative (b) — type-keyed rejection of the materializers — is the reason D3 is a *trace* and not a *rejection*.
- **#3047 / v0.243 (the warning itself).** The predicate this widens. Its structure — member set, silent-root list, stated under-approximations — is the template D4 follows deliberately.
- **spec §29.4** — supplies the frame-resident-source list D1's adopting arm quotes (*"an owned local, a `using var`, or an `owned` parameter of the iterator body"*). Using §29.4's own list rather than a new one is what keeps this a re-reading of landed rules.
- **spec §29.7, §29.8** — the disposal mechanics that make the adopting arm's element dead (§29.7's reverse-order pending-cleanup pass; §29.8's frame disposal of an un-moved-from `owned` parameter).
- **spec §28.6, §28.11** — §28.6's receiver-sourced notion is why D5 restricts to name reads; §28.11's borrow-slot row is D3(a)'s ground.
- **stdlib §5's `GroupBy` paragraph** — D3(b)'s ground, and the source of the "dead before the first group is handed out" sharpening.
- **stdlib §1's block-by-name note** — worth checking at graduation: a receiver whose concrete type declares its own `First`/`Last`/`Contains`/`Count`/`ToArray` resolves to *that* member, not the §5 operator, so the warning does not apply there. `Array<T>` declares none of `First`/`Last`/`Single`/`ElementAt`, so D3(a)'s `docs.First()` does reach the §5 accessor.
- **proposal 0105 §1 (body-locality)** — this proposal's §6.
- **Guide 5 §4/§6/§8** — teaches this hazard and its remedies; re-pinned at v0.265. A graduation moves the pin again, unversioned per process §5 item 5.
- **process §2** — the routing that produced this document.

## 6. Body-locality classification (proposal 0105 §1)

▲ **Fact-discharged** — *not local*, and it was not local at v0.243 either. Stated precisely, since 0105's checklist line asks for the facts by name:

**The fact consumed:** per iterator body, its **yield-ownership valuation** — a tri-value in {no enumerator-owned yield, some, all} computed over the body's reachable `yield return` sites, each site classified by §29.2's bullets against expressions and bindings **of that body alone**. It is a per-body fact in 0105 §1's strict sense: yielded at the body's own single check, and consumable *without* any later pass over the body's content (it is a three-valued summary, not a retained AST or retained token text). No re-elaboration per use; no information from a *later* declaration is needed to check an *earlier* one — the verdict is a lookup of a summary at the call site, exactly the shape 0105 §1 discharges §22.13's static-initialization graph with.

**What the widening changes:** the fact's **valuation**, not its category or its computation cost. v0.243 already reads a per-iterator yield summary; D1 widens which yields set the bit. **The category is unchanged from the rule this amends** — which is worth stating because v0.243 predates the 0105 checklist line (landed v0.262) and therefore never stated one; this proposal supplies it retroactively for the rule it widens.

**D3 adds no new body fact.** Materializer and grouping pass-through are decided from (i) the call site's own expression shape — intra-body flow, **local** — (ii) the receiver member's stdlib identity, a signature-level fact, and (iii) the element type's class-or-interface category, a §3.6 declaration-computable property. Nothing consults the materializer's or the grouping's implementation.

**D5's name-read restriction is also a body-locality property, not only an FP guard.** A source-chain predicate would have to reason about what `Array<T>`'s indexer returns — a fact about *another declaration's* member semantics — where a name read is decided from the iterator body's own binding table.

**The cross-module bound (D4 item 1) is exactly where fact-discharge would become an §2.8.2 obligation.** Lifting it requires the artifact interface to carry the tri-value per public iterator member — a new summary field in §2.8.2 clause 1's contract-record family, in principle already reachable through the definition-site clause's exported generic bodies, but **reading those bodies at the consumer would be a re-elaboration**, which 0105 §1's carve-out does not cover (it covers generic *instantiation*, confined to declaration-computable facts and declaration-fixed behaviors). So the sound lift is *carry the summary as a fact*, never *read the carried body* — and that is why Q4 is a real design question and not a mere effort question.

**No invariant amendment is sought, and none is needed.**

## 7. Questions for the owner

- **Q1 — The adopting arm's shape (D1/D5): a direct name read, or the borrow's derived source chain?** Recommendation: **name read**. It buys a zero-false-positive claim (this proposal's §5), keeps the rule body-local, and costs only D4 item 2's shapes. Honest counter: it misses the natural `using var conn := Open(path); for var r in conn.Rows do yield return r; end;` shape, whose elements *are* frame-owned and *are* dead at a terminal accessor — a real hazard the name-read rule declines to catch. Ruling for the source chain instead requires an answer to §5's attack A2 (a frame-resident *borrowing* collection whose elements outlive the frame), which this proposal does not have.
- **Q2 — D3's pass-through nodes: in this proposal, or a second one?** Recommendation: **in**. They are the same defect, #3285 invites taking them together, and the FP guard (*"pass-through nodes, never roots"*) is one sentence. Counter: they enlarge the change from "one phrase in the predicate" to "a predicate phrase plus a trace rule", and the `IGrouping` propagation is the only genuinely *new* machinery in the proposal.
- **Q3 — Should the materializer itself warn, independently of any accessor?** `Load(p).ToArray()` at a class element type yields a collection of wholly-dead borrows, and `for var x in Load(p).ToArray() do … end` is a silent certain fault with no accessor anywhere. Not proposed (this proposal's §3). If the owner wants it, it is a **separate member set** (`ToArray`/`ToSet`/`ToDictionary`) and a separate diagnostic, and it would fire on the idiom stdlib §5's `GroupBy` paragraph currently *advises* (*"materializing it once … remains the advised shape"*) — so ruling yes also requires revisiting that advice.
- **Q4 — Cross-module (D4 item 1).** Keep the same-unit bound, or carry the yield-ownership valuation as an §2.8.2 interface fact? Recommendation: **keep**, in this proposal — the widening's value is already realized same-unit, and the interface-fact question is a §2.8.2 design question with its own delta-classification consequences (§2.8.6). Named here so the widening does not silently *pretend* to be complete, which #3285 explicitly warns against: *"the existing predicate is already explicitly incomplete across module boundaries, and a widening should not pretend otherwise."*

---

*Proposed README row (Live queue):*

| [0112](0112-enumerator-owned-yield-provenance.md) | [Terminal-accessor provenance, widened](0112-enumerator-owned-yield-provenance.md) — §29.11's must-warn predicate re-keyed from *fresh* yields to **enumerator-owned** yields (minting **and** adopt-then-yield, §29.2's two arms), plus materializer and `IGrouping` pass-through at class element types; adopting arm keyed on a direct name read to keep the FP rate zero (D5's `Array<T>`-indexer counterexample); under-approximation bounds restated in full, same-unit bound kept; no acceptance change, no new member, body-locality **fact-discharged** | draft (2026-08-20) | owner ruling on Q1–Q4 | [#3285](https://github.com/s8lap/bezel-lang/issues/3285) |
```

---

