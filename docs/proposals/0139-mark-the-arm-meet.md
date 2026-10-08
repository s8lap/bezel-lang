# Proposal 0139 — The arm-meet is marked too: a `using var` over an `if`, `case` or `??` whose every arm is an unmarked named owned binding states no intent, so it is rejected

| | |
|---|---|
| **Status** | graduated-v0.327 — superseded — graduated v0.327 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option B; critique round 1 applied, 2 blocking and 8 non-blocking findings resolved; landed with the v0.327 changelog entry) |
| **Tracking issue** | [#5591](https://github.com/s8lap/bezel-lang/issues/5591) (the question); the implementation tracker is filed at the v0.327 landing, as an extension of proposal 0137's tracker (§5.5) |
| **Target module(s)** | **spec** — §28.10 (the arm-meet paragraph, line 7334), §28.3 (*Ownership state of locals*, the `using var` bullet, line 6477; *Borrows*, the arm bullet, line 6560), Appendix C.3 (the v0.323 row, line 11818); **rationale** — §5.27 (the concession sentence, line 994); **code-generation reference** — §4.2 (the v0.323 bullet, line 471), §4.6 (the *Recommended wordings* row, line 652), §7.2 (one new *Looks right, is rejected* pair after the 0137 pair, line 1802); **`DECLINED.md`** — three rows (§5.6); **Guide 01** — §9's arm parenthetical (line 366; one clause, anchored on text the v0.326 guide re-trace leaves intact); **rule corpus** — one new `reject` case, handed to the implementing tracker (§5.5); **compiler** — the arm-meet branch of `src/ownership/Ownership.cpp:6782–6787` and a `??` branch beside it (§5.4). |
| **Written against** | spec **v0.325** at `b27728a` (main). The stdlib companion, rationale, code-generation reference and Guide 01 are at the same commit. Line numbers are at that commit. |
| **Origin** | Proposal 0137 graduated at v0.323 (option B): an unmarked `using var y := x;` over a class- or interface-typed named owned binding is a compile error, because the spelling does not say whether `y` adopts `x` or views it. Its graduation lane filed the neighbouring shape as a question (`scratch/v0323/g0137.md` (e)1, [#5591](https://github.com/s8lap/bezel-lang/issues/5591)): `using var y := (if c then a else b);`, the expression-`case` form and `using var y := a ?? b;` over named owned arms stay silent borrow-pins under the v0.323 text. The owner delegated the decision on 2026-10-08. |

*Reference convention: unprefixed §-references are language-spec references at `b27728a` (v0.325); K tags are §28.3's kernel ids; rationale, code-generation-reference, guide and proposal references are marked. Quotations are verbatim. ▲ marks proposed text, landed nowhere. The programs below use only members the stdlib companion declares; none invents an API.*

---

## 0. Thesis

v0.323 made the reader's rule for `using var` exception-free at the one position where it had an exception: **a producer adopts; a borrow pins; a named owned binding is marked `move`** (proposal 0137 §5.1). One position was left where the same two readings are both plausible and the spelling chooses neither: an arm-meet. `using var y := (if c then a else b);` over two owned locals is, per §28.3's producers table, a borrow of the selected binding with the union of sources, so the `using var` is a borrow-pin of both; `using var y := (if c then move a else move b);` is an adoption. Which one the unmarked form means is decided by the ownership state of `a` and `b` — the fact the reader has to look up, exactly as at the bare `x` (0137 §0). The gap also admits the rejected form back in by a grouping: `using var y := (if true then x else x);` is `using var y := x;` with a tautology, and the K7 meet makes it the pin v0.323 retired.

This proposal extends v0.323's rejection to the arm-meet whose **every** arm is such a read — `null`-literal and `throw` arms set aside, since they absorb (K7.5), and a nested arm-meet examined by its own arms — for the three arm-meet forms (`if` expression, expression-position `case`, `??`) and the three binding spellings (`using var y`, `using var _`, `await using var y`). A meet with any other borrow arm (a call without an owned return, a field, an indexer, a borrow-state binding) has no adoption reading by the arm marker alone — marking its named owned arms would make the meet mixed, which K7.3 rejects, and an owned field arm adopts only as `move o.f` under §28.8.1's restoration obligation — so it stays a pin, as a borrow-state source and a bare field read do under 0137. The remedies are the ones the spec already names: the marked arms where the arm position admits the marker (§20, §19.2), the `(if a <> null then move a else move b)` rewrite the `??` row names where axiom A7 rejects it, or a `var`/`const` view. **The corpus has no affected site**: zero in `tests/`, zero in the rule corpus, zero example code in the documents (§2). Kernel: a derivation of K3.4, K7 and A5, no row changed (§5.2); body-locality: local (§5.3). Four options are worked (§3); **B is recommended and, under the delegation, decided**.

---

## 1. Ground truth

### 1.1 The rule today

- **§28.10, the arm-meet paragraph (line 7334):** "An expression whose arm-meet is **owned** — a `??`, an `if` expression, or an expression-position `case` over owned producers (the K7 meet; §28.3) — is an owned producer for this section's admission … (An arm that is an *unmarked* read of a named owned binding is a borrow arm per the producers table — the whole expression is then a borrow and the `using var` a borrow-pin; a **marked** arm `move cached` is an owned arm.)" This is the sentence that states the pin this proposal retires.
- **§28.10 *Adopting a named owned binding* (line 7338, v0.323):** "An **unmarked** `using var y := x;` is a **compile error** when its right-hand side is a named owned binding whose declared type, nullability stripped, is a class or interface type. This includes the null-forgiving read `x!` …, a parenthesized `(x)` …, the anonymous `using var _ := x;` and `await using var y := x;`." And: "The covered spelling does not say whether `y` adopts `x` or views it, and the compiler does not choose."
- **§28.10 *`using var` initialized from a borrow* (line 7372):** the surviving pin shapes — "a method call without an `owned` return …, an indexer access, a field read, or an unmarked read of a **borrow-state** named binding (a plain parameter, a borrowing alias or loop variable, a captured owned binding inside a closure)".
- **§28.3 producers table, the `??` row (line 6548):** "A **named owned binding** written as an arm is an *unmarked read* and therefore a **borrow** arm (K3.13; the transfer marker is rejected in `??`-operand position — axiom A7): `a ?? b` over two named owned locals consumes neither binding. To coalesce two named owned bindings into one owned result, use the marked arm transfer — `var r := (if a <> null then move a else move b);` (K3.4) — or equalize by statement. A `null`-literal or bottom-typed (`throw`) arm absorbs to the other arm's state (K7.5)". **The `if`/`case` row (line 6549):** "the **marked arm** `move x` *is* admitted here (axiom A7 rejects it only in `??`-operand position), an **owned** producer and K3.4 transfer … An **unmarked** read of a named owned binding remains a **borrow** arm (K3.13 — no silent consume)".
- **§28.3 *Borrows* (line 6560):** the borrowing positions include "an operand of a null-coalescing expression (`a ?? b` — the table's `??` row then sees a borrow arm; the marker is rejected in that position, axiom A7), or an **unmarked** if-expression branch or expression-`case` arm result (the arm-meet row sees a borrow arm; the *marked* form `move x` in those positions is instead the K3.4 arm transfer)".
- **K7 (line 6419):** "borrowed ⊓ borrowed = **borrowed**, carrying the union of the arms' source sets (K2.2); owned ⊓ borrowed = **rejected**"; **K7.5**: "`null`-literal and bottom-typed (`throw`) arms absorb to the other arm's state." **A7 (line 6462):** "The transfer marker is not admitted in `??`-operand or operator-operand position … The `??` half awaits the null-evaluated-transfer ruling" (`DECLINED.md` line 479, `deferred`).
- **§20 (line 4135)** and **§19.2 (line 3933)** restate the arm-meet for the `if` expression and the expression-`case`: "an *unmarked* read of a named owned binding written as a branch is a borrow arm, and the **marked** branch `move x` … transfers".
- **§28.3 *Ownership state of locals* (line 6477, v0.323):** "When `expr` is a borrow — for example a call without an owned return, a field or indexer read, or an unmarked read of a **borrow-state** named binding — `x` is instead a **borrow-pin** … an unmarked read of a **class- or interface-typed** named **owned** binding is rejected there". The v0.323 lane prefixed the list with "for example" precisely because "arm-meets and binding atoms are borrows too" (`g0137.md`, deviation 3).
- **Appendix C.3 (line 11818):** the v0.323 invariant row — "A `using var` or `await using var` whose initializer is an unmarked read (`x` or `x!`, parenthesized or not) of a named owned binding … is rejected; `move x` adopts … and a `var` or `const` binding views".
- **Rationale §5.27 (line 994, v0.323):** "at a bare named owned binding, where both readings would be plausible, the form is rejected (v0.323), so the split is decided by the right-hand side's visible shape: a producer adopts, a borrow pins." The v0.323 critic dropped "the one right-hand side" from this sentence because the arm-meet contradicted it (`g0137.critic.md` N5).
- **Code-generation reference §4.2 (line 471):** the v0.323 bullet, with its covered-shape list "(… `x!`, `(x)`, `using var _ := x;` and `await using var y := x;` alike)"; **§4.6 (line 652)**: the recommended wording; **§7.2 (lines 1790–1802)**: the `using var c := conn;` / `move conn` pair. **Guide 01 §9 (line 366):** "(Write two *named* owned locals as **unmarked** arms and the expression becomes a borrow merge, exactly as with `??`. To coalesce them into one owned result, mark the arms: `using var best := (if a.Score() > b.Score() then move a else move b);` …)".
- **`DECLINED.md`:** rows 677–679 (0137's options A, C, D, `declined-at-proposal`) and 737 (the unmarked named-binding pin, `retired`). None records a decision about the arm-meet at a `using var`; the v0.323 lane filed it as open.

### 1.2 The compiler today (`src/ownership/Ownership.cpp` at `b27728a`)

- **6655–6657:** `checkArmMeetMixed` runs on a `ConditionalExpr`/`CaseExpr` initializer of any local (K7.3's mixed rejection, #1661).
- **6782–6787:** the `using var` pin branch for `ConditionalExpr`/`CaseExpr`: "producerState is the authority" — when the meet is not Owned, `usingVarSourceIsBorrow = true` (the pin; codegen then registers no disposal, `LlvmBackend_Setup.cpp:20284`). No rejection exists; this is the v0.325 pin. **Spec-ahead-of-impl** after this proposal.
- **5923–5933:** `producerState`'s built-in `??` arm is Owned only when the right operand is Owned; over two named owned bindings it falls through to Borrowed. **No branch of the `using var` layer (6715–6792) recognizes a `BinaryExpr` `??`** — `usingVarInitIsRecognizedBorrow` (6283) handles field, property and identifier shapes only — so `using var y := a ?? b;` records the binding Owned + implicitDischarge with the pin flag **clear**. HYPOTHESIS (source reading, no probe; `build/bezelc` is absent in this checkout): codegen then disposes `y` at scope end while the selected source also auto-drops — a double free. The same reading applies to `using var y := p ?? q;` over two plain parameters, where the spec's verdict is a pin and the hypothesized behaviour disposes a caller's object. Probe for the tracker: both programs under `BEZEL_AOT_FORCE_SANITIZE=address`. Under this proposal the named-owned shape is rejected, which removes the first program; the borrow-state shape stays a pin and keeps the question.
- **`src/ast/Node.hpp:157–158`:** `isAwaitUsing` implies `isUsing`, so every branch above covers `await using var` too.

---

## 2. Measurement: what the existing sites mean

**Method.** `grep -rnE` over `tests/**/*.{cpp,bzl}` and `docs/*.md` for a `using var` / `await using var` whose initializer begins `(if `, `(case `, `case `, or is `<id>!? ?? …`, on one line and with the initializer on the following line; the changelog is excluded (pinned history). Every match was read.

| Shape at a `using var` | Sites | Verdict today | Under this proposal |
|---|---|---|---|
| Owned arms (`new`, an `owned`-returning call, `move x` arms) | 26 (`marked_arm_transfer_2080`, `arm_meet_ownership_1661`, `record_moves_arm_and_throw_3989_3990`, `checkread_expression_walk_3971`, `complex_move_place_receiver_4063`, the owning-match `case move o of` rows of #4633/#4944/#5134, rule cases `s20_0006/0007/0008/0014`, `s28_10_0003`, doc cases `guide1_0018`, `codegen_0050`; spec 7334, Guide 01 359, codegen reference 1818–1819) | adoption | unchanged |
| Retired-marker arms (`owned a else owned b`, `1 => owned a`) | 3 (`record_moves_arm_and_throw_3989_3990:391`, `marked_arm_transfer_2080:134, :232`) | adoption via the retired spelling the parser still accepts (0137 §6 item 7) | unchanged by this proposal; the marker's retirement is #5576's |
| Mixed meet (an owned arm and a borrow arm) | 4 (`arm_meet_ownership_1661__named_owned_binding_arm_demotes_rejected.bzl:25`, `…__mixed_if_expr_rejected.bzl:28`, `…__mixed_case_expr_rejected.bzl:25`, `coalesce_mixed_meet_4964:185`, rule case `s7_7_0003` — `fallback ?? new Widget()` over a plain parameter) | rejected (K7.3) | unchanged |
| **Borrow-state arms** | 1 (`arm_meet_ownership_1661_test.cpp:112`: `using var x: Res := (if c then b1 else b2);` where `var b1 := owner; var b2 := owner;` are borrowing aliases of a `using var`) | pin | **unchanged** (no arm is a named *owned* binding) |
| **Every arm an unmarked named owned binding** | **0** | pin | **rejected** |

**Where the affected sites are.** Nowhere. The one `docs/` match of the shape is §28.10's own statement of the rule (line 7334); Guide 01 line 366 describes the shape in prose without writing it as an example. The rule corpus has no case stating the arm-meet pin at a `using var` (`manifest.tsv` grep for `arm`, `meet`, `??`, `borrow-pin`: `s7_7_0003` is the mixed rejection; `s28_10_0003`, `s20_0006/7/8/14` are owned meets). The stdlib writes no arm-meet at a `using var`.

**What the pin over such a meet buys that nothing else can.** The same freeze 0137 §2 examined, over whichever of two owned bindings a condition selects, while both stay readable by name. 0137 §5.1 recorded that freeze's loss over the bare binding as the accepted cost (cases (a)–(c)); the conditional form of it is used by no site in the corpus, and its view substitute is `var v := (if c then a else b);`, which freezes both sources while `v` is live (K5.1).

---

## 3. Options

The common core of B, C and D is one rule (▲, §28.10's arm-meet paragraph, replacing its parenthetical; option B's scoping is the text shown, with the differences for C and D stated in their sections):

> ▲ An arm that is an *unmarked* read of a named owned binding is a borrow arm per the producers table; a **marked** arm `move cached` is an owned arm. When **every** arm of the meet — `null`-literal and `throw` arms set aside, since they absorb ([K7.5](#sec-kernel-k7)); a nested `??`, `if` or `case` arm examined by its own arms — is an unmarked read of a class- or interface-typed named owned binding (the set *Adopting a named owned binding* below covers, `x!` and `(x)` included), and at least one arm is, the `using var` is **rejected** on that paragraph's ground: `using var y := (if c then a else b);` and `using var y := a ?? b;` over two owned locals say neither that `y` adopts the selected binding nor that it views it, and the compiler does not choose. The remedies are the marked arms where the arm position admits the marker — `(if c then move a else move b)`, an expression-`case` likewise ([§20](#sec-the-if-expression), [§19.2](#sec-arms)), except at an arm naming a `using var` source (axiom [A5](#sec-axiom-register)) or the `case` scrutinee's own binding ([K8.3](#sec-kernel-k8)); for `??`, where axiom [A7](#sec-axiom-register) rejects it, the `(if a <> null then move a else move b)` rewrite the producers table's `??` row names — an absorbing arm kept as written, so `x ?? throw new E()` becomes `(if x <> null then move x else throw new E())` — or a statement; or a `var` or `const` binding for a view ([§21.1](#sec-variable-declarations)). The anonymous form and `await using var` are covered as *Adopting a named owned binding* covers them. A meet with any **other** borrow arm — a call without an owned return, a field, an indexer, a borrow-state binding — is not covered: marking its named owned arms alone would make the meet mixed ([K7.3](#sec-kernel-k7)), an owned field or slot arm adopts only as `move o.f` under [§28.8.1](#sec-the-move-expression)'s restoration obligation, as at a bare field read, and the `using var` is a borrow-pin of the union of sources (*`using var` initialized from a borrow*, below).

### 3.1 Option A — status quo: the arm-meet stays a silent pin (interpretation (ii) of #5591)

**Rule text.** None. **For:** the arms are syntactically visible; the producers table already says what an unmarked arm is. **Against:** (1) visibility of the arms is not visibility of their *ownership state* — `a` and `b` read the same whether they are owned locals or plain parameters, and only in the first case does the reader need to know that `using var` does not adopt; this is 0137 §0's defect verbatim, one parenthesis deeper. (2) The retired form is reachable through a grouping: `using var y := (if true then x else x);` and `using var y := x ?? x;` are `using var y := x;` under K7, so v0.323's rejection has a loophole whose only content is a tautology. (3) Rationale §5.27's conceded ambiguity is then still open at a position the v0.323 critic had to carve the concession sentence around (N5). **Verdict:** declined (§5.6).

### 3.2 Option B — reject the meet whose every arm is an unmarked named owned read (recommended; interpretation (i), scoped)

**Rule text.** The ▲ paragraph of §3 as written. **Secondary edits** (exact `old→new` in the landing's edit list):

- **§28.3 *Ownership state of locals*, bullet 6477:** "an unmarked read of a **class- or interface-typed** named **owned** binding is rejected there" → "an unmarked read of a **class- or interface-typed** named **owned** binding — or an `if`, `case` or `??` arm-meet whose every arm is one — is rejected there".
- **§28.3 *Borrows*, bullet 6560:** append to the arm clause: "— at a `using var`, an arm-meet whose every arm is such a read is rejected ([§28.10](#sec-using-var))".
- **Appendix C.3, row 11818:** "whose initializer is an unmarked read (`x` or `x!`, parenthesized or not) of a named owned binding" → "whose initializer is an unmarked read (`x` or `x!`, parenthesized or not) of a named owned binding — or an `if`, `case` or `??` arm-meet whose every arm is one, absorbing arms aside —".
- **Rationale §5.27, line 994:** "at a bare named owned binding, where both readings would be plausible, the form is rejected (v0.323)" → "at a bare named owned binding, and at an `if`, `case` or `??` arm-meet of such bindings alone, where both readings would be plausible, the form is rejected (v0.323, v0.327)".
- **Code-generation reference §4.2, bullet 471:** the covered-shape list gains "and an `if`/`case`/`??` arm-meet whose every arm is such a read (`(if c then a else b)`, `a ?? b`)"; **§4.6, row 652:** the row's first cell gains "or an arm-meet of such bindings alone" and the wording gains the arm form; **§7.2:** one new *Looks right, is rejected* pair after the 0137 pair — `using var best := (if c then a else b);` → `using var best := (if c then move a else move b);`, with the `??` rewrite in the gloss.
- **Guide 01 §9, line 366:** "(Write two *named* owned locals as **unmarked** arms and the expression becomes a borrow merge, exactly as with `??`. To coalesce …)" → "(… exactly as with `??` — and at a `using var` a compile error, since the declaration would say neither that it adopts the selected local nor that it views it (spec §28.10). To coalesce …)".
- **§20 (line 4135), §19.2 (line 3933), the producers table rows 6548–6549, §17.13, §28.12:** unchanged in text; each stays true, and §28.10 is the rule's one home.

**Diagnostic.** `error: `using var y := (if c then a else b)` does not say what happens to `a` and `b`: mark the arms — `(if c then move a else move b)` — to adopt whichever is selected (y disposes it at this block's end; that binding is left moved-from on its path), or write `var y := …;` / `const y := …;` to view it (§28.10, v0.327)` — with the `??` variant naming the `(if a <> null then move a else move b)` rewrite (an absorbing arm kept as written: `x ?? throw new E()` → `(if x <> null then move x else throw new E())`), and the conditional remedies 0137 fixed carried over: the `move` remedy is dropped for an arm that is a `using var` source (A5) or the `case` scrutinee's own binding (K8.3, §19.2), the anonymous form's view is replaced by deleting the declaration, and a `Task`-family anonymous bind is reported under A6 first.

**Accepts / rejects.** Rejects nothing in the corpus (§2). Newly **unwritable**: a scope-long freeze of two owned bindings selected by condition that stay readable by name — 0137 §5.1's case (b) in conditional form, with the same substitutes (adoption, or a `var`/`const` view frozen while live).

**Cost.** One paragraph rewritten, six one-clause edits (one in Guide 01), one §7.2 pair, four `DECLINED.md` rows, one rule-corpus case for the tracker; nothing new to teach — the v0.323 rule now closes over the expression forms the producers table already treats as one join.

### 3.3 Option C — reject every arm-meet that contains an unmarked named owned arm

**Rule text.** The ▲ paragraph with "every arm" replaced by "any arm". **Against:** `using var v := (if c then a else h.Borrowed());` has one reading only — `v` views, because the call arm cannot be adopted and marking `a` would make the meet mixed (K7.3) — so the form is not ambiguous (over an owned field arm the adoption reading needs `move o.f` and its §28.8.1 restoration obligation, the line 0137 drew at a bare field read), and rejecting it contradicts 0137's own line for borrow-state sources: "no adoption reading exists; stays a pin" (0137 §2's table). It would also reject the pin over a meet of a named owned binding with a field read, the one place a conditional whole-scope view of an owned local has a use (hold `a` or the fallback field to scope end). **Verdict:** declined (§5.6).

### 3.4 Option D — reject the `if`/`case` meet only, leave `??` a pin because A7 rejects the arm marker there

**Rule text.** The ▲ paragraph restricted to the `if` expression and the expression-`case`. **Against:** the ambiguity is the reader's, not the marker's — `using var y := a ?? b;` over two owned locals says no more than the `if` form does — and the `??` row already names the adoption spelling (`(if a <> null then move a else move b)`), so the remedy exists; leaving `??` out keeps the tautology loophole open in the `x ?? x` spelling and makes the rule depend on which arm position happens to admit `move` today, a bound A7 records as liftable (`DECLINED.md` line 479: "awaits the null-evaluated-transfer ruling"). When that ruling lands, option D's carve-out would have to be re-decided. **Verdict:** declined (§5.6).

---

## 4. The options side by side

| | A: status quo | B: every arm (recommended) | C: any arm | D: `if`/`case` only |
|---|---|---|---|---|
| Closes the tautology loophole (`(if true then x else x)`, `x ?? x`) | no | yes | yes | `if`/`case` only |
| Rejects a form with no marker-only adoption reading | — | no | **yes** (`(if c then a else h.Borrowed())`) | no |
| Consistent with 0137's borrow-state exemption | — | yes | no | yes |
| Depends on A7's liftable bound | — | no | no | **yes** |
| Corpus sites rejected | 0 | 0 | 0 (none written) | 0 |
| Kernel | — | derivation | derivation | derivation |
| Rationale §5.27 | concession stays open | concession closed | closed | half-closed |

---

## 5. Decision

### 5.1 Option B, decided under the delegation

**Decided: B.** The criterion the owner set for 0137 — the spelling must say whether `y` adopts or views — fails at the arm-meet of named owned bindings in exactly the way it failed at the bare binding, and the v0.323 rule can be re-entered through a grouping that adds nothing. B closes both at zero migration cost (§2) with remedies the spec already names, and it keeps the v0.323 line for borrow-state and call/field arms, so the reader's rule after v0.327 is one sentence: *a producer adopts; a borrow pins; a named owned binding feeding a `using var`, directly or as every arm of a meet, is marked `move`.* The 0137 asymmetry stands and is deliberate: `var y := (if c then a else b);` remains a silent borrow merge (§28.3), because `var` announces nothing; `using var` announces ownership (A5), so it is the one place where a silent borrow contradicts the declaration (0137 §5.1, critique N1).

### 5.2 Kernel classification (spec-change-process §9)

**Derivation, with no kernel row changed.** The rejection is an admissibility restriction at a K3.4 position (the marked arm transfer, §28.3's `if`/`case` row, lifted at v0.209 by proposal 0056 as a K3.4/K7.4 derivation), grounded in A5's declared intent as 0137 §5.2 grounds the bare-binding rule. K7's meet is applied unchanged: the rejected form's meet is borrowed ⊓ borrowed, and the rule rejects the *binding form* over it, not the meet. K3.13 continues to hold vacuously — the rejected form consumes nothing. A7 is untouched: the `??` remedy is the rewrite the producers table names, and if A7's `??` half is ever lifted the marked `move a ?? move b` becomes a second remedy with no change to this rule. No axiom is added or extended.

### 5.3 Body-locality (proposal 0105 §1)

**Local.** The rule reads each arm's syntactic shape, the declared type and the ownership state of the binding it names (facts of the body being checked; a bare type parameter decided under §28.1 clause 3 before substitution, as in 0137), and the marker's presence. No cross-body fact is consumed; a generic body's acceptance never depends on its instantiation set.

### 5.4 Implementation sketch (`b27728a`)

1. **`src/ownership/Ownership.cpp:6782–6787`**, the `ConditionalExpr`/`CaseExpr` branch: before setting `usingVarSourceIsBorrow`, walk the arms (through `PostfixBangExpr`, a parenthesized read — the parser keeps no `ParenExpr` node — and nested `ConditionalExpr`/`CaseExpr`/`??`, skipping `null`-literal and `throw` arms); if every surviving arm is an `IdentifierExpr` that 0137's bare-identifier predicate (§5.4 item 1 of that proposal: a tracked Owned local, an `owned` parameter, an owning-enumerable loop variable, an owning-match binding; class- or interface-typed, bare `T` included; not a captured owned binding, not a field), report the §3.2 diagnostic at `ld`'s span and still set the flag for recovery. Reuse the same predicate the 0137 tracker introduces, so the two rules cannot drift.
2. **A `BinaryExpr` `??` branch beside it** with the same walk, and — independently of this proposal — a pin branch for a borrow-arm `??` so that `using var y := p ?? q;` over borrow-state arms sets the flag (§1.2's hypothesis; the tracker probes it first).
3. **Codegen:** unchanged (`LlvmBackend_Setup.cpp:20284` reads the flag as today).
4. **Regression tests.** A rejection test per form (`if`, `case`, `??`, `x ?? throw …`, nested, `_`, `await using var`, `x!` and `(x)` arms, a `using var`-source arm with the `move` remedy dropped); acceptance tests for the surviving pins (a borrow-state meet — `arm_meet_ownership_1661_test.cpp:112` already is one — a meet with a call or field arm) and for the marked-arm adoptions (already pinned by `marked_arm_transfer_2080`). Each cites §28.10 and v0.327.

### 5.5 Tests that flip, and the corpus

- **`tests/`:** none (§2).
- **Rule corpus (`tests/conformance/rules/`, process §5 item 3c):** no case states the retired pin; none is edited. The ruling opens coverage the corpus lacks, which is recorded here rather than written under landing pressure, and for the same reason v0.323 withheld `s28_3_0022`'s rewrite (`g0137.md`, *Corpus banking notes*): a new `reject` case the compiler still admits would read as `fail`. The implementing tracker lands one `reject` case over two owned `var` locals, so that `move` is a legal remedy — `var a := new Widget(); var b := new Widget(); using var held := (if c then a else b); held.Ping();`, `rule:` the ▲ paragraph's first two sentences, `cites: 28.10`, `diagnostic: using var` — in the commit that implements the rejection. `s7_7_0003` (mixed `??`, `reject`) and `s28_10_0003` (owned arms, `run`) are unaffected and were re-read.
- **Doc-example corpus:** `guide1_0018`, `codegen_0050` and the spec's own example write owned arms; unaffected.

### 5.6 `DECLINED.md` rows (indexed at landing, as this proposal is accepted)

| Topic | Verdict class | Authoritative record | Reopening bar |
|---|---|---|---|
| `using var` over an arm-meet of named owned bindings — leaving the unmarked `if`/`case`/`??` meet a silent borrow-pin (this proposal's option A; #5591 interpretation (ii)) | `declined-at-proposal` | [proposal 0139 §3.1, §5.1](0139-mark-the-arm-meet.md) (graduated v0.327); spec §28.10 (the arm-meet paragraph, v0.327) | 0137's bar: a measured need for a scope-long freeze of a conditionally selected named owned source that stays readable by name and that neither adoption nor a `var`/`const` view expresses — a real program, not a test of the mechanism. |
| `using var` over an arm-meet — rejecting every meet that contains an unmarked named owned arm, a call, field, indexer or borrow-state arm beside it included (option C) | `declined-at-proposal` | [proposal 0139 §3.3](0139-mark-the-arm-meet.md) (graduated v0.327) | A meet with such an arm has no adoption reading by the arm marker alone (marking its named owned arms makes it mixed, K7.3; an owned field arm adopts only under `move o.f`'s restoration obligation), the line 0137 keeps for borrow-state sources and bare field reads. Reopening bar: a measured reader confusion at the mixed-borrow shape — none recorded. |
| `using var` over an arm-meet — rejecting the `if`/`case` meet only and leaving `??` a pin because axiom A7 rejects the arm marker there (option D) | `declined-at-proposal` | [proposal 0139 §3.4](0139-mark-the-arm-meet.md) (graduated v0.327) | The ambiguity is the reader's, not the marker's; the `??` row already names the adoption rewrite; the carve-out would hang on A7's liftable `??` half. Reopening bar: —(none stated). |
| Unmarked `if`/`case`/`??` arm-meet of class- or interface-typed named **owned** bindings alone at a `using var` as a borrow-pin (v0.184–v0.326) | `retired` | spec §28.10 (the arm-meet paragraph, v0.327); rationale §5.27 (the concession, v0.327); [proposal 0139 §3.2, §5.1](0139-mark-the-arm-meet.md); changelog v0.327 | Retired v0.327 (delegated ruling 2026-10-08): the spelling said neither adopt nor view, and it re-admitted the v0.323-retired form through a tautological grouping. Zero corpus sites. Reopened only with the first row's evidence; the pin over a meet with a call, field, indexer or borrow-state arm is not retired. |

---

## 6. Open questions and side findings

1. **The `??` pin flag (§1.2).** HYPOTHESIS, to be probed by the implementing tracker before anything is asserted: `using var y := p ?? q;` over two plain parameters may dispose a borrowed object at scope end. It is outside this proposal's verdict change and is listed so the tracker's first commit is the probe.
2. **A7's `??` half.** When the null-evaluated-transfer ruling lands and `move a ?? move b` is admitted, the diagnostic gains it as a remedy; no rule text changes.
3. **The binding atom.** `using var y := (var t := a; t);` peels to the body's classification (§28.3's atom row) — a borrow of the atom's own binding, which may not escape the atom (K8.4), so the form is rejected already. Not a gap.
4. **#5592**, the asynchronous pin, is ruled beside this proposal in the same train (an `await using var` over a borrow right-hand side is admitted and awaits nothing at scope exit, as is one over an owned right-hand side with no `DisposeAsync`; the spelling-keyed static rules of §30.1 and §30.7 apply to both as written; §28.12 states it). It is independent: this proposal covers `await using var` over a meet of named owned bindings exactly as 0137 covers `await using var y := x;`.

---

## 7. Self-critique (adversarial pass)

**C1. "This is a design change contradicting the v0.323 text; process §2's third rule says such a change is reopened only with new evidence."** v0.323 recorded no *decision* on the arm-meet — its lane filed the shape as a question in the same landing, and the critic rewrote §5.27's concession so as not to prejudge it. There is no recorded decline to reopen; the delegation of 2026-10-08 authorizes the ruling, and this is the proposal the process requires for it.

**C2. "Zero affected sites means the rule is unmotivated."** The motivation is consistency and the loophole, not migration: 0137 was accepted on the ground that a spelling must say adopt-or-view, and §0 shows a one-token way to write the rejected form under the v0.325 text. A rule with zero flips is the cheapest moment to close that.

**C3. "The nested-arm recursion makes the rule hard to state and to implement."** The recursion is K7's own: a nested meet is a join of joins, and §28.3's `??` row already defines `a ?? b ?? c` arm by arm. The implementation walk (§5.4) mirrors `producerState`'s existing recursion over `ConditionalExpr`/`CaseExpr`/`??`.

**C4. "`x ?? throw new E()` over a nullable owned local is an idiom this rejects with an awkward remedy."** It is the same ambiguity (does `y` adopt `x`?), and the remedies are real: `(if x <> null then move x else throw new E())`, which keeps the exception (the `throw` arm absorbs, K7.5; the marked arm is K3.4's); `move x!` where a `NullReferenceException` on `null` is acceptable (§17.8: the `!` is transparent to the marker); or the statement form `if x = null then throw new E(); end; using var y := move x;`. `x ?? null` is `using var y := move x;`. No corpus site writes the idiom at a `using var`.

**C5. "A meet with a `using var`-source arm and an owned-`var` arm has one `move`-admissible arm and one that is not; what is the remedy?"** The diagnostic drops `move` for the A5 arm and the view remedy covers the whole form — `const y := (if c then u else v);`. Adoption of only the `var` arm is the mixed meet (K7.3) and is not offered. Stated in §3.2's diagnostic.

**C6. "Does the rule key on the declared or the narrowed type of an arm?"** On the declared type, nullability stripped, as 0137 does; an arm's flow-narrowing (`x ?? …` over `W?`) does not change the binding's class-typedness.

**C7. "Guide 01 is edited in another train."** The v0.326 guide re-trace lands first and leaves the §9 parenthetical's text intact, so the one-clause edit (§3.2) applies after it. The guide's text is correct about `var` and would be wrong only about `using var` without it.

**C8. "The retired pin's K2.2 union of sources — is any rule lost for the surviving pins?"** No. The union-of-sources pin over a meet with a call, field, indexer or borrow-state arm is unchanged; only the all-named-owned meet is rejected.

**C9. "Setting the absorbing arms aside can leave no arm at all — `(if c then null else throw new E())`."** The rule requires at least one surviving arm ("and at least one arm is"), so a meet of absorbing arms alone is not covered; it has no binding to adopt or view and is whatever §28.10 makes of a `null`/bottom right-hand side.

**C10. "A field arm can be adopted — `(if c then move a else move o.f)` is an owned meet."** It is, under §28.8.1's restoration obligation; the rule does not say a field arm has no adoption reading, only that marking the meet's named owned arms *alone* yields none, and it leaves the unmarked field-arm meet a pin exactly as 0137 left the bare field read (0137 §6 item 4).
