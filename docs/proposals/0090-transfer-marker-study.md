# Proposal 0090 — Study: one action verb for the transfer marker (`move` at every named-location consume; `owned` purely declarative)

| | |
|---|---|
| **Status** | under-review — drafted 2026-08-11 (owner-directed, from the elegance review). **A study in the 0076 mold**: complete inventory, a worked unified design, costed alternatives, and a staged recommendation with the owner's rulings enumerated — not a single rule change. |
| **Tracking issue** | [#3056](https://github.com/s8lap/bezel-lang/issues/3056) |
| **Target module(s)** | If adopted: **spec** — §22.3 (the call-site marker), §28.3 (*Move semantics for owned locals*, the producers rows, K3.4's family), §28.8.1 (MoveExpr operand class), §28.10 (adoption form), §29.2 (yield notes), §35.5 (owning-match trigger 2), §17.13 (discard cross-refs), axiom A7's register row, Appendix A (MoveExpr, MarkedTransfer, ArgModifiers, CaseStmt/CaseExpr, LocalDecl/AssignmentStmt commentary), Appendix C.3; **rationale** — §3.10 (a recorded reversal), §5.10; **guides** — 01 (the marker discipline sections), 04/05 (marker-bearing examples). **This is the most invasive proposal of the elegance-review set**, and the study says so plainly: it respells ~630 test-corpus lines and ~310 docs lines, touches the most-cited kernel section, and reverses a v0.169 adversarially-verified adoption. |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | Elegance review 2026-08-11, finding 2: to consume a value out of a named location the user learns a three-way spelling rule plus a position table — `owned` for bindings, `move` for fields/slots, nothing for announced targets, and a gap (A7) where no spelling exists — while one token, `owned`, means "this slot holds an obligation" at declarations and "consume this binding now" at use sites. Rationale §3.10 records that a `move` spelling was considered and `owned` preferred — for grammar-economy reasons ("fills the reserved `ArgModifiers` slot, mirrors `out owned var x`, avoids relaxing MoveExpr") — and **nothing in the record weighs the cost of one token carrying both a state meaning and an action meaning**. That unweighed cost is this study's subject. |

*Reference convention: unprefixed §-references are language-spec references; K/A tags are §28.3's kernel ids. Prior art read in full: §22.3, §28.3, §28.8.1, §35.5, axiom A7 (register + Appendix C), rationale §3.10/§5.10, proposal 0056 (the arm-position lift, incl. its critique record).*

---

## 1. Deliverable (i) — the complete current-state inventory

Every position where a consume of a class-typed value can occur, against K3's enumeration (the one definition site), with its current spelling. The table is exhaustive over K3.1–K3.12 + K9.1 + the recorded gaps; anything not listed is not a consume position in v0.241.

| # | Position | Kernel id | Current spelling | Spelling class |
|---|---|---|---|---|
| 1 | store into `owned` field / owning-collection slot / `set(owned)` accessor | K3.1 | `_f := x;` / `coll[i] := x;` — unmarked | **announced by target** |
| 2 | `return` under `owned` return; unmarked `Task`-family return (non-`async`) | K3.2 | `return x;` — unmarked | announced by signature |
| 3 | `owned` argument, **named owned binding** | K3.3 | `M(owned x)` — marker (§22.3) | **marker: `owned`** |
| 3′ | `owned` argument, fresh producer / `move` result | K3.3 | `M(new T())`, `M(move o.f)` — unmarked | fresh needs no marker |
| 4a | marked binding transfer | K3.4 | `var y := owned x;` / `using var y := owned x;` | **marker: `owned`** |
| 4b | marked assignment transfer | K3.4 | `y := owned x;` | **marker: `owned`** |
| 4c | marked owning-match scrutinee | K3.4 (§35.5 trigger 2) | `case owned x of …` | **marker: `owned`** |
| 4d | marked arm transfer (if-expression branches; expression-`case`/`when` arm results) | K3.4 (0056, graduated v0.209) | `… then owned x else …` | **marker: `owned`** |
| 5 | Pattern B `await` operand | K3.5 | `await t` — unmarked | announced (keyword + single-consumer type; A7 notes no operand marker exists) |
| 6 | Pattern A chaining `return await` over an owned source | K3.6 | unmarked | announced (form itself; K5.4 vacuity) |
| 7 | owning-iterator `yield return` of a named owned binding | K3.7 | `yield return x;` — unmarked | announced by declared return type; §29.2: *"there is no yield-position transfer marker — the same recorded, liftable bound as the arm positions of `??`"* |
| 8 | `throw` operand | K3.8 | `throw x` — unmarked | announced (keyword) |
| 9 | discard of a named owned binding | K3.9 | `_ := x;` — unmarked; `_ := owned x;` **is not a form** (§17.13) | announced (sink) |
| 10 | explicit `Dispose()` | K3.10 | `x.Dispose();` | the call is the spelling |
| 11 | reassignment (consume of the *displaced* value) | K3.11 | `x := fresh;` — unmarked w.r.t. the displaced value | implicit (freeze-guarded, K5.2) |
| 12 | move-only value stores | K3.12 | unmarked (type's own move semantics, §15.3) | type-announced |
| 13 | owned **field** take-out | K9.1 | `move x.f` | **marker: `move`** |
| 14 | owning **collection-slot** take-out | K9.1 (§28.11) | `move coll[i]`, `move span[i]` | **marker: `move`** |
| G1 | `??`-operand transfer | — (axiom A7, live half) | **no spelling exists** — a named owned binding there is a borrow arm | gap |
| G2 | operator-operand transfer | — (A7, live half) | **no spelling exists** | gap |
| G3 | binding-atom **body** transfer | — (§28.3 producers, BindExpr row) | **no spelling exists** — "the body is not a K3.4 marked-transfer position (no `owned x` body in this revision)" | gap |
| G4 | yield-position marker | — (§29.2's recorded liftable) | no marker; the *adopt-then-yield* idiom (`using var l := owned line; yield return l;`) is the workaround in borrowing iterators | gap (announced form exists in owning iterators, row 7) |
| G5 | named-destructure slots in an owning match | — (§35.5 case patterns; rationale §3.14's recorded liftable) | no transfer through named slots — positional destructure is the spelling | gap (recorded) |

The reader's burden, read off the table: two marker keywords (`owned`: rows 3, 4a–4d; `move`: rows 13–14) split by *location kind* (binding vs field/slot), eight unmarked-announced forms, one implicit form, and five gaps — and the same token `owned` appears declaratively in ~914 parameter declarations across the corpus (757 tests + 157 docs) meaning *state*, versus ~630 marker sites meaning *action*.

## 2. Deliverable (ii) — the unified design, worked position by position

**The design:** `move x` becomes the one action verb for every named-location consume that carries a marker today; `owned` becomes purely declarative (parameters, returns, fields, `out owned`, function-type slots, choice payloads, `[N owned]`, `static owned readonly` — every declaration position, untouched). Announced positions **stay unmarked** (K3.13's announcement rule is preserved, not extended — see the policy paragraph below).

**Grammar deltas (Appendix A):**

- `MoveExpr ::= "move" PrimaryExpr` — operand class **extended** with alternative (c): a bare `Identifier` naming a local binding or `owned` parameter (the current `MarkedTransfer`'s exact operand class). Alternatives (a) field access and (b) owning-slot index are unchanged.
- `MarkedTransfer ::= "owned" Identifier` — **deleted**; its positional-admission commentary (RHS-root of bindings/assignments, if-expression branches, expression-arm results — semantic, A.7's note) transfers verbatim to the identifier-operand `MoveExpr`.
- `ArgModifiers` — the bare `"owned"` alternative is **deleted**: `M(move x)` parses through the plain `ArgExpression` path, the marker now being *part of the expression*. (`"scoped"`, `"ref"`, `"out …"` alternatives unchanged — `scoped` marks a lending mode, not an action, and stays an argument modifier; the asymmetry is stated, not hidden.)
- `CaseStmt`/`CaseExpr` — the `"owned"?` scrutinee marker is **deleted**; `case move x of …` is an ordinary scrutinee expression, §35.5's trigger 2 re-keyed to "the scrutinee is an identifier-operand `MoveExpr`".
- `LocalDecl`/`AssignmentStmt` commentary — the binding/assignment-position marker note re-points at MoveExpr-at-RHS-root.

Because `move` is reserved and begins no other expression, every delta is additive-or-mechanical; the 0056 §2.1 atom argument (primary-level, binds tighter than every operator, no F2-class collision) transfers to the identifier operand unchanged.

**Position by position against the inventory:**

| Row | Under the unified design |
|---|---|
| 3 | `M(move x)`; required exactly where `M(owned x)` is required today (named owned binding), rejected on borrows/fresh/plain positions with the same error shapes; fresh producers stay unmarked (3′ unchanged) |
| 4a/4b | `var y := move x;`, `y := move x;`, `using var y := move x;` — identifier-MoveExpr at RHS root; polymorphic-null adoption, state-match, and drop-on-overwrite rules read the same classification |
| 4c | `case move x of …` — §35.5's trigger-2 semantics verbatim (frozen per K5.1, moved-from after `end`) |
| 4d | `… then move x else move y` — 0056's consume-if-selected, K7.4 join, condition-side non-freeze, scrutinee-arm always-frozen note: all verbatim (the arm semantics never depended on the token) |
| 13/14 | `move x.f`, `move coll[i]` — unchanged; the K9 restoration rule vs K7.4 maybe-consumed split stays per-location-kind, as today |
| 1–2, 5–9, 12 | unchanged and unmarked — see the policy next |
| 10–11 | unchanged (the call; the freeze-guarded implicit) |

**Policy at announced targets (a ruling the study puts to the owner as Q2):** with `move x` an ordinary owned-producing expression, `return move x;` under an `owned` return, `throw move x`, `_ := move x;`, `sink._f := move x;`… all *type-check naturally*. The study recommends **rejected-as-redundant** — one spelling per site, matching the existing discipline ("writing the marker on a borrow, a fresh producer, … is likewise an error", §22.3): announced targets stay unmarked, `move`-at-announced-target is an error naming the announcement. The alternative — *admitted-redundant* — is costed under Q2: it buys grep-ability ("every consume says move") at the price of two legal spellings for eight positions, the exact both-ways surface the marker discipline was built to avoid.

**The reading-model argument, made explicitly (the record's missing half):** after the change, `owned` appears **only** where a *slot* is described — parameter lists, return types, field declarations, `out owned var x` — and always answers "what obligation does this location hold?"; `move` appears **only** where an *action* occurs — and always answers "this named location is being emptied, now, here." Today the same reader-facing token answers both questions, disambiguated by position: `TakeOwnership(owned resource)` is a callee describing its parameter's state; `TakeOwnership(owned resource)` is *also* what a caller writes performing the action — literally the same character sequence meaning declaration in one file and invalidation in another. The unified design makes the two grammatical roles two tokens, and the `move` verb's meaning becomes location-uniform (binding, field, or slot: "transfer out, leave moved-from"), deleting the three-way spelling rule and the position table from the newcomer's load. §5.10's three recorded reasons for explicit `move` at fields — review visibility, refactor stability, the exception-path obligation — argue *for* this extension: they are reasons for an action verb at every take-out site, and none of them is field-specific.

**What the design deliberately does not do:** it does not extend markers to the announced set (K3.13 preserved); it does not touch the declarative grammar (no declaration site changes spelling); it does not by itself lift the A7 gaps (§5); it does not change any kernel rule, join, freeze placement, or disposal point — the flip is spelling-only, and the acceptance test is that every K-tagged rule reads unchanged with "the marker" re-pointed.

## 3. Deliverable (iii) — the costs, not understated

**The migration table** (counts by substring grep at v0.241 HEAD; methodology: `M(owned x)` counted as `\(owned ident[,)]` + `, owned ident[,)]` — parameter declarations excluded by their `:`; the `:= owned` row includes its `using var` subset; overlaps between rows are possible on multi-marker lines, so totals are ceilings-by-row, not a disjoint sum):

| Spelling that changes | tests/ | docs/ | Becomes |
|---|---|---|---|
| `M(owned x)` argument marker (first-arg + mid-arg) | 416 + 37 | 131 + 24 | `M(move x)` |
| `var y := owned x;` / `y := owned x;` / `using var y := owned x;` | 162 (incl. 75 `using var`) | 116 (incl. 64) | `:= move x` |
| `case owned x of` | 7 | 23 | `case move x of` |
| arm markers `then/else owned x` | 11 | 17 | `then/else move x` |
| **Total changed lines (ceiling)** | **~633** | **~311** | |
| `move x.f` / `move coll[i]` | 208 + 137 | 48 + 114 | unchanged |
| `out owned var x` | 197 | 12 | **unchanged** (see below) |
| parameter/field/return declarations `owned …` | 757 (params alone) | 157 | unchanged |

**The `out owned var x` seam, argued:** it stays `owned`. The form *declares a fresh owned binding* at the call site — a declaration position that happens to sit inside an argument list, exactly parallel to `out using var x`. Nothing is consumed (the binding is being *created* into the owned state); under the reading model it is a state description and lands on `owned`'s side of the line. The v0.169 record cited "mirrors `out owned var x`" as a reason for the `owned` action marker; the unified design answers: the mirror was between a declaration and an action wearing the same token — precisely the conflation under study. `out owned var x` mirroring `owned p: T` (declaration ↔ declaration) is the coherent mirror, and it survives.

**Teaching-material churn:** Guide 01's marker-discipline passages (the "unmarked reads never consume; write `owned x` where you mean transfer" rule collapses to "…write `move x`…" — a *simplification*, but every worked example respells); Guide 04/05's `using var c := owned conn;` idioms (~15 sites); the spec's own worked examples across §28/§29/§35 (the docs column above). The changelog carries the reversal entry; rationale §3.10 is amended as a **recorded reversal with the new evidence named** (the unweighed reading-model cost) — the v0.169 entry itself is pinned history, untouched.

**The deprecated-alias question (Q3):** does `owned x` survive as an accepted synonym during a migration window? With **zero external users**, the study recommends **no alias — one-revision hard flip**: the corpus rewrite is mechanical (four regex families, reviewed by the gate), an alias means specifying both spellings' interactions for a window nobody needs, and the one-time cost is the lowest it will ever be. This "now or never" timing fact cuts both ways and is stated as such: it is the strongest argument *for* deciding now (either direction), not for adopting.

**What is genuinely at risk:** §28.3 is the most-cited section in the project; a respelling touches dozens of normative examples, and transcription errors in a 900-line-diff docs train are the realistic failure mode (mitigation: the flip's docs train is mechanical-first — regex passes, then hand review of the ~40 prose sentences that *name* the marker — and `check_docs.sh` gates it). The kernel is untouched by design, but proving that is stage-2's acceptance test: the full suite under both oracles with only spelling-diff test changes.

## 4. Deliverable (iv) — partial alternatives, each costed

- **Alt A — `owned` at call arguments only; `move` everywhere else** (bindings, assignment, scrutinee, arms). *For:* preserves the argument marker's echo of the parameter declaration it feeds (`ref`/`out` mirror exactly so); shrinks the migration by the largest row (~453 test lines). *Against:* the K3.4 family — today ONE spelling — becomes two (`var y := move x;` but `M(owned x)`), so the position table this study exists to delete survives in miniature; the declaration/action conflation survives at precisely its highest-frequency site; and the `ref`/`out` mirror analogy is imperfect — `ref x` at a call site does not *invalidate* `x`, so its declaration-echo carries no action ambiguity. **Net: worse than both endpoints; costed and not recommended.**
- **Alt B — add `move` as an accepted synonym everywhere the marker exists; remove nothing.** *For:* zero forced migration; grep-for-`move` finds every marked consume. *Against:* two legal spellings for one meaning, forever — style fragmentation the docs must either pick a side on (de-facto migration anyway) or stay neutral on (the reader's burden *grows*: both spellings must be learned); the discipline "writing the marker where it isn't required is an error" loses its shape. **Net: the worst long-run state; viable only as a never-finish migration.**
- **Alt C — status quo + teaching investment** (a §28.3 summary table of the spelling rule; Guide 01 box). *For:* zero risk, zero churn; the marker system is sound and the complaint is pedagogical. *Against:* the complaint is not *only* pedagogical — the same-token conflation is permanent reader overhead every review pays, and the A7/G3/G4 gaps stay unspellable (each future lift then extends the **`owned`** marker family and deepens the investment against ever unifying). **Net: the honest decline path; if chosen, §3.10 gains the re-examined-and-kept record and this study is its evidence.**

## 5. Deliverable (v) — the A7 gap, treated on its own

A7's live content (post-0056 narrowing): no transfer spelling exists in **`??`-operand** and **operator-operand** position; the register row's recorded reason is *"keeps no-silent-consume (K3.13) uniform where no marker spelling exists."*

**Can the unified marker fill the positions?** *Syntactically, yes, trivially:* `move a ?? move b` parses today's grammar shape with zero ambiguity — `move` prefixes, binds as a primary, and the F2 collision that sank 0056's first draft (the binding-slot marker `":=" "owned"? Expression` colliding with an arm marker) **cannot arise**, because under unification there is no separate binding-slot production at all. This dissolves the *syntactic* half of A7's recorded reason: after the flip, "no marker spelling exists" is false — a spelling exists and is *semantically rejected*, pending the kernel rulings. A7's register row must therefore be reworded under adoption (its content becomes purely semantic), and the study flags this as a mandatory consequence, not an option.

**The semantic half stands, per 0056's record:** the `??` lift needs the **null-evaluated-transfer ruling** — the post-state of `move a` evaluated while `a` holds `null` (0056 Q1's recorded open rule; the natural answer — the read of null transfers nothing, the binding stays owned-currently-null — is coherent but genuinely *new*, must land as a recorded ruling, and interacts with K7.5's absorption). Operator operands remain unmotivated (0056 §6/Q1: no use case on record). **Recommendation:** the flip ships with A7's positions still rejected (`move` there is a compile error naming the narrowed axiom, exactly as `owned` is today per the MarkedTransfer commentary); the `??` lift is stage 3, gated on the Q1 ruling — sequenced *after* the flip so the new rule lands in the unified spelling and is never written twice. 0056's arm-position ground (consume-if-selected as a pure K7.4 derivation; guard exclusion; scrutinee-arm freeze) carries over token-for-token and needs no re-derivation.

The remaining gaps: **G3** (binding-atom body) and **G4** (yield-position) each become one-line lifts *in the unified spelling* if ever taken (`(var x := E; move x)`; `yield return move x;` in a borrowing iterator as an explicit enumerator-transfer) — both stay recorded liftables, neither is proposed here; **G5** (named-destructure transfer) is §3.14's recorded liftable and is unaffected by the token choice.

## 6. Deliverable (vi) — staged recommendation and the owner's rulings

**The study's verdict, with its confidence stated:** the reading-model argument is real, the migration is pre-user-feasible **now and never again cheaper**, and the semantics are provably untouched (spelling-only flip) — the study recommends **adoption via the hard flip** (no alias), at **medium confidence**: the v0.169 grammar-economy reasons were real and two of them survive as permanent small costs (the vestigial-`ArgModifiers` cleanup is mild; the `scoped` argument-modifier asymmetry remains), and Alt C is a defensible decline whose record this study pre-writes. What the study asks is that the ruling be made *on the reading-model question the v0.169 record never weighed* — that is the new evidence that licenses reopening a recorded decision.

**Stages (on adoption):**
- **Stage 0** — the owner rules Q1–Q3.
- **Stage 1** — the docs revision: grammar deltas (§2), §22.3/§28.3/§28.8.1/§35.5/§29.2/§17.13 respellings, A7 row rewording, rationale §3.10 reversal record + §5.10 note, guides; mechanical-first with the hand-review pass; one changelog entry.
- **Stage 2** — compiler flip + corpus: parser (identifier-operand MoveExpr; deletions), sema (positional admission transferred; diagnostics respelled — every "write `M(owned x)`" fix-it becomes "write `M(move x)`"), the four-family regex rewrite of tests (~633 lines) landed as its own commit, both oracles green with spelling-only test diffs as the acceptance criterion. Spec-cited regression tests: marker-required/marker-rejected at each K3.4 position and argument position in the new spelling; `case move x of` freeze + moved-from; arm-position consume-if-selected; `move`-at-announced-target rejection (per Q2's ruling); A7 positions still rejected with the reworded diagnostic.
- **Stage 3 (gated)** — the `??` lift under 0056 Q1's ruling, in the unified spelling.

**The owner's rulings, 0077-Q-style:**

- **Q1 — adopt, decline, or Alt?** Adopt the unified marker (recommended, medium confidence); or decline and record in §3.10 as re-examined-and-kept with this study as the evidence; or one of §4's alternatives (each costed there; none recommended).
- **Q2 — announced-target policy.** `move` at announced targets (`throw`, discard, owned returns, owning `yield return`, Pattern B await has no operand-expression seam) rejected-as-redundant (recommended — one spelling per site) or admitted-redundant (grep-ability at the cost of dual spellings)?
- **Q3 — migration mode.** Hard flip, no alias (recommended — zero users; both-spellings specification cost avoided) or a deprecated-alias window (`owned x` accepted with a warning for N revisions)?
- **Q4 — the A7 sequencing.** Confirm stage 3's gating (the `??` lift lands only with the null-evaluated-transfer ruling, after the flip) — recommended; the alternative (bundle it into the flip) couples a genuine new kernel rule to a spelling migration, the coupling 0056's critique record warns against.
- **Q5 — `out owned var x`.** Confirm it stays `owned` (recommended; §3's declaration-position argument) — the one seam where a reader might expect the verb.
- **Q6 — plain-position `move`.** Confirm `M(move x)` at a *non*-`owned` parameter position stays an error (the argument would be an owned expression feeding a borrow slot — same state-match rejection as today's misplaced marker), with the diagnostic naming the borrow default. (Transcription of the current rule; listed because the deleted `ArgModifiers` path moves the check into expression classification.)

## 7. Migration & compatibility (changelog house categories)

On adoption: **newly rejected** — every current `owned`-marker spelling (`M(owned x)`, `:= owned x`, `case owned x of`, arm `owned x`), each with a fix-it naming the `move` respelling (mechanical, one token); **newly defined** — the identifier-operand `MoveExpr` and its positional admission (semantics identical to the forms it replaces); **doc-only** — everything in stage 1; **unchanged** — every declaration-position `owned`, `move x.f`/`move coll[i]`, all announced/implicit consume positions, all kernel rules, every disposal point and diagnostic *semantics*. On decline: doc-only (§3.10's re-examination record).

## 8. Interactions

- **0056** — the superseded ground this study builds on: its arm semantics, atom argument, and critique record (F2) are load-bearing inputs; nothing is re-litigated.
- **0083 (front-half sibling, reserved `_`)** — reads the same §17.13/§22.3 surface: the discard rows (9) and the `out _` forms are inventory rows here; under adoption, §17.13's "no `_ := owned x;` form" note respells to "no `_ := move x;` form" (the sink stays the announcement) — flagged so the two graduation trains sequence their §17.13 edits.
- **0080** — row 8 (`throw`) is 0080's operand; both proposals leave its announced, unmarked status untouched (stated in both).
- **0081** — diagnostics in 0081's remedies quote marker spellings; a wording dependency on Q1's outcome, graduation-ordering only.
- **0082** — no contact (await operands are announced; the hidden slot is inexpressible).
- **0088** — `sourced` is a declaration-position marker and lands on `owned`'s side of the reading-model line under any Q1 outcome; checked for collision, none.
- **§15.3 move-only values** — unmarked moves by type (row 12) are untouched; the study notes the residual vocabulary overlap (`move`-the-verb vs "move-only"-the-category) is already present today via `move x.f` and does not worsen.
