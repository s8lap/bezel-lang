# Proposal 0094 — Enumeration dedup: the announcement table, §30.5's admitted positions, and the Appendix C cap

| | |
|---|---|
| **Status** | graduated-v0.261 — **superseded — graduated v0.261** (ruled under the owner's 2026-08-18 delegation; charter item B3, the review program's last train; **stage 2 executed as ruled** — the §28.3 announcement table re-derived at the train's own HEAD against the post-v0.258 unified rule, landing as 8 rows × 4 callee-kind columns in place of the §1.2 pre-0096 snapshot's 12, the fence block dead with 0096's subsumption; the §30.5 demotion and the Appendix C cap landed per §11, the census re-measured at 44 over-cap rows and the one C-only fragment — the A8-tombstone pointer — moved into §28.6; zero verdict moves; the landed record is the v0.261 changelog entry) |
| **Tracking issue** | coordinator files |
| **Target module(s)** | **spec** — §28.3 (*Producers* rows for calls and function-value invocations; *Factory methods and sourceless returns*; *Task-family calls produce owned frames*; *Bare-type-parameter returns*; *Concrete-slot function bodies*; *Iterator and iterable returns…*; *Iterable-interface returns at instance and extension callees*; *Borrow lifetime rules* bullet 2; *Returning a deferred-activation frame*) *[v0.261: the two iterable paragraph names here are the v0.255 snapshot's — v0.258 replaced *Task-family calls produce owned frames* with *Calls mint for the mint-announcing families* and deleted *Iterable-interface returns at instance and extension callees*]*, §30.5 (*Consumer-side rules* — the admitted-position bullets), **Appendix C** (all fifteen subsections; 42 rows over cap ⧗), plus the cited-section receiving edits for any C-only content moved out (§28.6 confirmed; others per the §3.3 sweep); **rationale** — §12.10 (the announcement doctrine gains the table pointer), §5.7; **stdlib** — §5 intro, §6.8 (two pointer sentences); **guides** — 04 §2 and 05 §3 (the "the return type is the announcement" teaching sentences gain the table pointer; no rule text). Doc-only: **no semantics move.** |
| **Written against** | spec/stdlib/rationale/guides **v0.255** (git `5fa5a95b`, snapshot 2026-08-18). Every count, word-count, and table cell in this document is measured or verified against that snapshot unless marked otherwise. |
| **Currency** | Re-verified against **v0.256** (git `4130d2de`, the review program's defect basket, plus its gate-fix commit `e8f5387a`; merged HEAD `8f41e99d`). Counts marked **⧗** were re-measured at that merged revision and supersede the snapshot figures where they differ; the graduation train re-measures at its own HEAD regardless. |
| **Origin** | Review-program charter item **B3** (consolidation basket), ruled under the owner's 2026-08-18 delegation. The trigger: §28.3 states one classification rule — *what does a declared return type announce, at which callee kind?* — as eight ⧗ mutually-excluding prose paragraphs totalling 4,556 words ⧗ (six paragraphs / 3,718 words at the v0.255 snapshot; v0.256's #3246 fence added two), each of which must name every other one to stay correct; §30.5 states a closure and then nine bullets, two of which restate the closure verbatim; and Appendix C, whose contract is "a checklist", carries 42 rows over one sentence ⧗, one of them 315 words. |

*Reference convention: unprefixed §-references are language-spec references; `K`/`A` tags are §28.3's kernel and axiom-register ids; rationale, stdlib, and guide references are marked. Quotations are verbatim from the cited revision; any bold or italic emphasis this document adds to a quotation is marked "(emphasis added)" — unmarked emphasis inside a quotation is the source's own. This proposal uses proposal 0093's vocabulary — **type-announced** for the K2.4/K6.2 sense — and depends on it landing first (§8).*

---

## 0. What this proposal is, and is not

It is a **restatement program**: three enumerations are deduplicated into the rules they instantiate. No admission is widened, no rejection is added, no classification changes at any callee kind or any substitution. **This is the largest editorial surface in the book** — §28 is 38,933 words at v0.256 ⧗ (37,844 at v0.255) and §28.3 alone is 18,332 ⧗ (17,535) — and §7 makes the mechanical-validation protocol a landing condition rather than a courtesy.

## 1. The announcement table (§28.3)

### 1.1 What is duplicated

> *[Corrected at the v0.261 landing round (the 0100-§10 in-round precedent): this inventory is the v0.256 snapshot's. v0.258 (proposal 0096) deleted the two fence paragraphs and the conservatism-reach paragraph and replaced or deleted the two named iterable paragraphs, so at the train's HEAD the cluster was five paragraphs; the landed reduction is the v0.261 changelog entry's record.]*

Eight paragraphs in §28.3 ⧗ answer one question — *what classification does a call's declared return type announce, and at which callee kind?* — by excluding one another (word counts re-measured at v0.256, links and formatting stripped; the v0.255 snapshot had six paragraphs, 3,718 words — v0.256's #3246 fence added the last two and retired the recorded-residual parenthetical from *Bare-type-parameter returns*):

| Paragraph | words ⧗ |
|---|---|
| *Factory methods and sourceless returns* | 438 |
| *Task-family calls produce owned frames* | 543 |
| *Bare-type-parameter returns* (caller side, callee side, the calls-mint exclusion; the recorded residual retired at v0.256) | 1,672 |
| *Iterable-interface substitutions are excluded identically* (v0.256, #3246 — the mirrored exclusion) | 744 |
| *Conservatism reach* (v0.256 gate round — the fence's stdlib reach, stated) | 264 |
| *Concrete-slot function bodies* | 277 |
| *Iterator and iterable returns are outside the static-return error above* | 174 |
| *Iterable-interface returns at instance and extension callees* | 444 |
| **total** | **4,556** |

Each one carries an exclusion list naming the others, because prose has no other way to be exhaustive. The *Producers* table's "Method call returning `T` (no `owned`)" row alone carries **two** parenthesised exceptions (the `Task`-family carve and the bare-type-parameter carve) inside a single cell, and the function-value-invocation row's *type* column runs over a hundred words of exclusions ⧗ (96 at v0.255; the v0.256 fence parenthetical grew it) before reaching its classification. The cost is not length: it is that a new return kind must be added to eight exclusion lists ⧗, and an omission in any one of them silently changes a classification — v0.256 is itself the demonstration: closing one residual took **two new paragraphs** in this cluster plus edits to three others, exactly the accretion pattern the table ends.

### 1.2 The table

> **Pre-0096 snapshot.** Rows 3, 5, 6, 8, and 9 transcribe the v0.256 rules that proposal 0096 — graduating **first**, per the post-gate ordering ruling (§8) — replaces: under the unified calls-mint rule, rows 5 and 6's instance/extension cells become owned frames, row 3's family carve generalizes to the mint-announcing families, row 8 joins that family, and row 9's exclusion reads the unified family list. The table as drafted is the **pre-0096 snapshot**, kept as the derivation record and **superseded on 0096's landing**; the stage-2 re-derivation (§8) re-derives every cell against the unified rule in this proposal's own graduation train.
>
> *[Landed at v0.261: the stage-2 re-derivation produced an **8-row × 4-callee-kind-column** table over the unified rule — the snapshot's rows 3–8 collapse into two mint-announcing rows (unmarked; explicitly `owned`), the fence block below is dead (subsumed at v0.258), and the "unclassified" cells are gone (the function-value mint closed at v0.258). Spec §28.3's landed table is the carrier; this section stands as the pre-0096 derivation record.]*

Two rules are stated once, above the table, and apply to every cell:

> **Keying.** Every row is read from the callee's **declared** return type, **before** generic substitution, with nullability stripped (`Task?`, `T?` qualify as their unstripped kinds). No type-keyed classification applies to a call whose declared return type is claimed by a row above it.
>
> **Inheritance.** A member implementing or overriding a generic declaration classifies at **the declaration it implements**, at every call site, direct or dispatched; a bare-type-parameter row's potential source set is read from the generic declaration's signature, its source parameters identified positionally through the substitution. Interface dispatch is therefore not a column: it is the declaring signature's row.
>
> **Not calls.** Property, field, indexer, and slot **reads** are governed by the *Producers* table's storage rows, never by this table; a read of `Task`-family-typed storage lends a borrow of the storage.

| # | Declared return kind | instance callee | static callee | extension callee | function-value invocation | Callee-side admission | Cite |
|---|---|---|---|---|---|---|---|
| 1 | **`owned T`** (concrete class or interface; a bare-type-parameter `owned T` is this row, not row 9) | **Owned** | **Owned** — an admissible static signature | **Owned** | **Owned** | Every return path supplies a fresh or owned value; a borrow is rejected. At value and runtime-managed substitutions of a bare-`T` `owned` return the marker is **erased** by the substitution and the result is a plain value. | §28.8; *Producers*; §28.1 |
| 2 | **plain concrete class or interface type** | **Borrow**, source = the receiver | **rejected at the declaration** — nothing names a responsible party | **Borrow**, source = the receiver parameter | **Borrow**, carrying the **union of the function value itself and every plain (non-`owned`) reference-typed argument** | *Instance/extension:* return only a receiver-(parameter-)sourced borrow; a fresh owned value is rejected. *Static:* no admissible body — the signature is rejected. *Static-storage-sourced borrows* pass only through a **static property getter**, never a static method. *Lambda into such a slot:* `null` (nullable slot), a capture-sourced borrow, a plain-parameter-sourced borrow, or static storage; a fresh owned value rejected. | *Producers*; *Factory methods*; *Borrow lifetime rules*; *Concrete-slot function bodies*; §26.7; §28.6 |
| 3 | **`Task`-family** (`Task`, `Task<T>`, `OwningTask<T>`), unmarked | **Owned frame** — calls mint | **Owned frame**; an admissible static signature | **Owned frame** | **Owned frame** | The unmarked type is itself container-ownership-transferring: every return path of a non-`async` body supplies an owned task (a forwarded task-returning call, a completed-task factory result, an owned task local or `owned` parameter, a `move`d owned task field); `null` on any path under a nullable declared type; an `async` body satisfies this by construction. Returning a **stored task borrow** (bare field or property read, plain parameter, captured binding) is rejected. The invocation of a **call-bounded** plain function-typed parameter is excluded. | *Task-family calls produce owned frames*; K6.3; axiom A2; §28.14 |
| 4 | **`Task`-family, explicitly `owned`** | identical to row 3 | identical to row 3 | identical to row 3 | identical to row 3 | identical to row 3 — the marker makes the same contract explicit (`Domain.Spawn(…): owned Task`, stdlib §6.8) | *Task-family calls produce owned frames* |
| 5 | **non-owning iterable/enumerator interface** (`IEnumerable<T>`, `IEnumerator<T>`, `IAsyncEnumerable<T>`, `IAsyncEnumerator<T>`), unmarked | **Borrow** — the stored-collection view; source = the receiver | **caller-owned frame** — the iterator convention's carve | **Borrow**; source = the receiver parameter | **deliberately unclassified** (a recorded open question; see the residual below) | *Instance/extension:* return only receiver-(parameter-)sourced borrows, or `null` under a nullable declared type — a bare collection-field read upcast to the interface is the canonical form; a **freshly-produced frame** through an unmarked instance or extension return is a **compile error**, the diagnostic naming `owned`. *Static:* an iterator body's state machine, or a composed frame, whose every carried borrow is parameter- or receiver-sourced. | axiom A2; *Iterator and iterable returns*; *Iterable-interface returns at instance and extension callees*; *Returning a deferred-activation frame*; K6.2 |
| 6 | **owning head** (`IOwningEnumerable<T>`, `IOwningEnumerator<T>`, `IOwningAsyncEnumerable<T>`, `IOwningAsyncEnumerator<T>`), unmarked | **rejected at the declaration** — no unmarked instance spelling exists | **caller-owned frame** — the iterator convention's carve | **rejected at the declaration** | **unclassified**, as row 5; §27.6's formation fence bars bridging a *static* owning-head factory into another slot | *Static:* as row 5's static cell. *Instance/extension:* no admissible body — the declaration is rejected; expose the stored view at the concrete collection type or a non-owning interface, or declare the return `owned`. | *Iterable-interface returns at instance and extension callees*; §28.6's read-or-iterate-but-not-drain contract; §29.2's consuming desugaring |
| 7 | **iterable/enumerator interface marked `owned`** (either head) | **Owned frame** | **Owned frame** | **Owned frame** | **Owned** (row 1's slot) | A fresh state machine, a composed deferred-operator result, or a fresh or owned enumerable; carried borrows still parameter- or receiver-sourced (K6.2). `GetEnumerator(): owned IEnumerator<T>` is the protocol's own instance of this row. | *Iterable-interface returns…*; §29.1; §29.2 |
| 8 | **lazy query** (a stdlib §5 deferred-operator result; a compiler-known category, not a spellable type name) | frame classification is type-announced; the value is **affine**, discharged at its single consumption point | as instance | as instance — the stdlib operators are extension callees | **unclassified**, as row 5 | *Returning a deferred-activation frame:* carried borrows parameter- or receiver-sourced only, followed transitively through intermediate frames and the plain function-typed arguments' capture-set contributions. | K6.1; K6.2; §29.11; *Deferred-activation callees* |
| 9 | **bare type parameter `T`, non-empty potential source set** (the receiver — for a function-value invocation, the function value — together with every plain parameter whose declared type mentions `T`) | at class/interface substitutions a **Borrow** carrying the union of the contributing sources' liveness; at value/runtime-managed substitutions, no borrow and no constraint | as instance; an admissible static signature | as instance (the receiver is the receiver parameter) | as instance (the function value is the receiver) | Every return path supplies `null` or a borrow whose sources outlive the call: the receiver (directly, or through its fields and indexers, with §28.3's iteration-variable clause), a plain source parameter (directly, through a borrow reached from it, or by invoking a function-typed source parameter), or static storage. A fresh owned value, or a borrow of an owned local, a `using var`, an `owned` parameter, or a temporary, is rejected. Enforced **per instantiation category** (§3.6 rule 2); inherited by implementations and overrides. **Calls-mint exclusion:** a member is inadmissible at an instantiation substituting a `Task`-family type for a parameter that appears as the **return slot of a function type** anywhere in a parameter's declared type — and, v0.256 (#3246), **identically at a substitution of one of the eight §29.3/§30.7 iterable and enumerator interface heads** (a lazy query's static type is `IEnumerable<T>`, already a head, so it needs no separate mention; stdlib §5's derived interfaces `IOrderedEnumerable<T>`/`IGrouping<TKey, T>` are outside the excluded set). | *Bare-type-parameter returns*; *Iterable-interface substitutions are excluded identically*; K2.4; §3.6 rules 2/4; §27.1.1; §28.1 |
| 10 | **bare type parameter `T`, empty potential source set** | **cannot arise** — an instance callee always has a receiver | **rejected at the declaration** — spell `owned T` (admissible over every domain under §28.1's per-instantiation erasure) | **cannot arise** — the receiver parameter is a source | **cannot arise** — the function value is a source | no admissible body: the signature is rejected | *Bare-type-parameter returns*; *Factory methods* |
| 11 | **value type, `string`, or other runtime-managed type** | **not applicable** | **not applicable** | **not applicable** | **not applicable** | outside this table; no obligation arises | §28.9 |
| 12 | **move-only value type** (an owning-classified choice, including a case construction; an owning inline array) | **Owned** move-only value | **Owned** | **Owned** | **Owned** | The caller discharges by binding into a local (auto-disposed at scope end), into an owning-inline-array slot, into a class field of move-only type (only for a value carrying no borrows), or — for a choice — by a consuming match. A `Task<T>`/`OwningTask<T>` over such a T is rejected at the declaration. | *Producers*; §15.3; §35.4; §35.5; §30.5 |

**The fence, stated beneath the table** (v0.256, #3246 ⧗ — a live fence, not a duplication; the v0.255 draft of this block kept the recorded-residual parenthetical "verbatim", text v0.256 has since **deleted**): a function value over a **static iterable factory** could launder the iterator convention's caller-owned frame into a borrow classification — through a bare-parameter substitution, a retyped formed value, or direct formation bridging. v0.256 fenced all three routes: row 9's iterable-family exclusion (the bare-parameter route), §27.1.1's announcement-bearing family boundary (the retyping route), and §27.6's formation fence (the direct route) — *"no route remains by which a static iterable factory's mint reaches a slot classified as a lend … no recorded leak remains in this rule."* Rows 5, 6, and 8's "unclassified" cells are the classification question the fence deliberately leaves open (admissibility, never classification). The fence prose surviving beneath the table is #3246's minimal closure, kept with its recorded conservatism (the stdlib-reach statement included); it is **superseded wholesale by proposal 0096** at its graduation, which unifies the classification the fence only guards.

### 1.3 Cell-by-cell verification

> *[v0.261: the landed verification ran against HEAD text; the quotes below cite v0.256 paragraphs v0.258 deleted or replaced — kept as the snapshot's record.]*

Every cell above was read out of the snapshot, not inferred. The load-bearing checks:

| Cell | Landed sentence |
|---|---|
| 2/static = rejected | *Factory methods*: "A `static` method or `operator` declaration … whose declared return type, nullability stripped, is a class or interface type is a **compile error** unless one of the following holds" |
| 2/fn-value = borrow carrying the union | *Producers*, function-value row: "Borrowed — carrying the **union of the function value itself and every plain (non-`owned`) reference-typed argument**" |
| 3, all four columns | *Task-family calls produce owned frames*: "produces an **owned** task at the call site, whatever the callee kind: instance, static, extension, interface-dispatched, or an invocation through a function value" |
| 4 = 3 | same paragraph: "an explicit `owned` on a `Task`-family return remains admissible with identical semantics" |
| 5/instance = borrow; 5/static = frame | *Iterable-interface returns at instance and extension callees*: "The iterator convention above is the *static* face … **unmarked**, the call result is a **borrow** (source: the receiver …)" |
| 5/fn-value unclassified | *Producers*, function-value row (v0.256 wording ⧗): "frame ownership at an iterable-typed function-value invocation is an open question, deliberately unclassified here; a reference over a **static iterable factory** never reaches a slot this row claims — all three routes are fenced, v0.256, #3246" |
| 6/instance, 6/extension = rejected | same paragraph: "an unmarked instance or extension return typed at an owning head is **rejected at the declaration**" |
| 6/static = frame | *Iterator and iterable returns*: the list names all four `IOwning*` forms |
| 9 caller/callee sides | *Bare-type-parameter returns*, **Caller side** and **Callee side** paragraphs |
| 10/instance, extension, fn-value cannot arise | *Bare-type-parameter returns*: the potential source set "is the call's receiver (for an extension method, the receiver; for a function-value invocation, the function value), if any, together with every **plain** … parameter whose declared type mentions the returned type parameter" — every one of those callee kinds contributes a receiver, so the set is never empty |
| 12 = owned move-only | *Producers*: "Function call producing a move-only value type … \| Owned." |

Row 10's three "cannot arise" cells are the table's clearest gain: today that fact is derivable only by composing the source-set definition with the callee-kind list across two paragraphs, and no sentence states it.

### 1.4 What the eight paragraphs become ⧗

The table **subsumes the exclusion prose**, not the reasoning. Each paragraph keeps its recorded *why* and loses its *what*:

- *Factory methods and sourceless returns* → keeps the sourcelessness argument (a static call has no structural borrow source; a static method may not re-expose static storage; the two-owners defect) and the "documentation phrases carry no weight" sentence; its four-bullet admissible list **is rows 1, 3, 5–7, 9's static column** and dies.
- *Task-family calls produce owned frames* → keeps the mint reasoning (a task-returning callee mints a fresh frame or forwards one; `async` is implementation-side; §30's disposal story requires the caller to hold the container) and the property-face resolution; its callee-kind enumeration dies into row 3.
- *Bare-type-parameter returns* → keeps the caller/callee reasoning, the inheritance clause, the calls-mint exclusion's soundness argument, and the recorded residual; its "whatever the callee kind" enumeration and its two "not this row's case" exclusions die into rows 9 and 10.
- *Concrete-slot function bodies* → becomes row 2's callee-admission cell plus its one-sentence reason.
- *Iterator and iterable returns…* → becomes rows 5–7's static column plus the "redundant noise over a type-announced transfer" sentence.
- *Iterable-interface returns at instance and extension callees* → keeps the stored-collection-view argument and the owning-head double-free argument (both load-bearing, both cited from axiom A2); its spelling matrix dies into rows 5–7.
- *Iterable-interface substitutions are excluded identically* (v0.256) ⧗ → **folds into row 9's iterable-exclusion arm plus the fence block beneath the table** (§1.2): the exclusion's *what* is one cell; its soundness argument (the callee-kind-erasure reason, the recorded conservatism) survives as the fence prose. Pre-0096 content throughout — superseded when 0096 unifies the classification.
- *Conservatism reach* (v0.256 gate round) ⧗ → **folds into the fence prose** as its stdlib-reach sentence (the six affected stdlib §5 members and the statement-form remedy); the worked `Select` example survives as one instance, not a paragraph. Pre-0096 content — under unification the fence retires and this reach statement with it.

Estimated surviving prose: **~1,900 of 4,556 words** ⧗, plus a 12-row table (the two v0.256 fence paragraphs contribute the fence block's share, all of it pre-0096). The train reports the actual figure; the target is not a word count but that **no paragraph names another paragraph's exclusions.**

## 2. §30.5's nine bullets become worked instances

### 2.1 The spec already says the list is not closed

> §30.5: "The principle resolves into the following examples of admitted task-expression positions — each an instance of one of [K2]'s source kinds, the first eight named locations of the several kinds, the ninth the frame case; **the list is not exhaustive but illustrates the rule, and its closure is [K2]'s named-location taxonomy**: a task-typed storage read through any field path rooted at a live object, and a borrowed alias of any admitted operand, are admitted with the source set the path derives:" *(emphasis added)*

The nine bullets that follow run 640 words. Two of them restate that very sentence.

### 2.2 Bullet-by-bullet disposition

| # | Bullet | Content beyond the closure | Disposition |
|---|---|---|---|
| 1 | `using var` / `await using var`-bound local | none — a K2 named location | **keep** as a worked instance (the canonical form) |
| 2 | `owned` parameter | none | **keep** as a worked instance |
| 3 | plain owned local | *the freeze/announced-kill cross-reference*: consuming or reassigning it while the borrow lives is rejected by the consume-site freeze; an explicit `Dispose()` remains a site-announced kill with the checked-mode backstop | **demote the admission; preserve the note** beneath the instances |
| 4 | plain (non-`owned`) local or parameter | *the answer to "why is a borrow admitted at all?"* — the caller's own §30.1 settlement and §30.4 fan-out obligations keep the task alive, and "ownership of the location was never the admission's real criterion" | **demote the admission; move the sentence into the principle paragraph**, where it belongs |
| 5 | task-typed field path rooted at a live object | **one clause the closure does not carry** ⧗: "a plain task-typed **borrow** field, whose held borrow's own chain joins the set" — the K2.2 union of a borrow field's held chain into the derived source set; the closure sentence says only "the source set the path derives" | **the chain-join clause moves into the closure sentence** ("… are admitted with the source set the path derives, a borrow field's held chain joining it"), **then the bullet dies** |
| 6 | admitted static field | *the §22.12 rule-4 consequence*: the class-typed result reached through such a chain is process-shared across every domain that awaits the cached task, so the confine-or-synchronize obligation applies | **demote the admission; preserve the note** |
| 7 | indexer access on an owning collection of tasks | none | **keep** as a worked instance |
| 8 | iteration variable over an array of tasks | none | **keep** as a worked instance |
| 9 | producer call directly inside `return` (the chained slot) | **none** — it is the frame case, already stated in the producer-side kind-2 paragraph *and* in the principle paragraph immediately above ("A producer call whose result is directly consumed by a Pattern A return in the enclosing async method is the **chained slot** (kind 2 above; K6) — a task *move*, not a borrow escape") | **dies** |

**Result:** 9 admission bullets → the K2 closure as the rule (absorbing bullet 4's criterion sentence and bullet 5's chain-join clause), **4 worked instances** explicitly labelled non-exhaustive, and **3 preserved notes** that were never admissions in the first place. The scoped await-bind paragraph, the `Process(await Producer())` worked example, the may-flow-to list, and the may-not-flow-to list are untouched.

## 3. Appendix C: one sentence and a citation

### 3.1 The cap, and the census

The appendix's own contract is "a checklist for implementers and a reference for programmers wanting to know which classes of error are caught at compile time." The cap makes that literal: **one sentence of invariant, plus its citations and an optional parenthesised kernel-tag list.** Content that a row states and its cited section does not must **move into the cited section** before the row is capped — a checklist is not a normative carrier.

Measured over Appendix C at v0.256 ⧗ (rows = top-level bullets with their continuation lines; word counts strip link targets and formatting): **205 rows**, median **30 words**, **42 rows over 60 words**, 14 over 100, 6 over 150. (The v0.255 snapshot: 203 rows / 39 over cap. v0.256 added the C.3 function-slot exclusion row and the C.3 construction-unwind row, and grew C.7's destructuring named-slot row past the cap.)

### 3.2 The full over-cap census, with disposition

Disposition codes: **C** compress to one sentence + citation; **C+M** compress *after* moving C-only content into the cited section; **C\*** compress with care — the invariant is genuinely multi-clause and the cap is met by one sentence plus a parenthesised tag list. (Rows marked ⧗ are the three that joined the over-cap set at v0.256; the other rows' word figures are the v0.255 snapshot's — the ±2-word drift under the re-measure method is immaterial to disposition.)

| § | words | Row (opening) | Disposition |
|---|---|---|---|
| C.1 | 160 | The bare token `_` is never a declarable or readable name… | **C\*** (the rejected-positions list is §17.13's; cite it) |
| C.1 | 107 | When more than one overload is applicable to a call only… | **C** (§22.6) |
| C.1 | 75 | Generic-method type arguments are inferred per §14.5… | **C** (§14.5, §22.6) |
| C.1 | 67 | A type may not declare two members that are indistinguishable… | **C** (§11.3, §22.6) |
| C.1 | 88 | A method-local `record`/`enum` declaration (§21.10)… | **C\*** (§21.10) |
| C.1 | 120 | `owned` on an ordinary parameter distinguishes overloads… | **C\*** (§22.6, §28.1, §14.2) |
| C.1 | 155 | A write that lands in the §8.6 box of a §12.7 value-narrowed carrier… | **C\*** (§8.6, §12.7, §7.4) |
| C.1 | 76 | A collection literal targeting a reference type… | **C** (§15.4, §28.3) |
| C.1 | 68 | Calling `.Equals(Object?)`… on a non-auto-hashable body record… | **C** (§9.2) |
| C.1 | 108 | A storage component marked `equality skip`… | **C** (§9.2) |
| C.3 | 62 | A choice that carries borrows (§35.4's census…)… | **C** (§35.4, §9.7 — the bullet list is §9.7's, verbatim) |
| C.3 | 88 | Owned values may not escape… | **C\*** (§28.6, K9.1) |
| C.3 | 123 | A `new` expression's result, and any owned local, is disposed exactly once… | **C\*** (§28.3, K3.14/K3.15) |
| C.3 | **241** | A `static` method or operator whose declared return type… | **C** — **the largest C.3 row; it collapses to one sentence citing this proposal's §1.2 table** |
| C.3 | 80 | An owning-collection or owning-wrapper return type… | **C** (table row 1/2 + §28.6) |
| C.3 | 100 | A call whose declared (pre-substitution…) return type is a `Task`-family type… | **C** (table row 3) |
| C.3 | 86 | Explicit `Dispose()` requires an owned receiver… | **C+M** — carries the **only verified C-only fragment in the appendix**: *"the A8 tombstone records the retired carve."* Move that pointer into §28.6's owned-receiver rule, then compress. |
| C.3 | 69 | An unbound owned temporary… | **C** (§28.3, K8.1) |
| C.3 | 75 | A binding-atom binding whose initializer is an owned production… | **C** (§17.15, K8.4/K8.5) |
| C.3 | 74 | An `out owned` parameter is a callee-local owned slot… | **C** (§22.3, §22.4) |
| C.3 | 69 | A trivial-dispose class… | **C** (§28.9) |
| C.3 | 69 | A `_ := expression;` discard statement… | **C** (§17.13, K3.9/K4.2, axiom A6) |
| C.3 | 78 | The `move x.f` expression… | **C\*** (§28.8.1, K9.1/K9.2) |
| C.3 | 83 | Owned-element collections… own their elements… | **C\*** (§28.11, axiom A9) |
| C.3 | 108 | Iterator yield ownership is determined by… the declared return type… | **C** (§29.2, K6.2) |
| C.3 ⧗ | 96 | On **construction unwind** (§22.9) the throwing class's own `destructor` body is **never** run… | **C** (§22.9 — new at v0.256, the C10/#3245 ruling; the rule is stated in full at §22.9, so the row is a plain compress) |
| C.3 ⧗ | 156 | A member whose declared type or signature contains… a function type whose return slot… is a **bare type parameter**… | **C** — **the new fence row (v0.256, #3246; 108w as landed, 156w after the gate-fix commit's "eight heads" expansion). Collapses to one sentence citing this proposal's §1.2 table row 9 (both family arms) + §27.1.1 + §27.6.** Pre-0096 content: 0096's graduation re-derives it with the table. |
| C.3 | 65 | In an owning iterator, `yield return x;` on a named owned binding… | **C** (§29.2; and proposal 0093 re-points its two announcement senses) |
| C.3 | 100 | A fresh-producer element… in a borrowing-family collection literal… | **C\*** (§15.4, §28.11) |
| C.3 | 78 | A stdlib §5 class-element terminal accessor… | **C** (§29.11) |
| C.5 | **177** | In a value-kind instance member body… | **C\*** (§9.4 — the v0.255 store-landing rule; the three producing positions stay as a parenthesised list) |
| C.6 | **314** | Awaiting a `Task<T>` for class T (Pattern A…)… | **C** — **the appendix's largest row.** It restates §30.5's nine bullets, which §2 is demoting; it collapses to one sentence citing the K2 closure. The two edits interlock and must land together. |
| C.6 | 125 | `await` on a `OwningTask<T>` (Pattern B…)… | **C\*** (§30.5, K3.5/K4.2) |
| C.6 | 62 | A blocking wait… never parks a thread inside a domain continuation… | **C\*** (§30.3, §30.12) |
| C.6 | 64 | The task returned by invoking a call-bounded callback… | **C** (§28.14) |
| C.6 | 63 | A deferred-activation callee's plain function-typed parameter… | **C** (§28.14) |
| C.7 | 83 | A marked named slot (`move N = var x`, §19.3)… | **C\*** (§19.3, §35.5) |
| C.7 ⧗ | 72 | A destructuring pattern's slot list is **positional or named, never mixed**… | **C** (§19.3 — grew past the cap at v0.256; the resolution rule is §19.3's in full) |
| C.11 | 99 | Cyclic static initialization across distinct types… | **C\*** (§22.13 — both approximations are stated there in full) |
| C.11 | 83 | A mutable (non-`readonly`, non-`const`) static field or auto-property… | **C** (§22.12) |
| C.11 | 63 | A handler argument at any call resolving to a subscribe position… | **C** (§22.12, stdlib §6.5) |
| C.15 | 75 | `result` appears only in `ensures`… | **C** (§24) |

### 3.3 How the C-only sweep was run, and what it found

A mechanical containment screen was run over all over-cap rows (39 at the snapshot; 42 at v0.256 ⧗): each row was split into sentences; each sentence's six rarest words (by whole-spec frequency computed **excluding** Appendix C) were taken as its fingerprint; a sentence with no paragraph outside the appendix matching ≥3 fingerprint words was flagged as possibly C-only.

The screen flagged **7 rows**. Hand-checking against the snapshot resolved **6 as restatement** (screen false positives — the fingerprints were rare *phrasings* of content stated in other words at §22.6, §35.4/§9.7, §28.3, §15.4, §9.4, and §22.13 respectively) and confirmed **1 genuine C-only fragment**:

- **C.3, the `Dispose()`-receiver row** — "the A8 tombstone records the retired carve" appears **nowhere else in the spec**. The A8 tombstone row itself exists in the axiom register, but the *pointer* — that the retired element-borrow carve is recorded there — is Appendix-C-exclusive. Disposition: move the pointer into §28.6's owned-receiver rule (which already states "The owned-receiver rule is exception-free"), then compress the C row.

The train **re-runs this screen at its own HEAD** (§7 item 4) rather than trusting this list: the screen is a lint, and one revision of drift can add a row.

## 4. Corpus evidence

- **Documents.** §28 = 38,933 words at v0.256 ⧗ (37,844 at v0.255; the charter's "~36,700" was measured earlier still — the drift is reported, not smoothed). §28.3 = 18,332 ⧗; the eight exclusion paragraphs = 4,556 ⧗; §30.5 = 5,446 ⧗, of which the nine bullets are 640; Appendix C = 8,730 words across 205 rows ⧗.
- **Compiler and tests (read-only; no code changes proposed).** The classification the table restates is already one dispatch in the front end; no diagnostic string quotes an exclusion paragraph's prose. Appendix C is a checklist, cited by tests only as a section reference.
- **Nothing in the stdlib's API surface changes.** Two stdlib sentences (§5 intro, §6.8) point at the announcement doctrine and gain the table pointer.

## 5. Migration and compatibility (changelog house categories)

- **Newly rejected / newly warned / newly defined:** nothing.
- **Runtime behaviour:** unchanged everywhere.
- **Doc-only:** all of it. **Zero semantic migration.**
- **Structural summary:** 8 prose paragraphs (4,556 words ⧗) → 1 table (12 rows × 4 caller-kind cells = 48 classification cells, plus per-row callee-admission and citation columns) + ~1,900 words of surviving reasoning ⧗; 9 §30.5 bullets → 1 closure (absorbing bullet 4's sentence and bullet 5's chain-join clause) + 4 worked instances + 3 preserved notes; 42 Appendix C rows capped ⧗, 1 C-only fragment moved into §28.6. *[v0.261: landed as 8 rows × 4 callee-kind columns (32 classification cells) over the unified rule — the 12-row figure is the pre-0096 snapshot's; the 5,473-word §30.5 demotion kept two preserved notes (bullet 4's became the principle paragraph's sentence); the Appendix C census at HEAD was 44 rows.]*

## 6. The honest risk

**This is the largest editorial surface the review program touches, and the failure mode is not disagreement — it is transcription.** §28.3 is the most-cited section in the project; the eight paragraphs being folded ⧗ are the ones every other ownership rule points at; Appendix C is read by implementers as a spec-conformance checklist. Three specific risks:

1. **A dropped cell reads as a rule change.** Prose that says "any other such static signature is rejected" survives an editor's eye; a table cell that should read *rejected at the declaration* and is left blank does not. Mitigation: §1.3's cell-by-cell verification list is re-run by the reviewer against the snapshot, and every one of the 48 classification cells must trace to a landed sentence.
2. **A compressed Appendix C row silently narrows an invariant.** A 241-word row compressed to one sentence can lose a clause that no other section carries. Mitigation: §3.3's containment screen is re-run *before* compression, and the C+M dispositions are executed *first*.
3. **Fenced examples drift.** §28/§30 carry worked examples inside the paragraphs being restructured. Mitigation: this program edits **prose only** — a changed fence is a defect, and §7 item 5 makes that mechanical.

## 7. Mechanical validation, as a landing condition

The train does not report "done" without all six:

1. **`scripts/check_docs.sh`** (full, not `--quick`) green **before and after**: anchor resolution, version consistency, §-citation sanity, statics-facts derivation, example extraction.
2. **Anchor preservation.** Every `<a id="…">` in §28.3, §30.5, and Appendix C survives the restructure, because other documents link to them: `sec-producers`, `sec-factory-methods-and-sourceless-returns`, `sec-bare-type-parameter-returns`, `sec-iterable-instance-returns`, `sec-returning-a-deferred-activation-frame`, `sec-consume-site-freeze`, and the Appendix C section anchors. An anchor that must move gets a retained empty anchor at its old id. (check_docs' anchor check catches inbound breakage; it does not catch an anchor that vanished with nothing pointing at it *yet*.) *[Corrected at the v0.261 landing round: `sec-iterable-instance-returns` was deleted at v0.258 with every referrer re-pointed, so the condition applies to the anchors standing at the train's HEAD — all of which survive; the train added `sec-announcement-table`.]*
3. **Fragment extraction before and after.** `scripts/extract_doc_examples.py` is run over the pre-edit snapshot — materialized as a git worktree of the train's base revision — and over the post-edit tree, into two scratch trees under `/var/tmp/bezel-scratch`; the two fragment sets must be **byte-identical**. Prose-only edits produce no fragment diff, so any diff is a finding.
4. **The C-only screen, re-run at the train's HEAD** (§3.3's method), before any row is compressed. Every newly-flagged row is hand-resolved and its disposition recorded in the changelog entry.
5. **The no-verdict-changes checklist.** For each of the following, the reviewer states the verdict *before* and *after* and confirms they match:
   - a plain concrete class return on a static callee → rejected at the declaration;
   - the same on an instance callee → borrow, source the receiver;
   - the same on a function-value invocation → borrow carrying the function value and every plain reference-typed argument;
   - an unmarked `Task<T>` return on each of the four callee kinds → owned frame, no factory warning;
   - an unmarked `IEnumerable<T>` return: static → caller-owned frame; instance → borrow; instance returning a fresh state machine → compile error;
   - an unmarked `IOwningEnumerable<T>` return: static → caller-owned frame; instance/extension → rejected at the declaration;
   - a bare-`T` return with a non-empty source set at a class substitution → borrow with the union source set; at a value substitution → no constraint;
   - a bare-`T` return with an empty source set → rejected at the declaration; on an instance callee → cannot arise;
   - the calls-mint exclusion at a `Task`-family substitution of a bare-`T`-returning function slot → inadmissible at the instantiation — and identically at a substitution of one of the eight iterable/enumerator heads (v0.256, #3246), with stdlib §5's derived interfaces outside the excluded set;
   - each of §30.5's nine former bullets → still admitted, now as an instance of the K2 closure;
   - each of the 42 capped Appendix C rows ⧗ → the invariant it states is unchanged, and its content is present in the cited section.

   *[Corrected at the v0.261 landing round: the three iterable entries above state v0.256 verdicts that proposal 0096's graduation moved — at HEAD an unmarked `IEnumerable<T>`/`IOwningEnumerable<T>` return mints at every callee kind (a stored-view instance body rejected with the property-face remedy), and the iterable family carries no per-substitution exclusion. The landed checklist asserts the v0.258 verdicts; zero verdict moves at this train. The census at the train's HEAD measured 44 over-cap rows, not 42.]*
6. **A word-count report** in the changelog entry: §28.3 before/after, §30.5 before/after, Appendix C before/after. Not a target — a record, so the next reviewer can see what moved.

## 8. Sequencing and dependencies

**Proposal 0093 lands first.** The table's header sentence and rows 3–8 use the name **type-announced**; the Appendix C.3 yield row and C.6 Pattern B row carry announcement senses 0093 is re-pointing; and both proposals edit §28.3's *Producers* rows and Appendix C.3/C.6. Landing 0094 first would mean editing those rows twice.

**Proposal 0096 is a hard ordering constraint.** 0096 (iterable announcement unification — accepted under the same delegation) rewrites the very rules this proposal's table rows 3, 5, 6, 8, and 9 transcribe: calls-mint extends to the iterable family, axiom A2 retires to a tombstone, the instance-lends cells cease to be the law, and the v0.256 fence paragraphs (§1.1's two new rows) are deleted wholesale. A table derived before 0096 would be re-derived cell-by-cell immediately after it — the transcription risk of §6 paid twice on the book's most-cited section. **Ruled (coordinator, post-gate ordering ruling): order (a) is adopted — 0096 graduates first, and this proposal's §28.3 table is re-derived against the unified rule in its own, later graduation train** (program order: 0093 → 0095 → 0096 → … → 0094 re-derived). The table content in §1.2/§1.3 stands as the **pre-0096 snapshot** — the derivation record proving the method against the landed rules — and is superseded on 0096's landing. The §30.5 demotion and the Appendix C cap halves are 0096-independent in substance and may ride this proposal's own later train; their C.3/C.6 rows touching iterable classification are written once, post-0096.

Within this proposal the order is: **(a)** move the one C-only fragment into §28.6; **(b)** §28.3's table and the eight paragraphs' reduction (post-0096, against the unified rule); **(c)** §30.5's demotion; **(d)** Appendix C's cap (last, so it can cite the new table and the demoted closure).

## 9. Alternatives, costed

- **Alt A — the table only; leave §30.5 and Appendix C.** *For:* one section, one review sitting, the largest single win. *Against:* Appendix C.3's 241-word static-return row and C.6's 314-word Pattern A row are the two largest duplications *of exactly the text being deduplicated*; leaving them makes the appendix the surviving carrier of the prose the table replaced — the drift class inverted. **Not recommended.**
- **Alt B — Appendix C cap only.** *For:* smallest surface, mechanically checkable, no §28 risk. *Against:* the cap's rule is "content that exists only in C moves into the section it cites" — and for the two biggest rows the cited section is precisely the six-paragraph thicket. Capping into a thicket is not a simplification. **Not recommended** as the whole item; it is a defensible *fallback* if the train must be split across revisions, in which case it goes **last**, not first.
- **Alt C — decline; add a non-normative summary table to §28.3 and keep the prose.** *For:* zero risk to the normative text. *Against:* a seventh carrier of a rule already carried six times, and the one most likely to drift because nothing checks it. **Not recommended.**
- **Alt D — cap Appendix C by deleting over-cap rows outright rather than compressing.** *For:* mechanical. *Against:* the appendix's value is that it is a *complete* checklist; deleting a row removes an implementer's entry point. **Not recommended.**

## 10. Questions (drafting seams only — the rulings behind this proposal are settled)

- **Q1 — table granularity.** Twelve rows as drafted (recommended), or ten with rows 4 and 7 folded into 3 and 5–6 as "…, and identically when spelled `owned`"? The twelve-row form makes "the explicit marker changes nothing" a *visible cell* rather than a clause; the ten-row form is shorter.
- **Q2 — where the table lives.** Immediately after the *Producers* table, with the eight paragraphs' surviving reasoning beneath it (recommended — a reader arriving at *Producers* gets the call-classification answer in one place), or as its own `<a id>`'d subsection between *Producers* and *Factory methods*?
- **Q3 — the "cannot arise" cells (row 10).** Rendered as *cannot arise* (recommended — it is a derived fact worth stating), or as `—`? The first adds a claim the reviewer must check; §1.3 shows the check.
- **Q4 — the Appendix C cap's exact unit.** "One sentence of invariant + citations + an optional parenthesised kernel-tag list" (recommended, and the **C\*** disposition depends on it), or a hard one-sentence-including-everything cap? Fourteen of the 42 rows ⧗ are **C\*** under the first and would need content moved into their cited sections under the second.
- **Q5 — §30.5's four surviving worked instances.** Keep them as bullets under an explicit "worked instances, not a closed set" heading (recommended), or inline them into the principle paragraph as a parenthesised list? The bullets are what implementers currently navigate by.
- **Q6 — the lazy-query row (8).** The spec has no spellable type name for the category; the row names it as compiler-known (recommended, matching §29.11's own treatment), or the row is dropped and lazy queries are covered by a sentence beneath the table?

## 11. Graduation-train scope

*(Stage-2 note: this scope executes **after 0096 graduates** (§8), re-derived against the unified rule at the train's own HEAD — by then the two v0.256 fence paragraphs and the recorded conservatism are expected to have been deleted or generalized by 0096's train, and the table is drafted against what stands.)* *[v0.261: so it stood — the fence paragraphs and the conservatism reach were gone at HEAD, and the landed table was drafted against the unified rule.]*

**Spec §28.3**, in this order: (1) insert the §1.2 table (re-derived, per the stage-2 note) with its three keying rules, immediately after *Producers* (per Q2); (2) reduce *Factory methods and sourceless returns* to its sourcelessness reasoning; (3) reduce *Task-family calls produce owned frames* — or its post-0096 generalization — to its mint reasoning and property-face resolution; (4) reduce *Bare-type-parameter returns* to caller/callee reasoning + inheritance clause + calls-mint soundness; (5) fold *Concrete-slot function bodies* into row 2's admission cell plus one reason sentence; (6) reduce *Iterator and iterable returns…* and *Iterable-interface returns at instance and extension callees* (as they stand post-0096) to their surviving recorded arguments; (7) re-point *Borrow lifetime rules* bullet 2's three parenthetical cross-references and *Returning a deferred-activation frame*'s opening at the table.

**Spec §30.5:** the *Consumer-side rules* Pattern A block — the principle paragraph absorbs bullet 4's sentence, and the closure sentence absorbs bullet 5's chain-join clause (§2.2); the nine bullets become four worked instances under a non-exhaustive heading; bullets 3 and 6's notes are preserved beneath; bullets 5 and 9 then delete.

**Appendix C:** all fifteen subsections swept; the 42 rows of §3.2 dispositioned ⧗; the C.3 `Dispose()`-receiver row's A8 pointer moved into **§28.6** first.

**Rationale:** §12.10 (the announcement doctrine gains "the per-callee-kind matrix is spec §28.3's table"), §5.7 (the concrete-static selector cliff cites row 2's static cell).

**Stdlib:** §5 intro and §6.8 — two pointer sentences.

**Guides:** Guide 04 §2 *A task is a resource you own* and Guide 05 §3 *Consuming iteration* — the "the return type is the announcement" teaching sentences gain a table pointer. No guide rule text changes; no guide example changes.

**Changelog:** one entry, doc-only, carrying the §7 acceptance record and the §5 structural summary, and naming the §28 word-count drift (38,933 at v0.256 ⧗) so the next reviewer measures rather than inherits.

**Compiler:** none.

## 12. Interactions

- **Proposal 0093** (same basket) — hard dependency, lands first; see §8.
- **Proposal 0096** (iterable announcement unification) — **hard ordering constraint, ruled: 0096 graduates first and this proposal's §28.3 table re-derives against the unified rule in its own train** (§8; the §1.2 table is the pre-0096 snapshot). 0096 deletes the v0.256 fence paragraphs this proposal's §1.1 inventory counts and retires axiom A2, which rows 3, 5, and 9 currently quote.
- **Proposal 0095** (same basket) — renames §35.4's census, which Appendix C.3's choice row (62 words, disposition **C**) cites. Under the program order (0093 → 0095 → 0096 → … → 0094) 0095 graduates **before** this proposal's train, so the capped rows are written in the new name directly.
- **Appendix C.3 is a shared edit surface** — five accepted proposals touch it: 0093 (the kill-adjacent rows' announcement senses), this proposal (the cap), 0095 (the census rename in the choice rows), 0096 (the fence row's fate), and 0101 (the A13-adjacent rows, per its study's outcome). Composition rule, per the program order: 0093 first; 0096 before this proposal's stage-2; 0095's rename at its own train; and **each train re-reads C.3 at its own HEAD** rather than trusting any proposal's snapshot of it — the C-only screen (§3.3) re-runs each time for the same reason.
- **Proposal 0089** (graduated v0.245) retired §19.6's determination rule and shrank Appendix C.7; its precedent for "cap the appendix row, keep the section" is the shape this proposal generalizes.
- **Proposal 0087** (statics vocabulary, graduated v0.245) established the "N curated lists re-derived from orthogonal facts, with a reproduction table proving every membership" pattern; §1.3 and §7.5 are that pattern applied to classification cells.
- **Proposal 0046** (kernel consolidation) — the table is a K2.4/K6.2 derivation surface, not a new rule; the axiom register is untouched, and axiom A2's keying is quoted by rows 3, 5, and 9 without change.
