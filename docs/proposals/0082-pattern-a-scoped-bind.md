# Proposal 0082 — The scoped await-bind: `var x := await <fresh Task<T> producer>;`

| | |
|---|---|
| **Status** | **superseded — graduated v0.249** (owner-ruled 2026-08-11; C1/C2/C3 gates passed, stage-1 prototype [#3048](https://github.com/s8lap/bezel-lang/issues/3048)): spec §30.5 scoped await-bind + §28.3/§28.10 anchors + §30.3 `.Result` lockstep + Appendix C.6; rationale §5.24 second-recorded-stretch paragraph + §12.2; Guide 4 §3. Q1 run-first (C1 7/11 strict), Q2 `const` included (sequenced behind #3074), Q3 lockstep, Q4 normative recommended-message text, Q5 fenced. *(prior: under-review — drafted 2026-08-11 (owner-directed, from the elegance review))* |
| **Tracking issue** | [#3048](https://github.com/s8lap/bezel-lang/issues/3048) |
| **Target module(s)** | **spec** — §30.5 (consumer-side Pattern A: the scoped-bind admission and its diagnostics), §28.3 (unbound owned temporaries: one paragraph naming the pinned-slot form), §28.10 (the compiler-introduced slot rides the existing cleanup stack — one sentence), Appendix C.6 (the bare-bind invariant reworded). **rationale** — §5.24 (the parallel bounded-stretch paragraph the `case`-scrutinee entry's template anticipates), §12.2 (one sentence). **guides** — Guide 04 §3 rewrite of "the compile error newcomers hit" at graduation. **No grammar change** (the form already parses; the change is semantic admission). |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | Elegance review 2026-08-11, finding 1 — ranked first: "the most natural line of async code is a compile error." Prior art engaged in full: rationale §5.24 (reject-not-extend; the one recorded `case`-scrutinee stretch), the v0.182 changelog's recorded decline of exactly this mechanism (kept as a liftable), and proposal 0051 Part B — the deferred **study** of this rule, whose §3.1 statement and §3.2 soundness walk this proposal adopts and whose C1–C3 adoption gate it must answer (§6). |

*Reference convention: unprefixed §-references are language-spec references; K-tags are §28.3's kernel ids.*

---

## 0. Thesis

For a Pattern A producer (`Task<T>`, class `T`), the one-line bind is a compile error and the spec demands the two-line form:

```bezel
var cfg := await FetchConfigAsync(url);   // ERROR today (§30.5): cfg would outlive the temporary task

using var task := FetchConfigAsync(url);  // the demanded form
var cfg := await task;
```

Every individual rule is right (the task owns the result; a borrow needs a live source; a temporary dies at its K8 boundary), and the admission list that grew around the friction — nine bullet positions across three relaxation trains (v0.182, v0.184, v0.193; the history is 0051 §1.1's table) — is the recorded evidence that the composition fights the dominant idiom. The proposal: make the compiler write the two-line form itself, for exactly this shape.

> **The scoped await-bind.** In a local declaration `var x := await E;` (or `const x := await E;`, Q2) whose initializer full expression is exactly the await, where `E` is an **unbound owned temporary of static type `Task<T>` for class T** (§28.3 — a bare producer call or any other unnamed owned task expression), the temporary is not disposed at its K8 boundary; instead the compiler binds it to a **hidden scope-pinned slot** — established on the §28.10 cleanup stack at the declaration, disposed at the end of the innermost enclosing block, exactly as a hand-written `using var` at the same line would be — and `x` is an ordinary **borrowed** local whose source is that slot. The slot is unnameable: no program text can read, move, re-await, or dispose it.

The desugaring is literal: `var x := await E;` ⇒ `using var $t := E; var x := await $t;` with `$t` fresh and inexpressible. Disposal timing, exception behavior, and borrow bounds are **identical to the hand-written form** — the rule adds no lifetime the programmer could not have written, only removes the demand that they write it.

## 1. Which producer expressions qualify — the bounded set, argued

The operand class is **exactly §28.3's "unbound owned temporary" classification at static type `Task<T>` (class T)** — a classification the compiler already computes at every await site, which is the containment argument in one line: the rule introduces no new enumeration, only re-points an existing one. Worked boundaries:

- **A direct producer call** — `FetchConfigAsync(url)`, `client.GetAsync(url)`, an interface-dispatched or function-value invocation — qualifies: calls mint (§28.3, axiom A2's family keying), the result is a fresh unbound owned task.
- **A conditional producer** — `var x := await (if useAlt then FetchAltAsync() else FetchConfigAsync());` — qualifies: the arm-meet of two mints is owned (K7.1), and the whole parenthesized form is one unbound owned temporary. The desugaring binds the meet result; nothing arm-specific arises. Admitting it costs nothing because the qualification test is the classification, not the syntax; rejecting it would re-introduce a syntactic list.
- **A named operand** — `await t` for any admitted named position — does **not** engage the rule: those binds are governed (and mostly admitted) by §30.5's existing positions today. The rule fires **only where today's program is an error**: a pure widening; no accepted program changes meaning.
- **A composition** — `var y := (await E).Name;`, `var z := await E ?? fallback;` — does **not** qualify: the initializer must be *exactly* the await. Widening to compositions would extend the hidden slot's justification from "the compiler writes the two-liner for you" to general expression-temporary extension — §5.24's declined territory. (`(await E).Name` with a `string`/value member already compiles today under expression confinement; the class-typed-member composition stays the two-line form.)
- **A `.Result` read** — `var x := P().Result;` (completed task, outside domains, §30.3) — lockstep is Q3; v0.182 moved await and `.Result` in lockstep and §30.3 already unifies their temporary machinery, so the default answer is lockstep, but nothing in this proposal depends on it.

## 2. Which binding forms qualify — argued

- **`var x :=`** — the motivating form. `x` is a plain borrowed local, source = the hidden slot; every §28.3 borrow rule applies to `x` unchanged (returns, stores, captures past the scope: rejected by the ordinary machinery — the slot is a scope-bound source).
- **`const x :=`** — **admitted** (recommend; Q2). A runtime `const` binds values and borrows that carry no obligation (§21.1, rationale §5.27); the await's borrow is exactly that. Same desugaring; the `const` adds non-reassignability of `x` only.
- **`using var x :=`** — **stays rejected.** The await produces a borrow and `using var` requires an owned RHS (§30.5's flow-to list). The rule extends a lifetime; it manufactures no ownership. This is also the site of the teaching diagnostic (§4).
- **Assignment to an existing local** — `x := await E;` — **stays rejected** in v1. The existing binding's scope may strictly enclose the statement's block; honoring it would require hoisting the hidden slot to that outer scope, growing the invisible lifetime beyond "the two-liner you would have written here" and re-opening the containment argument. Recorded liftable, not taken.
- **Field stores, `owned` arguments, non-chained returns** — untouched; §30.5's may-not-flow list and the chained-slot rule govern as today.

## 3. The hidden binding's exact semantics

- **Scope and disposal point.** The slot is established at the declaration statement (its position on the §28.10 establishment-ordered cleanup stack is the declaration's — exactly where the hand-written `using var task` would sit) and disposed at the **innermost enclosing block's** end, on every exit edge — normal fall-through, `break`/`continue`, `return`, and the exception edge — in reverse establishment order with every other cleanup. Consequences, worked:
  - a `using var` declared *after* the bind disposes *before* the slot; one declared *before* disposes after — identical to the two-line form;
  - **early `return`**: the slot's disposal runs during that return's scope exit, after `x`'s last use (the return value cannot be `x` escaping — the borrow rules reject returning a slot-sourced borrow except through the ordinary admitted forms, none of which apply to a scope-bound source);
  - **exceptions**: if `E`'s production or the await faults, the already-produced task is disposed during unwinding by the same establishment-ordered pass that covers a hand-written `using var` (K3.15 edge parity); the binding never comes into being. If a *later* statement throws, the slot unwinds in stack order. No new edge (0051 §3.2's walk, adopted);
  - **a second scoped bind in the same scope** — `var a := await F(); var b := await G();` — creates two slots, disposed in reverse order at scope end: byte-for-byte what the two hand-written pairs produce;
  - **loops**: a bind inside a loop body pins to the loop *body* — per-iteration disposal, no accumulation across iterations (the enclosing-block rule; the same per-evaluation concern §5.24 resolved for condition temporaries).
- **No second await is expressible.** The slot is unnameable and the single await already ran; §30.4's multi-await surface never opens. The deferred interval holds a **settled** task — the container and its owned result, no live frame — so §30.1's source-outlives-settlement obligation is discharged *before* the deferral begins (the await is the settlement).
- **Consume-site freeze: no surface.** No user-visible binding exists to consume, reassign, or dispose (K5 has nothing to guard). Reassigning `x`... `x` is an ordinary borrowed local; reassigning it to another borrow merely ends its interest — the slot's disposal stays at scope end (timing only; scope end is the answer, matching the two-line form where re-purposing `cfg` never moves `task`'s disposal either).
- **Checked-mode backstop unchanged.** Task and result remain disposal-tracked; any path the static rules cannot see faults as `DisposedAccessException` (§28.2).

## 4. Diagnostics — the teaching moment

The rule's diagnostics are half its value; the shapes that ALMOST match must teach:

- `using var x := await FetchConfigAsync(url);` — **error**, new text: *"`await` on a `Task<Config>` produces a borrow of the task's result; `using var` requires an owned right-hand side (§30.5). The task itself is already scope-pinned by this bind — write `var x := await …;` (the result is valid to the end of this block), or use the explicit two-line form to name the task."*
- `x := await FetchConfigAsync(url);` (existing local) — **error**: *"the scoped await-bind applies to a fresh `var`/`const` declaration only: the hidden task is pinned to this block, but `x` outlives it. Bind the task first: `using var task := …; x := await task;` (§30.5)."*
- `return await FetchConfigAsync(url);` — unchanged (the chained-slot rule governs, kind 2 — the diagnostic there already names it).
- `_stored := await FetchConfigAsync(url);` (field) — unchanged error (no scope to pin to; §30.5's may-not-flow list).
- On request (info/inlay surface, non-normative): at an admitted scoped bind, *"the task returned by `FetchConfigAsync` is pinned to the end of this block; `cfg` borrows its result."* Wherever the slot participates in a later error: *"the result borrowed by `cfg` is owned by the awaited task pinned at line N, disposed at the end of that block."* (0051 §3.4's mitigation, adopted as the normative error-participation wording.)

## 5. Why this is not the general lifetime extension §5.24 declines — the containment argument

§5.24's posture: the K8 boundary is maximal and per-evaluation; borrows that would escape it are **rejected, never lifetime-extended**; the one recorded stretch (the `case` scrutinee) ends with the construct that syntactically contains it. This proposal asks for a **second bounded stretch of the same recorded kind**, and its containment is structural:

1. **One construct** — the declaration statement `var/const x := await E;`, initializer exactly the await. No inference: the trigger is a syntactic form plus one existing classification.
2. **One desugaring** — the hidden slot *is* a `using var` at the same line; the rule is definable (and implementable, C3) as macro-expansion into today's language. Nothing about K8 changes for any other expression: conditions, guards, arguments, stores, returns, and every non-await initializer keep the reject posture verbatim.
3. **One bound** — the innermost enclosing block, the same reader-computable bound the two-line form has. Contrast the declined general mechanism: arbitrary temporaries extended to arbitrary consumer lifetimes.
4. **The recording** — §5.24 gains the parallel paragraph its `case`-scrutinee entry's template anticipates: *the boundary ends with the enclosing block of the one binding form; everywhere else reject-not-extend stands; Bezel still has no general lifetime-extension mechanism.*

The honest asymmetry with the `case` precedent, restated from 0051 §3.3 rather than smoothed over: the scrutinee stretch was *forced* (rejecting it would have outlawed a long-admitted form); this one is **elective** — the two-line form always exists. An elective stretch needs evidence, which is §6.

**The cost, stated plainly (0051 §3.4, adopted):** the strongest objection is the **invisible lifetime** — `var cfg := await FetchConfigAsync(url);` reads as "cfg is the config" while an unnamed object holds the config until a scope end the line does not mention; a reviewer auditing disposal order must know the rule. Mitigations (the §4 diagnostics; inlay hints) narrow but do not remove this. It is the price of the feature and the reason the adoption gate below is answered rather than waved off.

## 6. The prior-art gate, answered — v0.182's decline and 0051 Part B

This exact mechanism carries a **recorded decline** (v0.182: "auto-binding the temporary to an anonymous scope-bound slot … kept as a liftable, tracked") and a **recorded study** (0051 Part B), whose deliverable was adoption criteria C1–C3. Revisiting a recorded decline requires new evidence against the stated reason; this proposal's evidentiary position, stated honestly per criterion:

- **C1 — the shape still dominates after the flip** (≥25% of Pattern A await sites are the bound-task-awaited-once shape, measured by the type-resolved protocol 0051 §3.5 fixed). **Not yet measured.** The v0.204 guidance flip has had six revisions and the stdlib waves to take effect; the raw directional counts at HEAD (~48 `using var t := …Async(…)` bindings in tests; the two-line pairs concentrated where Pattern A forces them) match 0051 §1.4's picture but are subject to its stated caveats. **This proposal makes the C1 run its stage 0** (§8): the compiler-assisted classification, run once, attached to #3048.
- **C2 — the admission pressure recurs.** The elegance review itself is the C2-shaped signal: an independent critic, reading v0.241 cold, ranked this friction **first** in the back-half report and identified the §30.5 admission list ("one of the longest single rules in the spec") as the cost center. Whether an owner-commissioned review's top finding satisfies C2's letter ("a further consumer-side admission is requested with enough force to be filed") is the owner's call — Q1 puts it squarely.
- **C3 — no new machinery.** The design above is the §28.10 cleanup-stack slot and nothing else — no new analysis, no freeze extension, no checked-mode change; the stage-1 prototype confirms or refutes this before the docs land (§8).

If C1 comes back under threshold and the owner does not waive, the decline path is 0051 §3.5's: a permanent DECLINED.md entry closing the v0.182 liftable — this proposal then converts to that record.

## 7. The larger question, named and fenced

The critic's alternative — *does class-typed `Task<T>` earn Pattern A's three-kind result-slot machinery at all, given that fan-out is rare and B is the default?* — is **out of this proposal's scope** and is not silently absorbed: it is a redesign of axiom **A3** (the two-container design), litigated, if ever, as its own numbered proposal against §30.5's producer taxonomy + rationale §12.1–§12.2, with the A3 register row as the recorded extension point and the v0.204 flip's evidence base as its starting corpus. This proposal is deliberately the *small* move: it keeps A3 untouched and makes Pattern A's consumers whole. Its adoption neither advances nor forecloses the larger question (a world that later retired Pattern A would retire this rule with it, having lost only the small implementation).

## 8. Staging

1. **Stage 0 — the C1 measurement** (no docs change): the 0051 §3.5 protocol, compiler-assisted, results attached to #3048. Exit: owner rules Q1 with the number in hand.
2. **Stage 1 — prototype** (C3's check): the slot on the cleanup stack behind a flag; confirm zero new analyses; run the §30.5 test family under it.
3. **Stage 2 — the docs revision**: §30.5 admission + diagnostics, §28.3/§28.10 sentences, C.6 rewording, §5.24's parallel paragraph, §12.2's sentence, changelog entry recording the lift of the v0.182 liftable with the gate's answers.
4. **Stage 3 — compiler conformance + tests**: the spec-cited families in §9.

## 9. Migration, compatibility, tests

- **Newly accepted:** `var/const x := await E;` for fresh Pattern A producers — a **pure widening**; the rule fires only on today's errors, so no existing program changes meaning or timing (changelog category: newly defined/accepted).
- **Newly rejected:** nothing. **Newly warned:** nothing.
- **Doc-only:** Guide 04 §3's "compile error newcomers hit" passage inverts into the feature's teaching example at graduation.
- **Tests shipped with the flip** (spec-cited; fail-without/pass-with): acceptance + disposal-timing of the canonical bind (scope end, reverse establishment order against a later `using var`); per-iteration disposal in a loop body; two binds in one scope; early-return and exception edges (LSan-clean under the AOT harness); the conditional-producer operand; `const` form (per Q2); rejection diagnostics for `using var x := await E;` and existing-local assignment (the §4 texts); non-firing at named operands (behavior unchanged); Pattern B binds unaffected (`var x := await AcquireAsync();` stays an owned bind).

## 10. Alternatives considered

1. **Status quo** (keep steering: Pattern B by default, two-line form for A). Declined as the *complete* answer by finding 1's evidence: the v0.204 flip was the cheap fix and was taken; the residual is every legitimate Pattern A consumer site — config caches and fan-out are retained *on purpose* (A3), and their consumers should not need to know K8 to bind a result. Status quo remains the fallback if the C1 gate fails (§6).
2. **A tenth §30.5 admitted position** (some further named-location relaxation). Declined: the list is the disease (three trains, nine bullets); the shape users write is the temporary-operand bind, which no named-position admission can reach.
3. **Transferring the result out of the task** (make `using var x := await t;` legal for Pattern A). Declined as unsound, per 0051 §7: Pattern A results may have multiple borrowers (§30.4); transferring out from under one dangles the rest. That semantics exists and is spelled `OwningTask<T>`.
4. **Syntactic peeking at await context** (producer pattern chosen per consumer). Declined; §30.5's own recorded principle ("no syntactic peeking") and §12.1's signature-carries-the-choice.
5. **Widening the rule to compositions or existing-local assignment.** Declined in v1 (§1, §2): each widening grows the invisible lifetime past "the two-liner you would have written here," which is the containment claim's entire content. Recorded liftables.

## 11. Open questions

- **Q1 — the gate.** Run stage 0 and hold adoption to C1's ≥25% threshold (0051's recorded protocol), or waive the gate on the strength of the elegance-review finding plus the owner's direction? Recommendation: **run stage 0 first** — it is one compiler-assisted query, the threshold was fixed in advance precisely so this moment couldn't be argued past it, and a clean number closes the v0.182 liftable properly in either direction.
- **Q2 — `const x := await E;`** admitted alongside `var`? Recommendation: yes (§2; same desugaring, borrow-compatible by §21.1/§5.27).
- **Q3 — `.Result` lockstep** (`var x := P().Result;`, completed-task startup code)? Recommendation: lockstep (v0.182 precedent; §30.3's machinery is already unified), stated in the same §30.5 amendment.
- **Q4 — diagnostics surface.** Adopt §4's error-participation wording as normative recommended-message text (recommendation: yes); inlay hints stay non-normative tooling.
- **Q5 — where the A3 question lives.** Confirm §7's fencing: a future numbered proposal + the A3 register row; this proposal's changelog entry cites it as explicitly not-advanced.

## 12. Interactions

- **0051 (task-ownership defaults):** Part A's flip is this proposal's context (Pattern A is now the *marked* case — the scoped bind serves its legitimate residue); Part B is this proposal's direct ancestor — its §3.1 rule statement, §3.2 soundness walk, §3.4 cost, and C1–C3 gate are adopted, cited, and answered here rather than re-derived.
- **0056 / 0090:** no contact — the await operand is an announced consume (K3.5/A7's operand note); no marker interacts with the hidden slot.
- **0080:** no contact.
- **0081:** complementary — 0081 warns where a fresh task is *abandoned* into a store; this proposal binds where a fresh task's *result* is wanted; between them the two fresh-task fates newcomers actually write are both handled.
- **§22.13(b) static caches / §30.5 kinds 2–3:** untouched; `return await _cfg!;` and the chained slots keep their own rules — the scoped bind never fires on named or chained operands.
- **TaskScope / structured concurrency (§30.10, rationale §12.12):** unaffected; the hidden slot holds a settled task, never an unsupervised running one.
