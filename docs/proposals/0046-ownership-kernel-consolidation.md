# 0046 - The ownership-kernel consolidation revision

**Status:** graduated-v0.199 — superseded — graduated v0.198 (kernel, train T1) + v0.199 (tag passes, trains T2–T6); acceptance audit at [`0046-audit-v0.199.md`](0046-audit-v0.199.md)
**Tracking issue:** [#2033](https://github.com/s8lap/bezel-lang/issues/2033)
**Target:** Docs-only. Spec [§28](../Bezel_Language_Spec.md#sec-resource-management) (a new kernel subsection at the head of [§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows) plus derivation tags through §28.3–§28.14), spec [§30.5](../Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal) and the five ownership paragraphs of [§30.1](../Bezel_Language_Spec.md#sec-async-methods) (tag pass), Appendix [C.3](../Bezel_Language_Spec.md#sec-appendix-c)/C.6 (re-audit, no semantic edits), rationale (one new section recording the kernel's reasons and the axiom-budget discipline; small amendments to [§5.1](../Bezel_Design_Rationale.md#sec-owned-not-borrows)/[§5.6](../Bezel_Design_Rationale.md#sec-the-lattice-direction-at-control-flow-joins)), Guide 01 revalidation pin. **No grammar change** (Appendix A untouched — the revision adds and reorganizes prose, no syntax). **No stdlib change. No compiler work** (the compiler already implements the rules being restated; every discovered text/compiler divergence files its own issue rather than riding this revision).
**Written against:** spec v0.197 (stdlib/rationale at the same revision)

This proposal expands item **P6** of [`future-directions-2026-07-18.md`](papers/future-directions-2026-07-18.md) to graduation depth. Its value is deliberately front-loaded: **§2 and §4 below are the kernel draft and the rule-by-rule mapping themselves** — the first pass a spec-revision author would otherwise have to produce — not a plan to produce them. Where this document sharpens or diverges from the P6 sketch, the divergence is recorded in §12.

## 0. Summary

Spec §28 and §30.5 are sound, probe-hardened, and increasingly shaped like case law: the consume-site freeze, the announced-kill family, the full-expression boundary, the field-restoration rule, three kinds of Pattern A result slot, a nine-bullet admitted-await-positions list — each individually justified, collectively unpredictable. As the P6 motivation puts it, *a reader cannot derive the rule they haven't read*. This revision restates the two chapters as a small kernel — nine principles, K1–K9 — plus a **register of recorded axioms** (this draft finds **fourteen**), and re-derives every named case-law rule from them. The intended semantic delta is **zero**: every rule below is a restatement of v0.197 text, every derivation cites the current rule it reproduces, and any divergence discovered during the rewrite is escalated to its own ruling issue instead of being silently normalized. The axiom count is published as the language's honest complexity budget; future rulings must state which axiom they extend or which K they derive from. The kernel is also the mechanization target for proposal 0047 (the number reserved for P7; that proposal is not yet drafted), so K-numbering is frozen on landing.

## 1. Problem and evidence

All citations verified against v0.197 headings and text.

**1.1 The chapters have grown by accretion of rulings, and it shows.** §28.3 alone now contains: a producers table of ~25 rows; three signature-classification rules layered across five revisions (*Factory methods* v0.60, *Task-family calls produce owned frames* v0.190, *Bare-type-parameter returns* v0.194); the consume-site freeze (v0.180) with a recorded announced-kill boundary (v0.184) whose membership is stated as a list; the full-expression boundary (v0.193) with a `case`-scrutinee extension; unbound-temporary rules (v0.149→v0.193); deferred-activation rules in three passes (v0.179, v0.184, v0.196). §30.5 states three kinds of Pattern A result slot (v0.162/v0.179/v0.184) and enumerates nine admitted await-operand positions. §28.14 carries five bounding-object resolution forms and the seven listed v1 admission bounds (#1421; an eighth ratified choice — self-capture — is promoted into the admission text). Each item is correct and individually reasoned; the *set* is memorized, not derived.

**1.2 The spec itself already gestures at the kernel — halfway.** §30.5's admitted-positions list is prefixed: *"The principle resolves into the following examples of admitted task-expression positions; the list is not exhaustive but illustrates the rule"* — and the v0.182 unification explicitly re-keyed the admission to "§28.3's ordinary borrow machinery." §28.11's element-slot policy states outright that per-collection clauses "are presentations of this table" and that a diverging clause "is a defect in this specification." The mode of the document is *trying* to be kernel-plus-derivations; it just never states the kernel in one place, so each ruling re-derives its fragment locally and the fragments drift stylistically (compare the four separately-worded statements of the residual-trust shape in §28.3's borrowed-field rule, §28.11's store rule, §28.14's quiescence, and §30.1's settlement obligation — the same principle, four spellings).

**1.3 The cost is concrete, not aesthetic.** Three observations from the project's own record:

- *Rules that must be stated once are stated many times, and the copies decay.* The v0.196 static-storage narrowing (property-getter-only) had to be edited into §28.3's borrow-lifetime bullet, §28.3's bare-type-parameter callee side, §28.6's first bullet, and Appendix C.3 — four load-bearing restatements of one decision. The residual-trust posture is likewise spelled independently four times, in four wordings: §28.3's borrowed-field bullet (*"The residual is trust: whether the caller-side source actually outlives the object is the programmer's obligation"*), §28.11's store rule (*"The residual is trust: whether each admitted element's source actually outlives the collection's use of it…"*), §28.14's quiescence (*"the same residual-trust shape as this chapter's borrowed-field rule"*), and §30.1's settlement obligation (*"This is the same residual-trust shape as §28.3's borrowed-field rule and §28.14's quiescence"*) — the last two already apologizing for the duplication by cross-citing it. Every future ruling pays this multiplication.
- *A reader cannot predict the next case.* Why is a Pattern B `await x` exempt from the consume-site freeze while an owning-iterator `yield return x` is frozen, when both are keyword-visible consumes announced by a declared return type (§28.3, *The explicit-`Dispose()` boundary*)? The current text answers with a parenthetical per case. The kernel answers with one criterion (§2, K4/K5: unfrozen ⟺ announced **and** flag-backstopped), from which both placements — and the drop-on-overwrite placement, the third surprise — fall out.
- *Mechanization and explanation are blocked on it.* P7 (proposal 0047) needs a finite rule set to model; P12's derivation-carrying diagnostics need a derivation to carry. Case law provides neither. The "few enough to learn in full" bet (rationale §1) is the language's justifying constraint, and today its truth cannot even be *audited*, because there is no count of what "in full" contains. This proposal produces the count.

**1.4 Why now.** The chapters are the most stable they have ever been — v0.190–v0.196 closed the producer-classification seams, and the R0 soundness runs have probe-hardened the text against the compiler. A consolidation pass over churning text would be wasted; over settled text it is the cheapest it will ever be, and it is the prerequisite on the critical path of P3 (contract static tier), P7, and P12.

## 2. The design: the kernel

The kernel is nine principles, K1–K9, proposed to land verbatim (modulo editorial polish at graduation) as a new head subsection of spec §28.3, titled **"The ownership kernel (K1–K9)"**, with one stable anchor per principle (`sec-kernel-k1` … `sec-kernel-k9`). §28's h4 subsections are unnumbered, so the insertion changes **no section numbering and no existing anchor**. Everything below is a restatement of v0.197 rules — the bracketed citations point at the current text each clause reproduces; they are the audit trail, and would be trimmed to ordinary cross-references at graduation.

A note on register: the kernel text is written at the spec's density, normative-quality, but it does not *replace* the detailed subsections in this proposal — the staging (§8) lands it additively first, then rewrites the case-law subsections beneath it as tagged derivations.

---

> ### The ownership kernel (K1–K9)
>
> Every rule of this chapter and of §30.5 is one of: a kernel principle below, a derivation from them (tagged `derives: K…` at the rule), or a recorded axiom (tagged `axiom A…`, register at the end of this subsection). A future revision that adds an ownership rule states which kind it is.
>
> **K1 — Value classification.** At every program point, every expression and binding of reference type has exactly one ownership classification:
>
> 1. **owned** — the holder carries the disposal obligation: dispose exactly once, on every path. [§28.3]
> 2. **borrowed** — no obligation; validity is bounded by the borrow's source set (K2). [§28.3]
> 3. **moved-from** — the obligation has transferred out; the binding or slot is unreadable until reassigned, and discharge machinery skips it. [§28.3, §28.8.1]
> 4. **maybe-consumed** — the K7 join of owned with moved-from or explicitly-disposed; unreadable; scope-end disposal is per-path. [§28.3, v0.180/v0.184]
>
>    Two auxiliary bookkeeping states complete the analysis: **uninitialized** (declared, unassigned — unreadable, §7.9) and **polymorphic-null** (`var x: T? := null;` before the first non-null assignment — adopts the state of that assignment; a null arm or RHS is compatible with either target, having no value to own). [§28.3 producers, null row]
>
> 5. **Atoms.** Fresh productions are owned: `new`, reference-typed collection literals, `owned`-returning calls, `move` results, owned operator results. Reads of storage lend: a field, property, indexer, or slot read is a borrow whose source is the storage's holder. Reads of named bindings lend unless the position consumes (K3): an unmarked read of an owned binding is a borrow of the binding. [§28.3 producers]
> 6. **The runtime-managed exemption.** Values of runtime-managed type (`string`, boxed value types, function values) and of ordinary value type carry no classification and no obligation; `owned` markers over them are inadmissible or erased per instantiation (§28.1, v0.196). Trivial-dispose class instances carry the classification, but their obligation's content is empty, so discharge is not demanded of the program. [§28.9]
> 7. **One holder.** An obligation has exactly one holder at a time; every transfer moves it, never copies it. Copy-semantics holders (record fields) therefore cannot hold obligations. [§28.5]
>
> **K2 — The source algebra.** Every borrow carries a **source set**. A source is one of:
>
> - a **named location**: a local binding, a parameter, `this`, a field path rooted at a live object, or static storage — the one distinguished *always-live* source (process-lived, disposed only at exit, §22.13);
> - a **frame** (K6): a task, iterator/enumerator frame, or lazy query value, itself an ordinarily-classified value;
> - a **full-expression temporary**: an unbound owned temporary, live exactly to its K8 boundary.
>
> 1. **Weakest source bounds.** A borrow is valid only where *every* source in its set is live; equivalently, it is bounded by its weakest source. [§28.3 borrow-lifetime]
> 2. **Union under composition.** Joins, `??` and arm meets, transitive re-borrows, and frame carriage combine borrows by taking the union of their source sets. [§28.3, v0.180]
> 3. **Escape must be bounded.** A borrow may not reach a holder — a binding of wider scope, a field, a collection, a frame, a return — whose lifetime the checker cannot bound by the borrow's source set. Where scopes are statically comparable (locals, temporaries, loop back edges), violation is rejected. Where they are not, an admission is made only under K4's posture: a structural, decidable check at the site plus a recorded residual obligation with a named backstop. [§28.3 borrow-lifetime; §28.11 store rule]
> 4. **Signature-level re-establishment.** A callee's declared signature announces its result's source set as a function of the call's receiver and arguments — the bare-type-parameter source set (§28.3, v0.194), the deferred-activation frame's carried sources (§28.3, v0.184/v0.196) — and every caller re-establishes the set against its own actuals. A borrow whose source no caller could re-establish (a scope-bound local, a temporary, an unannounced parameter source) may not be returned. [§28.3]
>
> **K3 — Discharge and consumption.** The obligation on an owned value is discharged at exactly one of the **owned-consuming positions**, enumerated here once:
>
> 1. store into an `owned` field or owning-collection slot, including through a `set(owned)` accessor [§28.3, §23.3];
> 2. `return` under an `owned` return type, or — non-`async` bodies — under an unmarked `Task`-family return type [§28.3, v0.190];
> 3. an `owned` argument under the call-site marker [§22.3];
> 4. a marked binding or assignment transfer — `var y := owned x;`, `y := owned x;`, `using var y := owned x;` [§28.3, v0.180];
> 5. the operand of a Pattern B `await` [§30.5];
> 6. the owned parameter-or-plain-local source of a Pattern A chaining `return await` [§30.5, v0.184];
> 7. the operand of `yield return` in an owning iterator [§29.2, v0.184];
> 8. the operand of `throw` [§31.1, v0.182];
> 9. the announced-consume discard `_ := x;` [§17.13, v0.184];
> 10. an explicit `Dispose()` (or `x?.Dispose()` / `x!.Dispose()`) on an owned receiver [§28.6];
> 11. reassignment of an owned location — a K3 consume of the *displaced* value, disposed after the RHS is evaluated (drop-on-overwrite) [§28.7];
> 12. for move-only values, any store into an owning location [§15.3, §28.3].
>
>    Three principles govern the enumeration:
>
> 13. **Announcement.** A consume is spelled by the transfer marker (`owned x`) except where the target or keyword already announces it — a declared `owned` slot or return, a `set(owned)` accessor, an unmarked `Task`-family return, an owning iterator's declared interface, `throw`, `await` of an `OwningTask<T>`, the discard. An **unmarked read never consumes**: `var b := a;` is always a borrow. Fresh producers need no marker anywhere (no live binding is invalidated). [§28.3 move semantics]
> 14. **Affinity.** An owned value that reaches its scope's end undischarged and not moved-from is disposed there implicitly; the implicit drop of a plain-`var` local or undischarged `owned` parameter carries the must-use warning (a lint over a sound program). [§28.3, v0.150–v0.152]
> 15. **Edge parity.** The exception edge performs the same implicit disposal as the normal edge, on the single establishment-ordered cleanup stack; the only difference is the lint. Unbound temporaries join the same stack at their production. [§28.3, §28.10, v0.182/v0.184]
>
> **K4 — Kills, announcement, and the backstop posture.** A **kill** ends a value's life under its current holder: a *disposing* kill runs `Dispose()` and sets the per-instance disposed flag; a *transferring* kill leaves the value alive under a new holder, setting nothing. The posture, in three clauses:
>
> 1. **The backstop.** Disposal is idempotent and flag-setting; in checked disposal mode any later use through any alias faults deterministically as `DisposedAccessException`, and unchecked mode is the same language with the check elided. This is the runtime backstop every trust admission names. [§28.2]
> 2. **Announced kills.** A kill that is both **keyword-visible at the kill site** and **flag-backstopped** is permitted while borrows of the value live: the announcement is the reader's notice and the flag is the safety net. The closed set, with each kill's disposal moment:
>    - explicit `Dispose()` (and the `x?.Dispose()` / `x!.Dispose()` spellings) — disposes at the call [§28.3];
>    - the discard `_ := x;` — disposes at the end of the discard statement [§17.13];
>    - `throw x` — the exception machinery disposes after the matching catch arm completes (or at program termination if uncaught) [§31.1];
>    - Pattern B `await x` on an owned `OwningTask<T>` binding — the await's implicit task disposal, on all outgoing edges including fault/cancel [§30.5].
>
>    Straight-line use of the killed binding afterward is a mandatory warning; a *conditional* kill joins to maybe-consumed (K7) and later use is an error. [§28.2, §28.3]
> 3. **Recorded trust, or nothing.** Where an admission's temporal guarantee can neither be statically compared (K2.3) nor flag-backstopped — the domain clause: a borrowed-from object transferred to, or made reachable from, another domain stays alive, so no flag is ever set — the rule must either reject statically or carry an **individually recorded trust clause**. The recorded clauses (field-held, collection-held, and frame-held borrows versus domain transfer, §28.3/§30.1) are the model's honest limit, and no new admission may be made without recording its clause or its backstop.
>
> **K5 — The freeze.** Consuming an owned **binding** (a local or `owned` parameter; K3 positions) while a live local borrow of its current value exists is a compile error — **unless the consume is a K4 announced kill**. Equivalently: *unfrozen ⟺ announced ∧ flag-backstopped.* (The criterion is today stated piecewise: §28.3's boundary text keys on announcement, and §31.1's throw-operand recording supplies the second predicate in as many words — *"it has exactly the runtime backstop whose absence forced the freeze for transfers."*) Consequences, each currently stated as a separate case:
>
> 1. moves, marked transfers, owned-slot stores, owned returns, and owning-iterator `yield return` are frozen — transferring kills, alive under a new holder, no flag [§28.3, §29.2];
> 2. drop-on-overwrite of a binding is frozen although it disposes — the reassignment is not keyword-visible as a kill of the *old* value [§28.3, §28.7];
> 3. `Dispose()`, discard, `throw`, and Pattern B `await` are unfrozen — announced and backstopped [§28.3];
> 4. a Pattern A chaining `return await x` is unfrozen by **vacuity**: its new holder (the outer task) cannot dispose the chained task before the body, `finally` blocks included, completes — no body borrow can outlive the kill [§30.5, v0.184];
> 5. the freeze is **body-local by construction**: liveness is the backward dataflow over one body, borrows are traced through `using var` pins, live closures, and borrow-bearing record locals, and this is why a closure may not consume its captured owned bindings (K2 + decidability) [§28.3, v0.184/v0.186];
> 6. **fields and collection slots are not frozen**: slot aliases are not body-local, and a slot kill disposes — the K4 backstop is real there; the slot kill *by move* is instead governed by K4.3's domain clause [§28.3, §28.7, §28.11];
> 7. a §28.14 storage admission **strengthens** the freeze over the admitted captures to cover announced kills as well: a stored callback's later invocation is invisible at a dispose site, so the announcement never reaches the reader it exists for [§28.14, §28.3].
>
> **K6 — Frames are values.** A deferred activation — a task (`Task`, `Task<T>`, `OwningTask<T>`), an iterator or async-iterator frame (enumerable or enumerator, borrowing or owning), a lazy query — is a **value** with an ordinary K1 classification that additionally **carries a source set** (K2): the constructing call's receiver, its plain reference-typed arguments, and the capture-set contributions of its plain function-typed arguments, transitively. [§28.3, v0.179/v0.196]
>
> 1. **The bound.** Carried sources must outlive the frame's bound: **settlement** for a task; **completion or disposal** for an iterator frame; the **single consumption point** for a lazy query, which is thereby affine. [§30.1, §29.4/§29.7, §29.11]
> 2. **Announcement by declared type.** Frame-carrying classification is keyed on the callee's declared, pre-substitution return type — a `Task`-family type, a §29.3/§30.7 iterable or enumerator interface, a lazy query — and every caller re-establishes the carried set against its own actuals (K2.4). This is what admits returning a composed frame, and what rejects a returned frame carrying a scope-bound local, `using var`, or temporary. [§28.3, v0.184]
> 3. **Containers: calls mint, storage lends — for tasks.** A call whose declared return type is `Task`-family produces a fresh **owned** frame at every call site; a read of task **storage** (property, field, slot) lends a borrow of the storage; callee-side, the unmarked `Task`-family return type transfers the container like an `owned` return. The keying to the task family alone is axiom A2. [§28.3, v0.190; rationale §12.10]
> 4. **Frames versus the freeze.** Frame-held borrows live in the frame, not in any local borrow binding, so K5 cannot see them; their outlives-obligation is the K4.3-recorded settlement clause, discharged trivially by awaiting before any source's scope exits and structurally by `TaskScope`. The abandonment idioms carry the residual explicitly. [§30.1]
> 5. **Interaction with K8.** A frame sourced by an unbound temporary must itself be consumed to its bound within the same full expression; a frame that escapes the expression is rejected — the reject-not-extend posture. [§28.3, v0.179]
>
> **K7 — The join rule.** At every control-flow join — statement merges, `??`, `if` expressions, expression-position `case`/`when` arms, loop back edges — a variable's or expression's state is the lattice meet, owned ⊑ borrowed:
>
> 1. owned ⊓ owned = **owned**, obligation carried forward (for expression arms: exactly one arm's value exists at runtime, so exactly one obligation);
> 2. borrowed ⊓ borrowed = **borrowed**, carrying the union of the branches' source sets (K2.2);
> 3. owned ⊓ borrowed = **rejected** — no single state is correct, and demoting owned would silently drop an obligation; the program equalizes the branches first;
> 4. owned ⊓ moved-from and owned ⊓ explicitly-disposed = **maybe-consumed** — unreadable after the join, no flow around a back edge into a use; scope-end disposal per-path;
> 5. `null`-literal and bottom-typed (`throw`) arms absorb to the other arm's state.
>
> The direction is deliberate: rejection at 3 rather than demotion is what makes the meet leak-free. [§28.3; rationale §5.6]
>
> **K8 — The full-expression boundary.** A **full expression** is a maximal expression — not a subexpression of any larger expression — at the enumerated positions (initializers, assignment RHSs, expression statements, each *evaluation* of a condition or guard, `for` sources, `return`/`throw`/`yield return` operands, `case` scrutinees). [§28.3, v0.193]
>
> 1. Unbound owned temporaries are disposed at their full expression's end, after their uses; on the exception edge, during unwinding in reverse production order. [§28.3, v0.184]
> 2. Conditions and guards are per-evaluation boundaries: temporaries never accumulate across iterations.
> 3. A `case` scrutinee's boundary is the whole construct — pattern bindings are borrows of the scrutinee, and arm bodies must outlive-be-outlived correctly — in statement position exactly as in expression position.
> 4. A temporary is an admissible borrow source only within its boundary; a borrow or frame that would escape it is **rejected, never lifetime-extended** — the language has no general lifetime-extension mechanism. [§28.3, §9.7; rationale §5.24]
>
> **K9 — Fields are object state, not scope state.** A field's obligation belongs to the object: it is discharged by the destructor cascade (owned fields, reverse declaration order) or by drop-on-overwrite, never by any scope's end, and no scope-end machinery ever touches it. Consequences:
>
> 1. every plain read of an owned field lends (K1.5); the only owned read is `move x.f` — and `move coll[i]` for owning slots — which puts the slot in moved-from state [§28.8.1];
> 2. **the restoration rule**: every control-flow path from a `move` of a field or slot to the enclosing method's exit — normal, `throw`, `yield break`, and every exception edge raised by an intervening throwable expression — must reassign the slot: the object must never escape the method with a hole a destructor could observe [§28.8.1];
> 3. a destructor body is exempt from restoration (teardown: no future read exists), and a constructor's `move` is governed by definite assignment before first assignment [§28.8.1, §7.9];
> 4. static owned fields admit no `move`: the "object" is the process, observable by other code while the method runs, so even a per-method restoration window is a hole [§28.8.1, §22.12];
> 5. drop-on-overwrite (K3.11) is RHS-first, so a throwing RHS leaves the slot untouched and still-owned — restoration idioms (`_resource := fresh;` onto a moved-from slot) are non-throwable [§28.7, §28.8.1].
>
> *(The build-before-take discipline, the throwable-expression classification, and the per-slot conservative tracking for non-constant indices are derivations under K9, retained in §28.8.1.)*

---

### 2.1 The axiom register (proposed normative list; reasons live in the rationale)

Fourteen axioms. Each is a rule the kernel cannot derive — a recorded design decision with its reason — and each is the extension point a future ruling in its area must name. Ordering is by home section.

| # | Axiom | Home | Recorded reason (one line; full record in rationale) |
|---|---|---|---|
| A1 | Sourceless static class-typed returns are treated owned, with the factory warning | §28.3; rationale §5.7 | Compatibility bridge for unannotated signatures; the safe default, sound only jointly with the callee-side borrow-return prohibition. **Proposal 0048 retires this axiom**; see §5 and §6.1. |
| A2 | Calls-mint is keyed to the `Task` family only; iterable calls classify under K1 atoms + A1 | §28.3 v0.190; rationale §12.10 | Uniform calls-mint is unsound for iterables — the stored-collection-view idiom (`Items(): IEnumerable<Item>` returning `_items`) is legitimate and common. |
| A3 | The two-container async design: `Task<T>` (class T) = shared borrows, `OwningTask<T>` = single owned transfer | §30.5; rationale §12.1–§12.2 | A surface design choice (fan-out vs. acquisition); the kernel checks both, forces neither. |
| A4 | Accessor faces are lend-only: properties/indexers carry no `owned`; a getter may not return a fresh owned value | §28.6, §23 | Signature predictability: every accessor read classifies as a borrow with no marker position available; factories are methods. |
| A5 | `using var` is a scope pin: non-reassignable, never movable out | §28.10; rationale §5.23 | Declared-intent form; scope-pinning is its entire meaning (kept strict deliberately at the affine transition). |
| A6 | `Task`/`Task<T>` may not be discarded; `Task.FireAndForget` is the routed spelling | §17.13, §28 preamble; rationale §5.23 | Semantic, not memory: abandoning a running operation is a decision, not a disposal. |
| A7 | No transfer marker exists in arm or operand position: a named owned binding as a `??`/`if`/`case` arm, or as an operator operand, is a borrow arm | §28.3 producers (`??` row), §22.3 | Recorded liftable bound; keeps no-silent-consume (K3.13) uniform until a marker spelling is designed. |
| A8 | The transit exception: element borrows read from a *borrowing-family* collection may be explicitly disposed, keyed on the source's static type | §28.6; rationale §5.3/§5.1 | The element's true owner is structurally unknowable; the caller-vouched escape hatch (manual form of `DisposeElements()`), backstopped per K4.1. |
| A9 | The two collection families: borrowing by default, owning as separately named types | §28.11; rationale §5.3 | The recorded v0.97 resolution; the `<owned T>` merged form was tried (v0.88–v0.96) and abandoned. |
| A10 | Stdlib certification grants: `CancellationToken`'s source-outlives-settlement certification; §30.11's legacy completion-source admission | §30.1, §30.8, §30.11 | A named allowlist of trust granted to specific stdlib types — by construction not derivable; each entry normatively stated at its type. |
| A11 | The §28.14 bounding-object resolution forms (1–5) and the ratified v1 admission bounds | §28.14 (#1421) | Decidability engineering: the chosen static story for "capture source outlives bounding object"; several bounds marked liftable. |
| A12 | Pattern A chaining's disposal-timing dispensation for a `using var` source (deferral to the outer task's teardown) | §30.5 kind 2, v0.179 | A recorded timing carve against A5's pin — not a move (the slot lives in the frame); exactly-once is preserved transitively. |
| A13 | Move-only inline-array whole-value reassignment does not dispose displaced slots (warning-backed) | §28.7, §15.3 (#1385) | Recorded debt, tracked: interacts with the open #747 design; the one carve-out on the exactly-once claim. |
| A14 | No `owned` generic type arguments; owning semantics live in named types | §28.1, §14.4; rationale §5.3/§5.11 | The v0.88–v0.96 substitution machinery failed in practice; named types capture the distinction without it. |

Two candidate axioms were examined and classified **derived** instead, recorded here because the classification is the work: the v0.196 *property-getter-only* static-storage admission derives from K2.4 + A1 (a storage-returning static *method* is signature-indistinguishable from a factory, so callers would mint a second owner — reject; the property face's `T.P` read is the sound dual), and the *maybe-consumed* join derives from K7 with the v0.184 free-precision argument (per-path tracking exists regardless; rejecting the read costs nothing and is not a borrow checker).

### 2.2 The claimed payoff, demonstrated: deriving rules you haven't read

The P6 motivation's failure mode is *"a reader cannot derive the rule they haven't read."* Two checks that the kernel fixes it — each answers a question by kernel computation alone, then confirms against the section that states the answer today.

**"May I dispose an element I'm iterating out of an owning collection?"**

```bezel
for var conn in pool do        // pool: OwningArray<Connection>
  conn.Dispose();              // ?
end;
```

Kernel derivation: default iteration reads slots, and slot reads lend with the collection as source (K1.5) — `conn` is a borrow. `Dispose()` is a K3.10 consume, and consumes act on owned holders. Borrow ≠ owned ⇒ rejected; the slot-coherent spellings are the ownership-moving operations (`Remove`/`RemoveAt`, the indexer-set, `move` on the slot) or owned iteration via `Drain()`. Confirms §28.6 (*Who may call `Dispose()`*) — including its transit exception: had `pool` been a borrowing-family `Array<Connection>`, the disposal would be admitted, and the kernel says why that cannot derive — it is axiom A8, the recorded escape hatch, not a computable case.

**"May I send a value to another domain while a span over its buffer is live?"** Channel `Send` and spawn arguments are K3.3 consumes; a live span local is a live local borrow of every carried source (K5.5's record-local tracing); the consume is transfer-shaped — the buffer stays alive in the receiving domain, so it is neither announced nor flag-backstopped — hence frozen (K5). Confirms §28.3's consume-site freeze, v0.186 span clause, and explains the *reason* the current text states as history: this is the exact no-backstop case (K4.3) that forced the static rule.

Every Bezel fragment in this proposal is current syntax traced against v0.197; nothing here proposes new surface.

### 2.3 What the kernel deliberately does not cover

The kernel is the **ownership/borrow/lifetime** model. Three adjacent systems are out of its scope and are cited, not restated: **sendability and the domain memory model** (§30.13–§30.15 — the kernel touches them only through K3's consume positions at spawn/send and K4.3's domain clause; a second kernel for §30.14 is 0047's concern, not this revision's); **nullability** (§7); and **definite assignment** (§7.9 — K1's uninitialized state defers to it). The scoped-construction rules for class-typed `scoped` parameters (§28.14, v0.180) are covered by K2+K5+A11 exactly as the callback form is.

## 3. Grammar delta

None. The revision adds and reorganizes normative prose; Appendix A is untouched, and no token, production, or admissibility position changes.

## 4. The mapping table

The heart of the revision, and its falsifiable claim: every named rule of §28.x/§30.5 (and §30.1's five ownership paragraphs) is **KERNEL** (restated as a K clause above), **DERIVED** (one-line derivation from the named K's), or **AXIOM** (register entry). Forty-eight named rules; the granularity is the named rule/list/paragraph, matching how the spec itself titles them. A rule tagged KERNEL is one the kernel *absorbs* — its section becomes a cross-reference plus examples; a rule tagged DERIVED keeps its statement, gaining the derivation tag.

| # | Rule (as named/stated today) | Citation (v0.197) | Class | Derivation / reason |
|---|---|---|---|---|
| 1 | Idempotent `Dispose()`, disposed flag, checked/unchecked modes | §28.2 | KERNEL | Absorbed as K4.1 (the backstop machinery). |
| 2 | Straight-line use-after-kill = mandatory warning; conditional kill ⇒ error | §28.2, §28.3 (v0.184) | DERIVED | K4.2 (announced-kill posture) + K7.4 (the join is maybe-consumed). |
| 3 | Producers table, base rows (fresh productions owned; storage and binding reads borrow) | §28.3 *Producers* | KERNEL | Absorbed as K1.5; the table remains as the worked enumeration. |
| 4 | Local ownership inferred, never annotated | §28.3; rationale §5.5 | KERNEL | K1 tracks states; annotations exist only where signatures need them (K2.4). |
| 5 | Polymorphic null state; null compatible with either target | §28.3 null row | DERIVED | K1 aux state + K7.5 (null absorbs; no value, no obligation). |
| 6 | Reassignment state-match (owned LHS ⇐ owned RHS; borrowed LHS ⇐ borrow) | §28.3 | DERIVED | K1.7: a mismatch would drop or double-hold an obligation; K3.11 covers the displaced value. |
| 7 | Mixed-merge rejection + the two equalization patterns | §28.3; rationale §5.6 | KERNEL | Absorbed as K7.3; patterns stay as examples. |
| 8 | Merged borrows carry the union of source-liveness constraints | §28.3 (v0.180) | KERNEL | Absorbed as K2.2/K7.2. |
| 9 | owned ⊓ moved/disposed = maybe-consumed; per-path scope-end disposal | §28.3 (v0.180/v0.184) | KERNEL | Absorbed as K7.4. |
| 10 | `??`, `if`-expression, and `case`/`when` arm meets | §28.3 rows (v0.180/v0.184) | DERIVED | K7 applied at expression joins + K8 (arms are one full expression; exactly one arm runs, so owned ⊓ owned is leak- and double-free). |
| 11 | A named owned binding as an arm is a borrow arm (no arm marker) | §28.3 `??` row | AXIOM | A7 (liftable: no marker spelling in arm position). |
| 12 | Factory methods / sourceless returns: owned-with-warning | §28.3; rationale §5.7 | AXIOM | A1. |
| 13 | Callee-side borrow-return prohibition; receiver-sourced and static-property-getter admissions (v0.196 narrowing) | §28.3 borrow-lifetime; §28.6 | DERIVED | K2.4 + A1: only caller-re-establishable sources (receiver; process-lived static storage through its property face) may be returned; a parameter-sourced or method-faced storage return would collide with A1's caller-side minting — two owners. |
| 14 | Task-family calls produce owned frames; unmarked `Task`-family return transfers the container | §28.3 (v0.190) | DERIVED / AXIOM | K6.3 (calls mint, storage lends); the keying to the task family alone is A2's row — the iterable analogue deliberately classifies under K1 atoms + A1 instead. |
| 15 | Bare-type-parameter returns: signature source set; empty set falls to factory | §28.3 (v0.194) | DERIVED | K2.4 (potential source set = receiver + T-reaching plain args, re-established per call); empty set ⇒ A1 governs, with the two-owners closure at static storage per row 13. |
| 16 | Iterator convention: no factory warning; caller-owned frame | §28.3 | DERIVED | K6.2: the declared iterable interface is the announcement; the warning would be noise over an announced transfer. |
| 17 | Move semantics: transfer markers; announced targets need none; unmarked reads never consume | §28.3 (v0.169/v0.180) | KERNEL | Absorbed as K3.13. |
| 18 | Captured owned bindings are borrow-state inside closures | §28.3 (v0.184) | DERIVED | K2 (capture = borrow of the enclosing scope) + K5.5 (a closure-side consume would be a kill no body-local liveness can see — decidability). |
| 19 | Borrow escape rules: no owned-return of borrows, field-store admission, wider-scope/back-edge rejection, closure-escape forms | §28.3 borrow-lifetime bullets | DERIVED | K2.3: reject where scopes compare; admit with K4.3-recorded trust where they cannot (field-held, collection-held); the three closure forms are exactly the boundable holders. |
| 20 | The consume-site freeze | §28.3 (v0.180) | KERNEL | Absorbed as K5 head. |
| 21 | The explicit-`Dispose()` boundary (announced kills unfrozen) | §28.3 | KERNEL | Absorbed as K4.2 + K5's criterion (unfrozen ⟺ announced ∧ backstopped). |
| 22 | Pattern A chaining `return await` unfrozen | §28.3, §30.5 (v0.184) | DERIVED | K5.4 vacuity: the outer task cannot dispose the chained task before the body (`finally` included) completes, so no body borrow can dangle. |
| 23 | Owning-iterator `yield return` frozen despite its announced target | §28.3, §29.2 (v0.184) | DERIVED | K5 criterion: a yielded value stays alive under the consumer — a transferring kill, no flag, so the backstop half fails. |
| 24 | Drop-on-overwrite of a *binding* frozen; fields/slots not frozen; the move-only whole-value carve | §28.3, §28.7, §28.11, §15.3 | DERIVED / AXIOM | K5 criterion (disposing but not keyword-visible ⇒ frozen at bindings); K5.6 (slot aliases not body-local; slot kill disposes ⇒ K4.1 backstop real; move-kills of slots route to K4.3's clause). The move-only whole-value reassignment carve is A13 — tracked debt, not a derivation. |
| 25 | Discharge requirement, affine auto-drop, must-use warning, exception-path discharge | §28.3; rationale §5.23 | KERNEL | Absorbed as K3.14/K3.15. |
| 26 | Unbound owned temporaries: end-of-full-expression disposal; unwind in reverse production order | §28.3 (v0.184/v0.193) | DERIVED | K1 (an unnamed owned value) + K3.15 (joins the cleanup stack at production) + K8.1 (the boundary). |
| 27 | The full-expression boundary, per-evaluation conditions, `case`-scrutinee extension | §28.3 (v0.193); rationale §5.24 | KERNEL | Absorbed as K8. |
| 28 | Deferred-activation callees: temporary-sourced frames must be consumed within the expression | §28.3 (v0.179/v0.196) | DERIVED | K2.1 + K6.5 + K8.4: the temporary source dies at the boundary; a frame outliving it violates weakest-source; reject-not-extend. |
| 29 | Returning a deferred-activation frame; capture-set contributions; provenance table | §28.3 (v0.184/v0.196); rationale §5.25 | DERIVED | K6.2 + K2.4 (announcement re-establishes carried sources); plain-storage function reads contribute nothing *because* row 19's escape rules never let a borrow-capturing closure into plain storage. |
| 30 | `owned` admissibility: field type restriction; the §28.1 marker positions with per-instantiation erasure (v0.196); no generic-argument position | §28.5, §28.1, §14.4 | DERIVED / AXIOM | K1.6 (markers admissible only where an obligation can exist; erased by substitution where none does) + K1.7 (copy-semantics holders — record fields — cannot hold obligations); the absent generic-argument position is A14 (owning semantics live in named types). |
| 31 | Owned-field reads lend; `move` is the only owned read; the restoration rule with exception edges, destructor exemption, static ineligibility | §28.6, §28.8, §28.8.1 | KERNEL | Absorbed as K9.1–K9.4; the worked examples and remedies stay in §28.8.1. |
| 32 | The throwable-expression classification for restoration analysis | §28.8.1 (v0.184/v0.193) | DERIVED | K9.2 needs the exception-edge enumeration; drop-on-overwrite's throwability derives from K3.11 + §28.4's defined-throw destructors. |
| 33 | Fresh owned values may not enter non-owned slots or non-`owned` returns; getters may not return fresh owned values | §28.6 | DERIVED / AXIOM | K1.7 + K2.3 (the obligation would have no holder — a leak); the getter half is A4. |
| 34 | Who may call `Dispose()` (owned receiver); the transit exception; borrowed owning collections restrict only `Dispose` | §28.6 (v0.184) | DERIVED / AXIOM | K3.10 consumes act on owned holders (the receiver classification already computed); transit = A8; the owning-collection borrow surface follows from K1's container-vs-element obligation split. |
| 35 | Element slot policy (family × element category); borrowing-collection store rule | §28.11 (v0.180) | DERIVED / AXIOM | The family split is A9; given it, every table cell is K1 states applied per slot + K2.3/K4.3 for stored borrows; `move coll[i]` restoration is K9 with per-slot tracking. |
| 36 | Runtime-managed exemption; trivial-dispose classes (per-instantiation) | §28.9 | KERNEL / DERIVED | K1.6 absorbs the exemption; trivial-dispose = K1 with empty obligation content, computed per instantiation by §3.6's machinery. |
| 37 | `using var`: pin, adoption, borrow-pin form | §28.10 | AXIOM / DERIVED | The pin is A5; adoption `using var y := owned x;` is K3.4; the borrow-RHS pin derives from K2 (the binding extends the source's required liveness; no obligation can be assumed from a borrow). |
| 38 | §28.14: storage-bounded admission, declared-before/self-capture tests, strengthened freeze, quiescence; call-bounded default; frame residence; sync-callee task-invocation rejection; method-reference formation bar | §28.14 (v0.175–v0.196) | DERIVED / AXIOM | Storage admission = K2.3 + K5.7 with A11's decidable forms; quiescence is the K4.3-recorded clause with the K4.1 backstop; call-bounded default = K2.4 (a plain signature carries no capture set, so no retention can be announced); frame residence = K6.1/K6.2; the sync-callee rejection = K6.3 + K3 (the minted frame inherits the call bound and a non-`async` body has no consuming position for it); the formation bar keeps position-keyed A10-register admissions alias-proof (a formed value's plain type would erase them). |
| 39 | §30.1: borrow-bearing param/result ban (+ `CancellationToken` exception); `ref`/`out` ban; suspension-crossing record locals need frame-resident sources; `owned` params transfer into the frame; settlement obligation + domain clause | §30.1 (v0.179–v0.186) | DERIVED / AXIOM | All five paragraphs are K2.1 + K6.1 (a frame outlives the synchronous call; only frame-resident sources provably survive; stack slots cannot enter it); `owned` params = K3 with the frame as scope (teardown = its scope end); settlement + domain clause = K6.4 + K4.3; the certification is A10. |
| 40 | §30.5 producer split: Pattern A vs Pattern B by declared type | §30.5; rationale §12.1 | AXIOM | A3; the kernel then checks each pattern's flows. |
| 41 | The three Pattern A result-slot kinds (fresh / chained / borrowed-source) | §30.5 (v0.162/v0.179/v0.184) | DERIVED / AXIOM | K1 (fresh: the task owns), K2+K6 (an owned source: the task cannot own what it borrowed, so the *container* moves into a chained slot — transitive exactly-once; a borrowed source: no transfer is possible, the teardown disposes nothing, residual per K4.3 discharged trivially for process-lived statics). The `using var`-source timing dispensation is A12. |
| 42 | The nine admitted Pattern A await-operand positions | §30.5 | DERIVED | Each bullet is one row of K2's source taxonomy: named locations (using-var local, owned param, plain owned local — K5 guards it, plain borrowed local/param, owned field of `this`, admitted static field — the always-live source, owning-collection slot, iteration variable) ⇒ scope-bounded; a bare producer call ⇒ K8-bounded temporary; `return await Producer()` ⇒ K6's chained slot. The spec already flags the list as illustrative of the principle; the principle *is* K2. |
| 43 | Await-result flow restrictions (a Pattern A borrow may not enter `using var`, owned slots, `owned` params/returns) | §30.5 | DERIVED | K1 state-match (row 6): a borrow cannot assume an obligation. |
| 44 | Pattern B result must reach an owned-consuming context; the context list | §30.5 | DERIVED | K1 (the await mints an owned result) + K3 (the listed contexts *are* K3's enumeration — the list dissolves into a cross-reference). |
| 45 | Pattern B await consumes its operand, fault edge included; announced kill, no freeze, no marker | §30.5 (v0.162/v0.184) | DERIVED | K3.5 + K4.2: the await disposes the task (flag set) and the keyword + single-consumer type announce it; the missing marker spelling in operand position is A7's bound. |
| 46 | Multiple awaits: `Task<T>` fan-out borrows; second `OwningTask<T>` await faults; disposing an unconsumed `OwningTask<T>` disposes the produced result (continuation-attach, non-blocking) | §30.4, §30.5 | DERIVED | K2 (borrows are many) vs. K1.7 (one obligation moves once — the second take has nothing to take; `InvalidOperationException` is its dynamic dual); the unconsumed-dispose cascade is K1 (the container owns the result until transfer), deferred by continuation per §30.13's non-blocking discipline. |
| 47 | Completed-task factories: `Task.FromResult` non-class only; `Task.FromOwnedResult` for class T; no external Pattern A class-T completion | §30.5 | DERIVED | A3 + K1: an externally-supplied owned value must transfer into a single-consumer container; a shared-borrow container of it would need an imperative completion story — deferred, recorded in §30.5. |
| 48 | `Task` discard rejection | §17.13, §28 preamble | AXIOM | A6. |

### 4.1 Worked derivation: the nine admitted Pattern A await positions (row 42, expanded)

The brief's own test case, done in full. §30.5's list prefaces itself as illustrative of a principle; the principle is K2's source taxonomy, and each bullet derives in one line. The tenth row is the unlisted general case the principle also covers, and the last is the rejection that proves the boundary.

| # | Admitted operand position (§30.5's bullets, in order) | K2 source kind | One-line derivation |
|---|---|---|---|
| 1 | a `using var` / `await using var` local of `Task<T>` | named location | The binding's scope bounds the borrow (K2.1); the pin (A5) makes the bound lexical and unconditional. |
| 2 | an `owned` parameter of the enclosing method | named location | The parameter's lifetime spans the body (K2.1); K5 freezes any consume of it while the result borrow lives. |
| 3 | a plain owned local (v0.184, #1533) | named location | Same scope bound as row 1; the movability of the binding is exactly what K5 guards — consume/reassign under the live borrow is frozen, explicit `Dispose()` is the K4.2 announced kill. |
| 4 | a plain (non-`owned`) local or parameter (v0.193) | named location | A binding is a named location whatever its state (K2); its own source set rides along by K2.2, carrying the true-task-outlives-use residual under K4.3 — ownership of the location was never the criterion, as §30.5 itself concluded. |
| 5 | an `owned` field of `this` | named location (field path) | A storage read lends with the holder as source (K1.5); `this` outlives the body's uses (K2.1); K9 keeps the field from any scope-end machinery. |
| 6 | an admitted static `Task`-typed field | named location (static storage) | K2's distinguished always-live source: every intra-program liveness constraint is satisfied by construction (§22.13). |
| 7 | an indexer access on an owning collection of tasks | named location (slot) | Slot reads lend with the collection as source (K1.5); the collection's own location bounds the borrow (K2.1). |
| 8 | an iteration variable over `Array<Task<T>>` / `OwningArray<Task<T>>` | named location (element borrow) | The element borrow's source is the collection per A9's slot table (K1.5); the loop body sits inside the source's liveness range (K2.1). |
| 9 | `return await Producer()` in a Pattern A body | frame | K6.3 mints the producer's frame owned; the K3.6 consume moves it into the outer task's chained slot, so the borrow rides the outer frame, bounded by its settlement (K6.1) — row 41's kind 2. |
| 10 | *(the general unlisted case)* a bare producer call whose result borrow stays in the same full expression — `Process(await Producer());` | full-expression temporary | K6.3 mints; the unbound temporary's K8 boundary bounds both the frame and the borrow; disposal after the last use (K8.1). |
| — | **rejected:** `var x := await Producer();` | — | The borrow would escape its temporary source's K8 boundary — weakest-source violation (K2.1 + K8.4); the remedy names the source first (`using var task := Producer();`). |

### 4.2 Worked derivation: freeze membership computed by the K5 criterion (rows 20–24, expanded)

The K5 criterion — *unfrozen ⟺ the kill is announced at the site ∧ disposes (sets the flag)* — evaluated against every consume position, reproducing §28.3's recorded boundary exactly, with one structural override the kernel states as K5.4:

| Consume (K3 position) | Value's fate at the site | Announced as a kill? | Verdict | Matches v0.197? |
|---|---|---|---|---|
| `owned` argument; marked transfer `y := owned x;` | alive under a new holder — no flag | announces a *transfer*, not a death | **frozen** | ✓ (freeze list) |
| store into an `owned` field / owning slot; `owned` return; unmarked `Task`-family return | alive under a new holder | target-announced transfer, not a death | **frozen** | ✓ |
| owning-iterator `yield return x` | alive under the consumer | keyword-visible, but nothing dies | **frozen** | ✓ (§29.2; the "surprise" of §1.3 dissolves) |
| reassignment of the binding (drop-on-overwrite) | displaced value **disposed** — flag set | the assignment does not read as a kill of the *old* value | **frozen** | ✓ (the other direction of the conjunction) |
| explicit `x.Dispose()` / `x?.Dispose()` | disposed, flag set | yes — the call is the announcement | **unfrozen** | ✓ |
| discard `_ := x;` | disposed at end of statement | yes — the discard sink | **unfrozen** | ✓ |
| `throw x` | disposed after the matching arm | yes — the keyword | **unfrozen** | ✓ (§31.1 states the criterion verbatim) |
| Pattern B `await x` | task disposed by the await | yes — keyword + single-consumer type | **unfrozen** | ✓ |
| Pattern A chaining `return await x` | alive, moved into the outer task | announced transfer | frozen by the criterion — **unfrozen by K5.4's vacuity override** (the outer task cannot dispose before the body, `finally` included, completes: no body borrow can outlive the kill) | ✓ (§28.3/§30.5 record exactly this structural reason) |

Nine placements, two predicates, one recorded override. This table is the extensional-equality claim of Q1 in checkable form; T1's critique attacks it directly.

**The count.** Of the 48 rows: **12 are KERNEL absorptions** (rows 1, 3–4, 7–9, 17, 20–21, 25, 27, 31; row 36 splits KERNEL/DERIVED), the remainder are **DERIVED** or **AXIOM** — each DERIVED row with its one-line derivation in place — and **all fourteen register axioms find their normative homes in the table**: A1 rows 12/15, A2 row 14, A3 rows 40/47, A4 row 33, A5 row 37, A6 row 48, A7 rows 11/45, A8 row 34, A9 row 35, A10 rows 38–39, A11 row 38, A12 row 41, A13 row 24, A14 row 30. *Fourteen is the honest complexity budget of the current ownership surface.* Everything else a reader meets in §28/§30.5 is, per this table, computable from nine principles. Two budget movements are already scheduled: proposal 0048 deletes A1 (budget → 13), and A7/A11/A13 are recorded liftable/tracked (each lift is a budget-neutral swap of an axiom for a derivation, or a deletion).

## 5. The editorial plan

**Order of operations** (each step independently landable; steps 2–6 are the tag passes):

1. **Land the kernel** — the §2 text as the new head subsection of §28.3, plus the §2.1 axiom register at its tail, plus a new rationale section ("The ownership kernel: reasons and the axiom budget") carrying each axiom's full recorded reason (largely relocations from §5.1/§5.3/§5.7/§5.23–§5.25/§12.1/§12.10, which remain and are cross-referenced, not duplicated). Purely additive: no existing sentence changes. The changelog entry states the zero-semantic-delta intent and the escalation rule.
2. **§28.2–§28.3 tag pass** — rewrite the case-law subsections beneath the kernel: KERNEL-tagged rules (rows 1, 3–4, 7–9, 17, 20–21, 25, 27) collapse to cross-references + their examples; DERIVED rules gain the derivation tag in the existing citation idiom — a trailing parenthetical, e.g. *"(derives: K2.4 + A1)"* — at the rule's head, and lose any sentences that merely restate kernel content.
3. **§28.5–§28.11 tag pass** (rows 30–37).
4. **§28.14 tag pass** (row 38), including re-pointing its quiescence statement at K4.3 — one of the four residual-trust spellings of §1.3; the §28.3, §28.11, and §30.1 siblings collapse in T2, T3, and T5 respectively.
5. **§30.5 + §30.1 tag pass** (rows 39–48): the Pattern B context list becomes a cross-reference to K3 (row 44); the nine-position list keeps its bullets as *examples* under an explicit "instances of K2's source taxonomy" head (row 42).
6. **Closing audit** — Appendix C.3/C.6 re-audited line-by-line against the tagged text (no semantic edits; wording synced); Guide 01 re-traced and its revalidation pin bumped; the acceptance audit below run and archived.

### 5.1 Subsection-fate inventory (the T2–T5 work orders, decided here)

What each existing subsection becomes. "Absorbed" = the rule statement collapses to a kernel cross-reference, keeping its examples and remedies; "kept" = the statement stays, gaining its tag and losing restatements.

**§28.3** (T2):

| Subsection (v0.197) | Fate |
|---|---|
| head + *Ownership state of locals* | absorbed → K1 states, K7 joins; the two equalization patterns stay as worked examples |
| *Producers* (the table) | kept — the worked enumeration under K1.5; rule-carrying rows gain tags (`Task`-family row → K6.3/A2; bare-type-parameter row → K2.4; `??`/arm rows → K7/A7; static-property row → K2's always-live source) |
| *Factory methods and sourceless returns* | kept, headed by its A1 tag; the caller/callee two-sides-of-one-boundary note becomes row 13's derivation |
| *Bare-type-parameter returns* | kept; head gains *(derives: K2.4; empty source set → A1)* |
| *Move semantics for owned locals* | absorbed → K3.13; the loop-accumulator and `Wrap`/`Wrap2` examples stay |
| *Captured owned bindings* | kept, one paragraph; *(derives: K2 + K5.5)* |
| *Borrow lifetime rules* | rewritten as K2.3 instances; its residual-trust spelling collapses to a K4.3 citation (the first of §1.3's four; the siblings fall in T3–T5) |
| *The consume-site freeze* + *explicit-`Dispose()` boundary* | absorbed → K5 + K4.2; the §4.2 placement table's content becomes the section's example matter; the mechanical-rewrite remedy stays |
| *Owned-local discharge requirement* | absorbed → K3.14/K3.15; must-use warning wording and examples stay |
| *Unbound owned temporaries* + boundary + `case` extension | kept; *(derives: K1 + K3.15 + K8)*; boundary definition itself absorbed → K8 |
| *Deferred-activation callees* / *Returning a deferred-activation frame* | kept; *(derives: K2.4 + K6)*; the capture-set provenance table stays as the decidable computation of K6's carried sources |
| *Diagnostics* | kept verbatim (message texts are compiler surface), gaining K-references per message |
| *Choosing a disposal form* (recap) | kept as-is — it is already the kernel's doctrine in table form |

**§30.5 + §30.1** (T5):

| Subsection (v0.197) | Fate |
|---|---|
| *Producer-side return-type rules* | kept, headed by A3; the three slot kinds gain row-41 derivations, A12 tagged at kind 2 |
| *Consumer-side rules* | rewritten: the v0.182 principle paragraph becomes the K2 citation; the nine bullets stay as examples classified per §4.1; the Pattern B context list collapses to a K3 cross-reference (row 44) |
| *Disposing an unconsumed `OwningTask<T>`* | kept; *(derives: K1 cascade + §30.13's non-blocking discipline)* |
| *Multiple awaits on `OwningTask<T>`* | kept; *(derives: K2 fan-out vs. K1.7 single obligation)* |
| *Constructing a completed task…* | kept; *(derives: A3 + K1)* |
| *Idiom: async resource acquisition* | kept (non-normative) |
| §30.1's five ownership paragraphs | kept; each gains its row-39 tag; the settlement paragraph's residual-trust spelling collapses to K4.3 |

**§28.5–§28.11** (T3) and **§28.14** (T4) follow the same pattern from rows 30–38; §28.14's structure survives intact (its admission machinery is A11's home), with quiescence re-pointed at K4.3.

**The acceptance test** (the P6 sketch's, made operational): *every normative sentence in §28, §30.5, and §30.1's five ownership paragraphs is (a) inside the kernel subsection, (b) inside a rule carrying a derivation tag, or (c) inside a rule carrying an axiom tag.* The audit artifact is a working paper in this folder (`0046-audit-<rev>.md`, superseded at close) listing every heading-to-heading block with its tag and — for DERIVED — the one-line derivation; sentence-level coverage is the audit's granularity, rule-level tags are the text's (tag-per-sentence was considered and declined as noise). A block that resists all three classifications is, by definition, either a missing kernel clause, a missing axiom, or a defect — each of which is a finding, filed.

**Tagging idiom.** Tags reuse the spec's existing parenthetical-citation style and revision-pin conventions; no new markup. K-anchors are stable ids (`sec-kernel-k1`…); axiom ids A1–A14 are frozen on landing (new axioms append; deletions leave a tombstone row in the register pointing at the deleting revision's changelog entry, so "which axiom did this ruling extend?" stays answerable historically).

**T1's changelog entry, skeleton** (drafted here so the landing writes to a decided shape):

> *Decided:* the ownership kernel K1–K9 stated normatively at the head of §28.3; the axiom register A1–A14 at its tail; the kernel/derivation/axiom obligation added to the spec-change-process checklist. *Intended semantic delta: zero* — every K clause cites the v0.197 rule it restates (proposal 0046 §2, §4). *Declined:* formal notation in the spec; a separate model chapter; sentence-level tags (0046 §13). *Escalation rule:* any divergence between kernel and standing text discovered during the T2–T5 passes is its own ruling issue; the trains never decide one. *Migration:* doc-only; no program, diagnostic-trigger, or conformance change.

**The process-checklist line** (added to `spec-change-process.md` §9 at T1, per §7's obligation): *"— [ ] If the change touches §28/§30.5: state its kernel classification — derivation (name the K's), axiom extension (name the axiom), or new axiom (add the register row with its recorded reason)."*

## 6. Interactions

Every feature the rewrite touches or abuts, with the honest statement of each:

1. **Proposal 0048 (explicit factory ownership)** — deletes axiom A1 and re-derives row 13's prohibition trivially (no unannounced static class return exists at all). No conflict; pure sequencing (§14, Q5). If 0048 lands first, this proposal's register records A1 as already-tombstoned.
2. **Proposal 0051 (task-ownership defaults)** — Part A flips *guidance* (rationale §12.2's decision tree), not rules; A3 and every §30.5 row stand. Its Part B study, if ever adopted, would be a new axiom or a K8 amendment — exactly the budget discipline this proposal installs. Coordinate changelog text if both land in one revision train.
3. **Proposal 0047 (mechanized core semantics; forthcoming — number reserved for P7)** — the consumer of the kernel; §11.
4. **§9.7 borrow-bearing records / spans** — K2 sources ride record values (the freeze's record-local clause, v0.186); §9.7's no-temporary-source rule is K8.4's posture. The tag pass cites, does not restate.
5. **§15.3 move-only value types** — K1's owned classification applied to values; the #1385 carve is A13. §15.3's own text is out of this revision's rewrite scope (only its §28.7/§28.3 tendrils get tags).
6. **§17.13 / §31.1 / §19.3** — discard, throw-operand, catch- and pattern-bindings: K3/K4 positions and K2 borrows respectively; their home sections keep their statements, gaining back-references.
7. **§22.12/§22.13 statics** — static storage as K2's always-live source; the confine-or-synchronize obligation on values *reached through* statics is §22.12's own rule and deliberately outside the kernel (§2.3).
8. **§23.3 `set(owned)` accessors, §26.7 extension receivers, §27.1 function-type identity** — K3.13 announcement, K2 receiver sources, and the marker-in-type-identity rules respectively; unchanged, cited.
9. **§29 iterators** — K6 covers frames; §29.2's yield-ownership keying and §29.11's affine single consumption are K6 constituents (rows 23, and K6.1's query bound). §29's own text gets no rewrite in this revision beyond back-references — a deliberate scope cut (§14, Q6 offers the extension).
10. **§30.13–§30.15 domains, sendability, channels** — out of kernel scope (§2.3) except: spawn arguments and channel `Send` are K3.3 instances, and the domain clause is K4.3's recorded limit. Any future freeze/sendability interaction ruling must name K4.3 or K5 — the budget discipline's first live test.
11. **§32 FFI** — `scoped` illegal on `extern`; boundary trust is §32.10's documented-trust contract in place of compiler verification. The kernel's K4.3 vocabulary ("recorded trust") matches; cited only.
12. **Appendix C.3/C.6** — C.3 is in effect a third restatement surface of the kernel's consequences (its ~28 bullets map onto rows above); the closing audit syncs wording and adds K/A tags to C.3's bullets so the invariant list and the chapters cannot drift silently again.
13. **The conformance corpus (`tests/conformance/`)** — hand-maintained (#1380); this revision's examples are unchanged (no example semantics move), so no corpus edits — but the referee role in §9 makes the corpus load-bearing for the rewrite's verification.
14. **The changelog discipline** — history narration must not leak into the rewritten text (spec-change-process §7); the kernel subsection carries only current rules with `(v0.NNN)` pins where they date decisions.

## 7. Migration and compatibility

- **Programs:** zero change. No rule's extension changes; no diagnostic's trigger condition changes. (Diagnostic *wording* may later cite K-numbers — that is P12's lane, explicitly not this revision's.)
- **The compiler:** zero work. The revision restates what the compiler already implements. Divergences discovered while auditing are filed as issues against whichever text/implementation is wrong — they are *outputs* of the revision, not obligations of it.
- **Citations:** no section is renumbered and no existing anchor moves (the kernel is a new h4 under §28.3; K/A anchors are new). External references of the form "§28.3, v0.19x" remain valid; the pinned-revision citation discipline covers the rest.
- **The guides:** Guide 01 teaches the model by example; the examples' semantics are unchanged, so revalidation is a re-trace plus pin bump, expected no-op.
- **Future rulings:** the one process-visible change. After this revision, a ruling touching §28/§30.5 states its classification (new K clause — expected never; new derivation; axiom extension — naming the axiom; new axiom — with a recorded reason and a register row). This is a changelog-entry obligation, added to the spec-change-process checklist in the same landing.

## 8. Staging

Six landings (§5's steps), classified per `spec-change-process.md` §2:

| Train | Content | Class | Gate |
|---|---|---|---|
| T1 | Kernel + register + rationale section | **Ruling** (see §10) | mechanical + adversarial critique + changelog decision record |
| T2 | §28.2–§28.3 tag pass | Clarification | mechanical + one reviewer confirms rule-for-rule preservation |
| T3 | §28.5–§28.11 tag pass | Clarification | same |
| T4 | §28.14 tag pass | Clarification | same |
| T5 | §30.5 + §30.1 tag pass | Clarification | same |
| T6 | Appendix C audit, guide revalidation, acceptance audit archived | Editorial | mechanical |

T2–T5 may batch into fewer revisions; if any two land together, the composition gate (process §4) runs over the union. **Any train that discovers a divergence stops classifying and files**: the divergence gets its own issue and, if text must change meaning, its own ruling-class landing outside these trains. T1 is the only train that can be argued about; T2–T6 are mechanical given T1 and the §4 table.

## 9. Risk register

| Risk | Mitigation |
|---|---|
| **Semantic drift during rewrite** — the classic rewrite failure: a "restatement" that quietly decides a case. | Additive-first staging (T1 deletes nothing); per-train old→new sentence audit — every deleted sentence maps to the covering kernel/derived rule; **the conformance suite as referee** — `check_docs.sh` extraction parity plus a re-trace of every §28/§30.5-derived corpus case against the rewritten text; and the standing rule: **every discovered divergence is filed as its own ruling issue, never silently normalized** — in either direction (kernel wrong or old text wrong). Divergence issues carry a fixed shape: the two texts quoted (or text + compiler behavior with a probe), the affected §4 row, which side is presumed wrong and why, and the K/A classification the resolution will bear — so the tracker doubles as the audit's exception log. |
| **The kernel itself is wrong** — a K clause over- or under-generalizes (e.g. K5's two-predicate criterion misplaces some future kill form). | The mapping table is the falsifier: 48 rows must each derive in one line. A derivation that needs more than one line is treated as a finding against the kernel, not padded. T1's adversarial critique is instructed to hunt exactly this — the killer counterexample to each K. |
| **Axiom laundering** — tagging as AXIOM what should derive, to make the audit pass. | Every axiom requires a recorded reason and an owner-visible register row; the budget is published in the changelog; the critique pass challenges each axiom's non-derivability specifically (§2.1's two reclassified candidates are the worked precedent). |
| **Tag noise degrades the spec's readability.** | Rule-granularity tags in the existing parenthetical idiom; KERNEL absorption *shortens* the chapters (the four residual-trust restatements collapse to one); guides untouched. |
| **Kernel/0047 drift** — the mechanization models yesterday's kernel. | Frozen K/A numbering (§5); 0047 §11's process rule (a §28/§30 change updates the model or records why out of scope) is seeded by this proposal's register. |
| **Concurrent rulings while trains land** — the spec moves at run cadence. | Standard re-pin at each train's landing (process §6); the trains rebase; a ruling landing mid-sequence adds its own row to the §4 table as part of *its* landing (the process change of §7 makes this self-enforcing once T1 lands). |

## 10. Process classification (per `spec-change-process.md`)

The revision is **intended as semantics-preserving** — by the process taxonomy, Clarification-class ("states an already-implied rule explicitly; consolidates text that lived in several places"). Two of the process's own rules push the gate up, and this proposal argues for honoring both:

- *"When unsure between two classes, take the stricter gate."* A kernel restatement is the highest-leverage place for an accidental ruling to hide — K5's two-predicate freeze criterion, for instance, *claims* to reproduce the current list exactly (rows 21–24), and that claim deserves an adversarial critique, not a nod. **T1 is therefore gated as a Ruling** (critique + decision-recording changelog entry), even though its intended semantic delta is zero.
- *"A change is a design change the moment it contradicts a recorded decision."* The kernel contradicts none — it cites the recorded decisions as its axioms — but any *discovered* non-preservation would be a contradiction of the standing text, and **escalates**: out of the editorial train, into its own ruling with its own critique, per the standing escalation rule (§8–§9). The editorial train never decides a divergence.

The proposal itself takes the design-change *path* (numbered proposal → owner acceptance → gated landings) not because it changes design but because of its scale: it is the honest gate for a revision that rewrites the two hardest chapters, and the P6 mandate routes it here explicitly.

## 11. Relationship to 0047 (mechanized core semantics)

The kernel is the mechanization target — P7's scope is defined in `future-directions-2026-07-18.md` as "the P6 kernel." (0047 is the number reserved for P7; that proposal is not yet written, so citations to it here are to the P7 sketch.) Concretely:

- **K1–K9 are the model's definitions**; the K-numbering is frozen at T1 (new principles append as K10+; no renumbering, ever — 0047's artifacts cite K-ids).
- **Each DERIVED row of §4 is a lemma candidate**: the one-line derivation is the proof sketch; a derivation the mechanization cannot reproduce is a finding against either the kernel or the row, filed like any divergence.
- **Each AXIOM is a definition or assumption the model must carry explicitly** — fourteen today; the model's assumption count and the register must agree, which gives 0047 its coverage oracle for free.
- **The acceptance audit is the coverage spec**: a normative sentence the audit tagged is a sentence the model must either capture or record as out of scope (0047's process rule).
- **The theorem map** — P7's four named theorems against the kernel principles each must exercise:

  | 0047 theorem (per the P7 sketch) | Kernel principles exercised | Carried assumptions |
  |---|---|---|
  | exactly-once disposal on every path | K1, K3, K7, K9 | A12 (chaining timing), A13 (the recorded carve — the model states it as the one exception, exactly as §28.7 does) |
  | no silent use-after-consume (checked mode faults deterministically) | K4, K5, K7 | A7 (no arm marker), A8 (transit vouching) |
  | frame-residence soundness (rationale §5.25) | K2, K6, K8 | A10 (certifications as assumed lemmas), A11 (admission forms as given) |
  | domain isolation | outside this kernel (§2.3) — modeled from §30.13–§30.15 directly, with K4.3's domain clause as the interface obligation | — |

- Scope seam, stated once: the kernel covers ownership/borrow/lifetime; 0047 additionally models domains, sendability, and channels (§2.3's excluded systems), for which no kernel restatement exists yet — 0047 takes §30.13–§30.15 as its source text there, and if that half ever warrants its own consolidation, it is a separate proposal.

## 12. Recorded divergences from the P6 sketch

1. **The kernel is nine principles, not the sketch's six-item list.** The sketch names "ownership states and the lattice, discharge, borrows-with-sources, frames-as-values, announced kills, the consume-site freeze"; this draft promotes the join rule (K7), the full-expression boundary (K8), and the field-restoration principle (K9) to first-class kernel status. Reason: each is load-bearing for multiple derivations in §4 (rows 5–10, 26–28, 31–32) and none derives from the sketch's six — leaving them out would have forced three more axioms instead.
2. **The §28.14 admission machinery is classified part-axiom (A11), not "re-derived as a corollary."** The sketch lists "the §28.14 admissions" among the case lists to re-derive. The storage-bounded *principle* does derive (row 38: K2.3 + K5.7), but the five bounding-object forms and the seven v1 bounds are decidability engineering — choices among sound alternatives, several explicitly liftable — and calling them corollaries would be exactly the axiom-laundering §9 guards against, in reverse. Recorded as the honest split.
3. **The audit scope adds §30.1's five ownership paragraphs to the sketch's "§28/§30.5".** Frame residence and the settlement obligation are half of K6; auditing §30.5 without them would tag rules whose kernel clauses live in an unaudited section. Deliberate widening, surfaced as Q6.
4. **Staged as six trains, not one revision.** The sketch reads as a single revision; the size (a full pass over the two hardest chapters) and the process's composition-gate economics argue for the additive-kernel-first sequence of §8. The acceptance test binds at T6 regardless.
5. **The axiom count is fourteen, with the register drafted** — the sketch asked only that the count exist. Publishing the draft register in the proposal (rather than at landing) is deliberate: the register *is* the reviewable claim, and §2.1's two reclassified candidates show the classification being done, not promised.

## 13. Alternatives considered and declined

- **A separate "ownership model" chapter (new §28.0 or a fresh top-level section), leaving §28.3 as-is.** Declined: it duplicates rather than consolidates — the two texts would drift exactly as the four residual-trust restatements did, and §28.3 is already the de-facto kernel home every other section cites. The head-of-§28.3 placement gets one home with zero renumbering.
- **Kernel in the rationale, spec unchanged.** Declined: the rationale is non-normative by charter; a kernel that cannot be cited normatively cannot anchor derivation tags, and the acceptance test would be unenforceable. (The rationale gets the *reasons*, per the standing division of labor.)
- **Full rewrite in one revision (delete-and-replace).** Declined: maximizes the semantic-drift risk §9 ranks first, defeats bisection of any discovered divergence, and makes the critique pass review everything at once — the additive-then-tag sequence keeps every train small enough to verify rule-for-rule.
- **Sentence-level tags.** Declined as noise (§5); rule-level tags with a sentence-level *audit* get the same guarantee at a tenth the ink.
- **Formal notation in the spec (inference rules, judgments).** Declined for this revision: the spec's register is precise prose, its readership includes the guides' audience, and the formal artifact has a better home — 0047's model, which can be exact without being normative. Revisit only if 0047 finds prose-K's genuinely ambiguous (that finding would itself be a T1-class defect to fix in prose).
- **Deriving harder (a smaller axiom register).** Each of A1–A14 was pressure-tested for derivability; the two that fell are recorded in §2.1. Forcing, e.g., A4 (accessor lend-only) or A5 (`using var` pin) into derivations would require inventing kernel principles with no other consumers — a larger kernel to fake a smaller register. The budget is honest at fourteen.
- **Doing nothing / relying on Appendix C.** Appendix C.3 is an invariant *checklist*, not a derivation structure — it restates outcomes with no reasons and is itself one of the drifting copies (§1.3). It becomes a beneficiary of the kernel (§6.12), not a substitute.

## 14. Open questions / owner decisions

1. **Q1 — Ratify K1–K9 as the normative kernel frame** (the §2 text, modulo graduation polish), including the two consolidation claims that go beyond mere restatement in *form*: K5's unfrozen ⟺ announced ∧ backstopped criterion and K2's three-kind source taxonomy. Both claim extensional equality with current text (rows 21–24, 42); the T1 critique verifies, but the *shape* is an owner call.
2. **Q2 — Ratify the axiom register at fourteen** (§2.1), or direct specific reclassifications. Each axiom row is individually contestable; a contested row becomes either a derivation (shrinking the budget) or keeps its reason (strengthening the record).
3. **Q3 — Register and tag placement**: recommended — register normative, at the kernel subsection's tail; reasons in the rationale; rule-level parenthetical tags. Confirm, or move the register to Appendix C.
4. **Q4 — The future-rulings obligation** (§7: every §28/§30.5 ruling states kernel/derivation/axiom, named): confirm adding it to the spec-change-process checklist at T1. This is the piece that keeps the budget honest after the revision closes.
5. **Q5 — Sequencing versus 0048**: recommended — land T1 before 0048 graduates, so 0048's change is legible as the deletion of axiom A1 (the budget's first planned decrease); no hard dependency either way.
6. **Q6 — Scope**: confirm the §30.1 audit widening (divergence 3). Optionally also direct the §29.2/§29.11 iterator sections into the tag passes (currently back-references only, §6.9) — recommended against for this revision (scope discipline; K6 already covers their content from the §28/§30 side).
7. **Q7 — The acceptance-audit artifact**: recommended — a committed working paper in this folder, superseded at T6 (matching the archive discipline); alternative is scratch-only, which loses the provenance the next consolidation would want.
8. **Q8 — T1's elevated gate** (Ruling-class for an intended-preserving change, §10): confirm the stricter-gate reading, or direct Clarification-class with a single reviewer.
