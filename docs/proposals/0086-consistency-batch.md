# Proposal 0086 — Consistency and record-keeping batch (elegance review, 2026-08-11)

| | |
|---|---|
| **Status** | graduated-v0.242 — superseded — graduated v0.242 (owner-ruled 2026-08-11, all Qs as recommended) |
| **Tracking issue** | [#3052](https://github.com/s8lap/bezel-lang/issues/3052) |
| **Target module(s)** | **spec** — §16.3/§16.4/§16.6 (item 1), §6.6/§17.1/§17.2 + A.6 `AssignOp` (item 2), §11.7/§12.4 + B.1 (item 3, if taken), §3.6/§14.2/§8.3 (item 4), §2.7 (item 6, presentation only). **rationale** — new entries for items 5, 7, 8; §9.5 (item 3). **stdlib** — §1 wording riders for item 4. **guides** — none. |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | The 2026-08-11 elegance review — the front-half critic's minor findings (report §8, plus finding 6) and the coordinator's minors/flagged rows, owner-accepted. Batch shape per proposals 0062/0067/0074: each item independently landable, individually ruled. |

*Reference convention: unprefixed §-references are language spec references; rationale and stdlib references are marked.*

---

## 0. Shape of the batch

Eight small items: two rule changes (1, 2), one spelling question (3), one vocabulary alignment (4), one presentation change (6), and three decline-with-reasons records whose entire deliverable is drafted rationale text (5, 7, 8). Items 5, 7, 8 change no behavior at all — they exist because the project's discipline is that decisions get recorded with reasoning, and these three were found made-but-unrecorded. Each item carries its own question; the owner can take any subset.

---

## 1. Range step validated at construction (§16.3/§16.4/§16.6)

**Friction.** One value of one property is currently three different things by position: a *literal* `step 0` is a compile error (§16.4); a *computed* step of zero constructs fine, is defined for `IsEmpty` and `Contains` ("direction-agreeing" — §16.3's range-operations text spends a parenthetical worked example on it), and raises `ArgumentException` only at `GetEnumerator()` (§16.4). A range value can therefore carry a zero step around indefinitely — stored, passed, compared, membership-tested — and faults only if someone eventually iterates. The zero-step *deferral to `GetEnumerator`* is recorded nowhere (the #1340/v0.162 owner corrections cover the membership formula and step-independence, not this).

**Proposal.** Validate at **construction**: evaluating a range expression whose computed step is zero raises `ArgumentException` at the expression. The constant case stays a compile error (§16.4, unchanged). Since range expressions are the sole producer of `Range<T>` values (the record is compiler-intrinsic and declares no constructor — §16.3; v0.241's §17.7 ruling closes `new` on builtins without declared constructors — the deserialization path §16.3 mentions for `Range<char>` validates identically), no zero-step `Range<T>` value can exist. Deletions follow: §16.3's direction-agreeing zero-step parenthetical (the state it describes is unconstructible), §16.4's at-`GetEnumerator` clause, §16.6's equal-bounds step-0 sentence. `IsEmpty`/`Contains`/`GetEnumerator` lose a case each. One caveat the batch flags rather than hides: a range expression is not a method boundary — proposal 0079 §4 rejects `ArgumentException` for operators on exactly the no-parameter-to-name ground, and 0078 §5 likewise for casts — so the fault type here is a **third site** of the 0079-Q2/0078-Q1 ruling and folds into it; the `ArgumentException` spelling above is the drafted placeholder, and whatever those proposals land, this site lands the same answer (Q1's rider).

**Alternative (status quo).** Keep laziness: "iteration is the operation a step parameterizes." Declined: `Contains` is *also* step-parameterized (through emptiness/direction, §16.3), so the principle is already half-false; and fault-at-earliest-point is the language's stated posture everywhere else (§5.1 constant narrowing, stdlib §1's negative-magnitudes-fault rule — which itself validates *constructor* arguments eagerly, the exact analogue).

**Migration.** Newly **changed observable behavior**: a program constructing a computed-zero-step range and never iterating it now faults at construction (strictly earlier; strictly more programs fault). Corpus: the `range_literal_step_zero_1628` lit family is unaffected in verdicts — the literal-rejection pins stand; `…runtime_computed_step_zero_not_flagged_at_compile_time` stays correct as written (still not a compile-time flag; the fault moves from `GetEnumerator` to construction and the test gains the runtime assertion). No doc example constructs a computed zero step.

**Q1** — validate at construction (recommended)? Rider: the fault type lands consistently with the 0079-Q2/0078-Q1 ruling — this is a third site of that ruling (a range expression is no more a method boundary than a shift or a cast).

---

## 2. `mod=` compound assignment (§6.6/§17.1/§17.2, A.6)

**Friction.** §6.6: "The compound assignment operators are `+=`, `-=`, `*=`, `/=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, and `??=`. `mod` has no compound assignment." Every binary arithmetic/bitwise operator has its compound form except one; §17.2 carries a bullet defending the exception ("`mod` is uncommon in compound forms"). "All except one, see the note" is a worse rule to learn than uniformity, whatever the frequency.

**Proposal.** Add `mod=`. Grammar delta: A.6's `AssignOp ::= ":=" | "+=" | "-=" | "*=" | "/=" | "&=" | "|=" | "^=" | "<<=" | ">>=" | "??="` gains `| "mod="` (one token, lexed by longest match — `mod` is a reserved keyword, `mod=` the keyword immediately followed by `=` with no intervening space; `x mod = y` remains the syntax error it is today). §17.1's assignment row gains it. Semantics: §6.6's desugaring verbatim — `x mod= e` is `x := x mod e`, target evaluated once, checked narrow write-back for integer/`char` targets (`b mod= n` on `byte`: the operation runs in `int` per §6.2, the store is the §5.2 checked narrowing — in-range by the nature of `mod`, but the rule needs no special case), `DivideByZeroException` per §6.1, operator overloads per §25.2 (a type overloading `mod` gets `mod=` mechanically, per §25.1's existing sentence). §17.2's defense bullet deletes.

**Honesty note (the corpus cuts the other way).** Grep: **zero** real `x := x mod c` sites exist in docs or tests — the only match is §17.2's own defense sentence. The recorded reason "uncommon" is *true*; the critic's charge was that it is "an assertion, not a weighing," and the weighing comes out: cost of adding ≈ one token, one table row, one desugar case that already generalizes; cost of the exception ≈ a permanent asymmetry in a rule users memorize, plus the defense bullet. Uniformity wins on cost, not on demand — stated plainly so the owner can weigh it with the evidence rather than the rhetoric.

**Alternative (status quo).** Keep the exception and its bullet; cost stays one asymmetry + one apology sentence. Genuinely cheap; declined on rule-shape grounds only.

**Migration.** Newly **admitted**: `mod=`. Nothing flips. **Q2** — add `mod=` (recommended, weakly held)?

---

## 3. `sealed abstract` → a self-describing spelling? (§11.7, rationale §9.5)

**Friction.** The combination means "closed abstract class: cannot be instantiated; cannot be subclassed outside the declaring module" (§11.7). Neither word says that: `sealed` alone means *no subclasses at all* (§12.4), so the pair reads as a contradiction, and rationale §9.5 must teach "the resolution reads the pair, not the words in isolation" — a defense conceding that the words mislead. The module-boundary seal is a *third* meaning carried by juxtaposition.

**Candidate.** A dedicated modifier: `public closed class Shape` — one word meaning exactly the §11.7 semantics. `closed` is not currently a keyword (B.1/B.2 checked; zero corpus identifiers named `closed`); it would be reserved (B.3's discipline: new keywords "drawn from … words not currently used as identifiers in the standard library" — satisfied). §19.6's prose already says "closed hierarchy"/"closed set" eleven times — the word is the spec's own vocabulary for the concept.

**Churn, honestly.** `sealed abstract` appears **26×** across the ten normative documents excluding the pinned changelog (spec 16, rationale 8, Guide 03 2; the changelog carries 10 more, pinned) and **86×** in tests — **~112** mechanical sites, plus every prose mention in §19.6/rationale §8.2/§9.5 and the guides' teaching. All grep-mechanical (the two-token sequence is unambiguous), and the no-users window argument (0074 item 1's precedent: "it only gets more expensive; the first external user makes it permanent") applies at full strength. But unlike 0074's rename this one also spends a **reserved word** — a permanent vocabulary cost — to relabel semantics nobody has reported misunderstanding *after* reading §11.7.

**The do-nothing option, framed fairly (the review's own low confidence).** The pair is learnable in one sentence, is already learned by every document in the tree, and rationale §9.5 exists precisely to teach it; C# users arrive knowing `sealed`, and `abstract` beside it is at least a *signal* that something non-default is happening — a fresh `closed` keyword is opaque until looked up too. The composition also has one virtue a single word lacks: `sealed` and `abstract` each keep their independent meanings elsewhere, and the §19.6 closed-set rule *already* covers hierarchies closed without the marker ("an `abstract` class whose only direct subclasses are themselves `sealed`"), so `closed` would name the root-marker idiom, not the concept.

**Recommendation:** adopt `closed` — **the batch's lowest-confidence recommendation**, carried by the window argument and the delete-the-apology payoff (rationale §9.5 shrinks to one line recording the rename). If the owner declines, the deliverable degrades gracefully to a doc-only touch: §11.7 gains the one-sentence gloss "read `sealed` here as sealing the *module boundary*" at first use, and §9.5 stands as the record. A dual spelling (both accepted) is not offered — two spellings of one semantics is the kind of duplicate the no-dual discipline (stdlib §1) exists to prevent.

**Migration (if adopted).** Newly **rejected**: `sealed abstract` (diagnostic names `closed`); newly **admitted**: `closed class`. ~112 mechanical sites (scope above); migration script per the `scripts/migrate_*.py` precedents. **Q3** — adopt `closed`, or decline and keep the pair (both defensible; recommendation above)?

---

## 4. One name for the `hashable` property (§3.6/§14.2/§8.3, stdlib §1)

**Friction.** One derived type property has two names: §3.6's table row calls it **equality-bearing**; the §14.2 constraint spells it **`hashable`**, with an inline apology ("the keyword names the capability site, the property keeps its §3.6 name"). Counted: `equality-bearing` **39×** across the spec, the stdlib doc, and the rationale; `hashable` **9×** in the same three documents. Two names for one concept is a documented seam, and every future rule citing the property must pick a side.

**Proposal.** One term: **hashable**, everywhere. The §3.6 table row renames (`equality-bearing` → `hashable`), with a parenthetical "(formerly *equality-bearing*)" for one revision's findability; §8.3/§9.2/§35.7/stdlib mentions follow mechanically (~39 sites across spec/stdlib/rationale, doc-only). Direction of deference: §14.2 already defers to §3.6 for the definition ("decided per instantiation by §3.6's computation") — that stays; only the name unifies. Why `hashable` wins: it is the user-facing spelling (the constraint keyword, already reserved contextually in B.2 — renaming the *keyword* instead would be a source-breaking change for nothing), it names the capability the property gates (hash-container keys/elements), and it is one word.

**Alternative (status quo, the seam recorded).** The §14.2 parenthetical *is* the record; keeping it costs the dual vocabulary permanently. Declined — the fix is doc-only and mechanical. (The adjacent §14.4 "slight imprecision" the review noted — `where T: class` admitting `string`/function types that owning collections then reject — is a *documented* seam with a real type-system reason and is deliberately **not** batched here; it would be a rule change, not vocabulary.)

**Migration.** Doc-only. No grammar, no behavior; the B.2 row is already `hashable`. **Q4** — unify on `hashable` (recommended)?

---

## 5. Record the enum-prefix case-only asymmetry as deliberate (§10.6 — rationale entry, text drafted)

**Friction.** `case p of Low => …` omits the enum prefix (§10.6, §19.8); `if p = Low` does not — it requires `Priority.Low`. Defensible, but the asymmetry is unrecorded; it is the kind of front-of-face question that gets re-asked until written down.

**Deliverable: the rationale entry, drafted** (new subsection under rationale §11, "Pattern matching"):

> **Enum-prefix omission is a pattern-position affordance, deliberately not an expression-position one.** Inside a `case` over an enum (or choice) scrutinee, the prefix may be omitted (`case p of Low => …`, spec §10.6/§19.8) because pattern position has exactly one scrutinee, fixed by `of`, whose type is known before any pattern is read — resolution is scrutinee-directed by construction, and a bare constant there can mean nothing else (§19.8: "a case name resolves to a case pattern, never a type test"). Expression position has no such anchor: in `p = Low`, the two operands are peers — nothing marks `p` as the side that supplies `Low`'s type, and a rule that resolved `Low` against "the other operand's type" would make name resolution type-directed and bidirectional, which §2.7's position-keyed, no-backtracking design deliberately excludes (the recorded decline of C#'s dual-meaning rule). Swift's leading-dot syntax (`p == .low`) shows the feature is buildable — at the cost of a dedicated sigil form and inference-driven member lookup pervading expression syntax; Bezel declines that machinery for the saving of one qualifier. The asymmetry is therefore the composition of two settled designs, not an oversight: patterns are scrutinee-typed, expressions are position-resolved. Write `Priority.Low` in expressions; inside the type's own body, §2.7's enclosing-member lookup already shortens it to `Low`.

**Migration.** Doc-only (rationale). **Q5** — land the entry as drafted?

---

## 6. A decision table for §2.7's enclosing-member paragraph (presentation only)

**Friction.** §2.7's enclosing-member paragraph is the densest single paragraph in the half — one ~700-word block deciding member-vs-type precedence from token shape. The rule is good (deterministic, never backtracks); its statement wants a table.

**Deliverable: the table, drafted** (inserted after the paragraph's first sentence; the prose keeps the normative definitions, the table carries the case analysis):

> | Head shape (decided from tokens, before any lookup) | Example | Enclosing-type members rank | Effect |
> |---|---|---|---|
> | **Chain head** — name followed by `.` (directly, or with a balanced generic-argument group: §22.12's scan) | `Color.Red;` `Registry<int>.M()` | **last, after aliases** | a reachable type outranks a same-named member; `property Color: Color` still lets `Color.Red` reach the choice type |
> | Bare value read | `Data` | **between local scope and module declarations** | member outranks same-named top-level/imported type |
> | Assignment target | `C := c;` | same | a field write is never preempted (a type cannot occupy value position) |
> | Bare callee (incl. explicit method type args) | `Helper();` `Method<int>(x)` | same | a callee is not a receiver |
> | Head continued by `?.` or postfix `!` | `Color?.ToString()` | same | these operators demand a nullable *value*; static access is spelled with `.` alone |
> | **Warning row:** chain head resolves to a type while a same-named accessible member exists | — | — | warn, *unless* the member's declared type is the resolved type (or an instantiation of the resolved generic) — the `property Color: Color` idiom; qualify `this.` to reach the member |

**Migration.** Doc-only; the paragraph's normative sentences are untouched (the table is derived presentation, checked against each prose clause at landing). **Q6** — land the table?

---

## 7. Record the `is T name` decline with reasons (§12.7 — rationale entry, text drafted)

**Friction.** §12.7 states flatly "`is` does not bind. There is no `is T name` syntax. To bind, use `as`." The decline is real and, per the review, defensible — but nowhere recorded; proposal 0062's deferred register covers Rust-style as-patterns (`name @ Pattern`), a different construct. The everyday cost is one line at fields and call results (`var d := _pet as Dog; if d <> null then … end;`), so the question will keep arriving.

**Deliverable: the rationale entry, drafted** (new subsection under rationale §11):

> **`is` does not bind — the three-construct decomposition, recorded.** Bezel splits what C#'s `is T name` bundles: `is`/`is not` are the *boolean* type tests (with flow narrowing on locals and `readonly` fields, spec §7.4/§12.7 — the tested variable itself narrows, so the common tested-local case needs no binding at all); `as` is the *nullable-binding* form; `case` typed patterns are the *multi-way binding* form. A fourth spelling, `is T name`, would buy one line at exactly the sites narrowing does not reach (mutable fields, call results) and would import the C# feature's recorded swamp: a pattern variable declared inside a boolean expression must be scoped by definite-assignment flow — bindings escape into `else` branches under negation (`if (!(x is T t)) return;` leaving `t` usable after), into the enclosing scope, and into `&&`/`||` arms — a scoping model so widely misread that it is a staple of C# style guides. Bezel's decomposition keeps every binding at a statement-shaped introduction (`var`, `case` arm) where scope is block-shaped and readable. The decline is of the *spelling*, not the capability: `var d := x as Dog; if d <> null then … end;` is the sanctioned two-step, one line longer, with no new scoping rules. If evidence accumulates that the two-step dominates real code at fields (post-§7.4-readonly-narrowing, the residual is mutable fields and expression results), the recorded lift is `is T name` with **true-branch-only** scope and no negation flow — the tight-scoping variant — which this entry pre-commits as the only acceptable shape.

**Migration.** Doc-only (rationale). §12.7 gains a one-line pointer. **Q7** — record the decline as drafted (recommended), or adopt tight-scoped `is T name` now (the review's assessment and this batch recommend against: a third binding spelling duplicating `case` at one line's savings)?

---

## 8. Record the collection-literal value-element inference decline (§15.4 — rationale entry, text drafted)

**Friction.** `var x := [1, 2, 3];` is a compile error (§15.4: "A collection literal has no standalone type"); every literal binding needs an annotation. For class-typed elements the refusal is principled (borrow-vs-own has no default); for value elements a default (`Array<int>`) is *coherent*, and the option was never weighed on the record. The coordinator's synthesis recommends **against** the carve-out; the front critic's own confidence was low. The deliverable is the decline record, so the question stops being re-asked.

**Deliverable: the rationale entry, drafted** (appended to rationale §5.16):

> **Value-element inference (`var x := [1, 2, 3];` defaulting to `Array<int>`) — considered and declined (2026-08, elegance review).** The carve-out is coherent for value and string elements, and would delete an annotation from a common binding. Declined on three grounds. (1) *The uniform rule is load-bearing for ownership.* The v0.238 literal-tree rule (spec §15.4) assigns every fresh node of a literal an owner derived from the **target**: owning-family slots take nodes by transfer, borrowing-family targets aggregate them, inline arrays copy. A literal with no target has no ownership story to compute; a value-element carve-out would make "does this literal need a target?" depend on element classification — recursively, since elements may be literals (`[[1], [2]]` — value leaves under a class-typed spine), splitting one rule into a family. (2) *Container choice is a silent decision.* `[1, 2, 3]` is equally an `Array<int>`, a `[3]int` inline array, a `Set<int>`, a `Queue<int>`; electing `Array<int>` makes the language guess where §22.6's resolution rules refuse to ("the compiler declines to guess"), and the guess is sticky — element-type inference (`[1, 2, 3]` as `Array<byte>`? §5.1 constant narrowing is target-driven) would be pinned to `int` by the default in exactly the positions where an annotation today chooses. (3) *The annotation is one type name, and the language keeps it meaningful*: `var xs: Array<int> := [1, 2, 3];` states the two decisions (container, element type) the literal alone cannot. Reopening requires evidence that annotation weight dominates real code *and* an ownership story for target-free literal trees; absent both, the uniform no-standalone-type rule stands.

**Migration.** Doc-only (rationale). **Q8** — land the decline record as drafted?

---

## 9. Corpus evidence (batch summary)

- Item 1: `range_literal_step_zero_1628` lit family (8 files) — verdicts preserved, one test gains a runtime assertion; no doc example constructs a computed zero step.
- Item 2: **0** `x := x mod c` sites outside §17.2's own sentence (the honesty note).
- Item 3: `sealed abstract` **26** occurrences across the ten normative documents excluding the pinned changelog + **86** in tests (the churn number).
- Item 4: `equality-bearing` **39** / `hashable` **9** occurrences across spec/stdlib/rationale (the seam's size); `closed` and `equality`-adjacent identifier collisions: **0**.
- Items 5–8: doc-only; no corpus exposure.

## 10. Questions for the owner (collected)

- **Q1** (item 1): range step validated at construction, fault type folded into the shared 0079-Q2/0078-Q1 ruling (item 1's rider)? *Recommended.*
- **Q2** (item 2): add `mod=`? *Recommended, weakly — the corpus supports "uncommon"; the argument is rule-shape.*
- **Q3** (item 3): `closed` replacing `sealed abstract`, or decline-with-gloss? *Recommended: `closed` — lowest-confidence item; the fallback text is in the item.*
- **Q4** (item 4): unify vocabulary on `hashable`? *Recommended.*
- **Q5** (item 5): land the §10.6 asymmetry rationale entry? *Recommended.*
- **Q6** (item 6): land the §2.7 table? *Recommended.*
- **Q7** (item 7): record the `is T name` decline (vs. adopting tight-scoped binding)? *Recommended: record.*
- **Q8** (item 8): land the §15.4 inference decline record? *Recommended.*

## 11. Interactions

- Item 1 ↔ **proposals 0079/0078** (#3045/#3044): the fault-type rider (item 1's Q1) is a third site of their shared Q2/Q1 ruling; 0079's Interactions carries the reciprocal pointer.
- Item 2 ↔ **proposal 0079** (#3045): both edit §6.6/§17.2; textual coordination at landing.
- Item 3 ↔ **proposal 0089** (#3055): §19.6's sealed-hierarchy bullet names `sealed abstract` throughout; if both land, the rename and the 0089 outcome should be sequenced in one §19.6 edit pass.
- Item 4 ↔ **proposal 0084** (#3050): 0084 changes when the property holds; item 4 renames it. Either order; cross-cite.
- Item 8 ↔ **spec §20's v0.241 branch-target-typing decline** ([#3042](https://github.com/s8lap/bezel-lang/issues/3042)): the same no-target-no-literal posture from the branch side; the drafted entry cites the literal side only, deliberately — the two records reinforce without depending.
- Items 5/7 live in rationale §11 beside §11.1–§11.4; no content overlap with the sibling proposals.
