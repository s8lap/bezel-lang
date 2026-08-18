# Proposal 0095 — Names that read: the census rename, and `defaultable` as a registered derived type property

| | |
|---|---|
| **Status** | graduated-v0.257 — superseded — graduated v0.257 (ruled under the owner's 2026-08-18 delegation; one train with proposal 0093, whose §35.4 census-first restatement landed written in this proposal's name; acceptance tests passed cell-for-cell at the train's HEAD) |
| **Tracking issue** | coordinator files |
| **Target module(s)** | **B4 (rename)** — **spec** §3.6 (three property rows), §9.7, §15.3, §22.16 ⧗ (the fused slip-and-census site), §28.3 (*The consume-site freeze*, *Producers*, K5.5; the *Deferred-activation* frame descriptor and §35.11's payload clause are **checked, not renamed**), §30.1, §30.14, §35.3, §35.4, §35.12 ⧗, Appendix C.1/C.3/C.6; **rationale** §3.14 (the v0.252 choice record) plus the two record-slip sites (§5.15, §15); **stdlib** §3.4 (one slip sentence); **Guide 04** §5. (The earlier draft's §28.5 entry is struck — zero occurrences ⧗ — and its §15.1/§17.7 entries belong to B5.) **B5 (`defaultable`)** — **spec** §3.6 (a new row), §17.11 (the home), §7.9, §9.8, §14.2, §15.1, §17.7, §22.7, §35.10, Appendix C.1/C.4; **rationale** the `default`-surface record (§9.6). Doc-only: **no semantics move**; one enumeration omission is corrected as a by-product (§4.4), and one interface-classification consequence is newly *stated* (§6). |
| **Written against** | spec/stdlib/rationale/guides **v0.255** (git `5fa5a95b`, snapshot 2026-08-18). Every count is measured against that snapshot unless marked otherwise; `src/` and `tests/` were read **read-only** for the diagnostic-surface counts in §3.4 and §5. |
| **Currency** | Re-verified against **v0.256** (git `4130d2de`, the review program's defect basket, plus its gate-fix commit `e8f5387a`; merged HEAD `8f41e99d`). v0.256 touched **no** `census`/`borrow-carrying`/`defaultable`-spelling line, so the B4/B5 measurement surface is unchanged; counts marked **⧗** were nonetheless re-measured at the merged revision, with reclassifications noted where the re-read tightened the snapshot's sense assignments. The graduation train re-measures at its own HEAD regardless. |
| **Origin** | Review-program charter items **B4** and **B5** (consolidation basket), ruled under the owner's 2026-08-18 delegation. B4's trigger: §3.6's own property table has to spend a clause on each of two adjacent rows distinguishing **borrow-bearing** from **borrow-carrying (the census)** — two `borrow-<participle>` terms whose difference the table cannot make visible. B5's trigger: three vocabularies ("well-defined default", "zero-initializable", §17.11's list) name one structural predicate across nine sections, and the spec already writes the word **defaultable** twice without defining it. |

*Reference convention: unprefixed §-references are language-spec references; `K` tags are §28.3's kernel ids; rationale, stdlib, and guide references are marked. Quotations are verbatim from the cited revision; any bold or italic emphasis this document adds to a quotation is marked "(emphasis added)" — unmarked emphasis inside a quotation is the source's own. §3.6's four common rules for derived type properties — computed-never-declared, per-instantiation evaluation, part of the public contract, named in diagnostics — are cited by number throughout.*

---

## Part I — B4: the census rename

## 1. The collision, measured

§3.6's table carries ten derived type properties. Two of them are adjacent and are told apart only by prose *inside the cells*:

> | **borrow-bearing** | §9.7 | records (and inline arrays, §15.1) carrying at least one borrow … ; for choices the §35.4 rule-2 classification — **distinct from §35.4's borrow-carrying *census***, which an owning choice can also satisfy … |
> | **borrow-carrying (the census)** | §35.4 | every choice type: whether any case has a **borrow-carrying payload field** … |

The second row's *name* and the second row's *definition* use the same string for different things: the property is called **borrow-carrying**, and what it tests for is a **borrow-carrying payload field** — §9.7's field-level predicate, which is a different property at a different granularity. §35.4's own definition sentence does the same in one clause:

> "A choice **carries borrows** iff any case has a *borrow-carrying* payload field."

And §3.6 rule 4 requires a diagnostic to *"name the property in the words of the table above"*. Today that produces "type `Loaded<T>` is borrow-carrying" one line away from "type `Span<T>` is borrow-bearing" — two different properties, at two different granularities, in near-identical words.

Measured, `borrow-carrying` appears **41 times across 28 paragraph sites** in the nine normative documents (⧗ re-verified at v0.256 — identical), carrying **five distinguishable senses** (⧗ counts below are the re-read's tightened classification; the snapshot's coarser split of 21/11/4/2/3 moved five occurrences between rows without changing the total):

| Sense | occurrences ⧗ | example |
|---|---|---|
| **the §35.4 census** (type-level, choices) | 16 (spec 14, rationale 2) | "a **borrow-carrying owning choice** may not be a class field"; "the borrow-carrying census" |
| **§9.7's field/value predicate** (field-, component-, and value-level) | 15 (spec 14, rationale 1) | "A field is borrow-carrying when: …"; "a borrow-bearing construction is a borrow-carrying value under §9.7's rules" (§35.3); §35.11's bare-type-parameter payload clause |
| **deferred-activation frame descriptor** (§28.3) | 4 | "it is treated as a borrow-carrying value whose sources are the call's receiver…" |
| **inline-array type label** — a slip; §3.6 files inline arrays under *borrow-bearing* | 2 | "Boxing a borrow-bearing record or a **borrow-carrying inline array**" (C.1); the same in C.3 |
| **record type label** — a slip; §9.7's label is *borrow-bearing record* | 4 | "a **borrow-carrying record** has no C-compatible form" (rationale ×2, stdlib ×1); "a borrow-carrying record or choice operand" (§22.16 — the fused site, §3.5) |

The last two are drift the rename makes visible and fixes in passing (§3.5).

`census` appears **36 times**; **one** of those is an unrelated homonym — rationale §8.2's record of §19.6's retired determination rule, which had "a census prohibition". So 35 occurrences across 24 paragraph sites are the property (⧗ re-verified: spec 29 occ / 22 sites, rationale 5/1 — the v0.252 record at rationale §3.14 — Guide 04 1/1).

## 2. The criterion

The charter states it: **one-glance distinctness from "borrow-bearing" in prose AND diagnostics**, under §3.6 rule 4. Operationally, a candidate must

1. not be a `borrow-<participle>` compound (that shape *is* the collision);
2. read as a grammatical clause in a diagnostic — §3.6 rule 4 requires the property to be *named*, not described;
3. not collide with §9.7's field-level `borrow-carrying`, which stays;
4. match the house style for derived type properties, which is literal and descriptive (`trivial-dispose`, `sendable`, `hashable`, `move-only`, `auto-hashable`, `C-compatible`), not metaphorical.

## 3. The choice: **holds-borrows**

### 3.1 The name

> | **holds-borrows** | [§35.4](#sec-choice-classification) | every choice type: whether any case has a *borrow-carrying* payload field ([§9.7](#sec-records-and-disposal)'s field-level predicate), reading **this** property at a nested choice payload — the condition on which the §9.7 borrow-lifetime obligations key for choices; an owning choice can hold borrows beside its owned payloads (v0.252) |

Prose form: *"a choice that **holds borrows**"*, *"an owning choice **holds borrows** when …"*. Diagnostic form: *"`Loaded<Response>` holds borrows at this instantiation (§35.4); a value that holds borrows may not be stored in a class instance field."*

### 3.2 Why, against the neighbour

Beside the term it must be distinguished from, in the very sentence where they meet today:

- **now:** "A **borrow-bearing** choice always carries borrows; an owning choice *can* … a **value-like** choice never does."
- **after:** "A **borrow-bearing** choice always **holds borrows**; an owning choice *can* … a **value-like** choice never does."

And in §35.4's definition sentence, which today is a near-tautology:

- **now:** "A choice **carries borrows** iff any case has a *borrow-carrying* payload field."
- **after:** "A choice **holds borrows** iff any case has a *borrow-carrying* payload field."

The second reads as a definition — one property defined in terms of a different, root-distinct one — which is what it is. `holds-borrows` shares no root with `borrow-bearing` and none with `borrow-carrying`, so criteria 1 and 3 are met by construction; it is an ordinary English verb phrase, so criterion 2 is met; and it is literal, so criterion 4 is met.

### 3.3 The candidates, costed

- **`carries-borrows`** — *the closest to today's prose*: §35.4 already writes "carries borrows" 26 times, so the property name and the prose would be one string at zero verb churn. **Rejected** on criterion 3: it shares its root with §9.7's `borrow-carrying`, which stays, and the definition sentence would read "A choice **carries-borrows** iff any case has a *borrow-carrying* payload field" — the near-tautology preserved, just hyphenated. The collision the rename exists to remove would be moved, not removed.
- **`carried-borrows census`** (a charter candidate) — **rejected** on criteria 1, 2, and 3: it keeps `borrow` beside a participle (criterion 1 in substance if not in letter), it is a two-word noun phrase that a §3.6-rule-4 diagnostic cannot use as a predicate ("type X is carried-borrows census" is not a sentence), and it retains the bare noun **census**, which is a live homonym — see §3.4.
- **`holds-borrows`** (a charter candidate, **recommended**) — meets all four; costs a verb respelling (§3.6).
- **`borrow-holding`** — **rejected** on criterion 1: it is exactly the `borrow-<participle>` shape.
- **`tethered`**, **`alias-holding`**, **`source-bound`** — **rejected** on criterion 4: new metaphors in a table whose other nine names are literal. `source-bound` additionally collides with §28.3's *source* vocabulary (K2's source sets), which is load-bearing three sections away.
- **`sourced`** — **rejected**: proposal 0088 reserves `sourced` as a return-position marker (stage 2, owner-gated). A property name that may become a keyword is not available.

### 3.4 Two homonyms the rename retires, measured in the compiler

Read-only measurement of `src/` and `tests/` at the snapshot:

- **`census`** appears **16 times in `src/`** ⧗ (all of them in `src/sema/ExprTypecheck*`) **and 14 in `tests/`** — and **every one of them is §19.6's determination-rule census or an unrelated engineering census** ("IR census", a serializer's captured-component census, sendability's member census), not §35.4's property (`src/sema/ExprTypecheck.cpp`: *"never a census of a bound's conformers (the §19.6 census prohibition)"*; `tests/codegen/async_chained_return_nested_body_2731_test.cpp`: *"IR census pre-fix vs post-fix"*). No diagnostic string in `src/` contains the word. Dropping the bare noun from the property's name removes a homonym that is already live in the implementation vocabulary.
- **`borrow-carrying`** appears **53 times in `src/` and 56 in `tests/`** (⧗ re-verified). `src/ownership/Ownership_Checks.cpp` alone carries **27 occurrences** ⧗: **four are diagnostic-string fragments** (`:4880`, `:4892`, `:5273`, `:8350`), **one is a comment-quoted phrase** (`:3024`, quoting *"no borrow-carrying field"*), and the remaining twenty-two are ordinary comments — and every diagnostic fragment uses the phrase **only at the field/component level** (*"store a value whose borrow-carrying components…"*, *"temporary bound to its borrow-carrying field `…`"*), i.e. §9.7's predicate, which this proposal keeps.

**Consequence: the rename costs zero diagnostic-string edits.** No shipped diagnostic names the census; the four diagnostic fragments that say "borrow-carrying" are already saying §9.7's thing, and after the rename they are the *only* thing that phrase means. That is the strongest single argument for the rename and it is measured, not asserted.

### 3.5 Two vocabulary slips, fixed in passing — six occurrences ⧗

All are visible only once the two terms are distinguishable, and none changes a verdict:

- **"a borrow-carrying record"** (4 occurrences ⧗): rationale §5.15's v0.188 ASCII-span retirement record ×1 and rationale §15's Strings FFI sentence ×1 (the earlier draft mis-attributed these to "§16.7" — the FFI reasoning lives at those two sites at v0.256), stdlib §3.4's `ReadOnlyAsciiByteSpan` tombstone ×1 (not "§10"), and **spec §22.16's `old(E)` capture paragraph ×1** — *"a borrow-carrying record or choice operand — §9.7, §35.4's census —"*, the **fused site**: at once a fourth record-slip and a census carrier, respelling to *"a borrow-bearing record or a choice that holds borrows"*. §9.7's type-level label for a record is **borrow-bearing record**; `borrow-carrying` at these sites is the field-level predicate applied to the wrong granularity. Respell to *borrow-bearing record*.
- **"a borrow-carrying inline array"** (2 occurrences: Appendix C.1's boxing row, C.3's static-field row) — §3.6's **borrow-bearing** row explicitly covers "records (and inline arrays, §15.1)", so the type-level label for an inline array is *borrow-bearing* too. Respell.

### 3.6 The verb question, and what it costs

The property name is a verb phrase, so the prose that reads it should use the same verb. Today the prose says **"carries borrows" / "carry borrows"** — **26 occurrences** (⧗ re-verified), and they split by subject:

- **20 read the census predicate of a choice** ⧗ (spec 18, rationale 2 — every occurrence whose subject is a choice, the census's own definition and derivation sentences included, plus the two inline-array contrast clauses *inside* census reasoning: §15.3's and rationale §3.14's "an owning inline array never carries borrows"). These respell under option (a).
- **6 are ordinary English about non-choice subjects** ⧗ and are **not** reads of the property: §8.6's boxed inline array ("the boxed array carries borrows"), §9.7's record identity ("a record that carries borrows *is* a borrow-bearing record"), §30.14's borrowing-collection sentence and its stdlib §4-intro twin, and stdlib §4.9 `OwningBuffer`'s two value-subject sentences. These **keep the existing verb** — respelling them would put the property's name on things that are not choices — and the stdlib thereby stays untouched, exactly as §5 states.

Two options for the 20:

- **(a) respell the verb** (recommended): every choice-subject "carries borrows" becomes "holds borrows", so the property name and the prose are one string and a reader never meets two phrasings of one predicate. Cost: 20 occurrences on top of the 51 of §3.7.
- **(b) keep the verb**: the property is named `holds-borrows` and the prose keeps saying "carries borrows". Cost: zero edits, but the §3.6-rule-4 diagnostic ("holds borrows") and the normative prose ("carries borrows") say the same thing in different words — the defect one level down.

Put to the coordinator as **Q1**; (a) is recommended and this proposal's counts assume it.

### 3.7 Migration count (B4)

| What | occurrences | paragraph sites |
|---|---|---|
| `census`, census-sense (excludes rationale §8.2's homonym) | 35 | 24 |
| `borrow-carrying`, census-sense ⧗ | 16 | 12 — every one also a census-token site |
| **rename carriers (union)** ⧗ | **51** | **24** |
| "carries/carry borrows" → "holds/hold borrows" (Q1(a) — the 20 choice-subject occurrences, §3.6; the 6 non-choice ones keep the verb) ⧗ | 20 | — |
| slips fixed in passing (§3.5) ⧗ | 6 | 6 — one of them (§22.16) shared with the rename surface |
| `borrow-carrying`, **kept** (§9.7 field/value predicate — §35.11's payload clause and §35.3's two "borrow-carrying value" reads included) ⧗ | 15 | — |
| `borrow-carrying`, **kept** (deferred-activation frame descriptor, §28.3) | 4 | — |

Distribution of the 24 rename sites ⧗: **spec 22** (§3.6 ×3 rows; §9.7's runtime-managed paragraph; §15.3's field-placement bullet; §22.16's `old(E)` paragraph — the fused site; §28.3 ×3 — the consume-site freeze, K5.5, the producers move-only row; §30.1 ×3; §30.14; §35.3 ×2; §35.4 ×2; §35.12's transfer-closure clause; Appendix C.3 ×2, C.6 ×2), **rationale 1** (§3.14, the v0.252 choice record — 7 census-sense occurrences in one paragraph), **Guide 04 1** (§5's sendability paragraph). Checked-but-not-renamed ⧗: §28.3's two frame-descriptor paragraphs and §35.11's bare-type-parameter payload clause (all field-/frame-sense). Slip-only sites outside the union: Appendix C.1, rationale §5.15 and §15, stdlib §3.4. The earlier draft's §28.5 entry is struck (zero occurrences ⧗), and its §35.11 and frame-paragraph entries moved to the checked list — the re-read classed both as the kept predicate, not the census. No stdlib site names the census.

---

## Part II — B5: `defaultable`

## 4. One predicate, three vocabularies

### 4.1 What the snapshot says

The predicate is "does `default(T)` exist for this type?". It is stated three ways:

- **§17.11 (the operator's home)** — a nine-bullet list ⧗ of what `default(T)` produces (numerics, `bool`, `char`, enums, nullable references, records, inline arrays, the choice error, the unit type), plus a four-bullet list of what it rejects, plus a closing sentence: *"A substitution whose `default` is not well-defined … is rejected at the instantiation site (the §3.6 rule 2 pattern)"* — which invokes the derived-type-property machinery **without the property being registered**.
- **§7.9 *Default initialization*** — *"…when the type has a **well-defined default value**: zero for numerics, `false` for `bool`, the all-default record …"*, then the carve-out: *"For value types whose default is not well-defined — records with non-nullable reference fields, inline arrays with non-nullable reference elements, owning inline arrays with non-nullable element type, enums with no declared zero-valued constant — …"*. Two enumerations of the same list, one positive and one negative.
- **§15.1 *Initialization*** — *"When the element type T is ***zero-initializable*** — every value-typed T whose fields recursively zero-initialize, or any nullable type T? (an enum is zero-initializable only when it declares a zero-valued constant, §17.11) —"*. A third spelling, and a third enumeration.

Two more sections read the predicate under yet other spellings: §14.2's `new()` row (*"value types … whose `default(T)` is well-defined"* — plus a fourth enumeration in its table cell), and §9.8's `serialized skip` (*"a component whose type has **no well-defined default** is a compile error"*).

Measured ⧗: **19 predicate-spelling paragraph sites, 26 occurrences** (24 retired-spelling occurrences per §4.5's table, plus the two undefined `defaultable` uses of §4.2), **five spellings** (the `well-defined default` family — "has a well-defined default (value)", "has no well-defined default", "`default(T)` is (not) well-defined", "its `default` is well-defined" — `well-defined zero value`, `zero-initializable`, `zero-initialize(s|d)`, `no principled zero`).

### 4.2 The spec already reaches for the word

Twice, undefined:

- **§9.8**, wire shape: *"on a **payload field** the full vocabulary applies (`skip` requiring a **defaultable** payload type)."* *(second emphasis added)*
- **§22.7**, the null watermark: *"before construction begins, every reference-typed field of the allocation is initialized to `null` and every **defaultable** value-typed field to its §7.9 default (normative)."* *(emphasis added)*

Both are normative sentences using an unregistered term. Registering it does not introduce vocabulary; it defines vocabulary that is already in normative use.

### 4.3 The registration

A new §3.6 row:

> | **defaultable** | [§17.11](#sec-the-default-operator) | types for which `default(T)` is defined: every nullable type `T?`; and every non-nullable value type whose components are recursively defaultable — the primitives, the unit type, an enum **declaring a zero-valued constant**, a record all of whose declared fields are defaultable, an inline array whose element type is defaultable (the owning forms `[N owned]T?` included). Not defaultable: non-nullable reference types; **every choice type** ([§35.10](#sec-choice-default-with)); an enum with no zero-valued constant; and any record or inline array with a non-defaultable component |

**What the registration attaches, stated exactly.** Registering the property attaches §3.6's four common rules to a predicate the compiler already enforces — and the four split unevenly:

- **Rules 1 and 2 are already in force**: the predicate is computed, never declared, and §17.11's own closing sentence already invokes *"the §3.6 rule 2 pattern"* for per-instantiation rejection — registration makes the register agree with a rule that already cites it.
- **Rules 3 and 4 are newly *stated*, not newly true.** Under rule 3 (with §2.8.2's interface-classification frame), a declaration change that flips `defaultable` — adding a non-nullable reference field to a record, removing an enum's zero-valued constant — is thereby **classified a breaking interface change**, a classification the spec has never carried in so many words for this predicate. Under rule 4 diagnostics name the property — and the compiler **already ships the word**: `src/sema/TypeResolution_Checks.cpp:7501`'s §9.8 conformance diagnostic reads *"mark the payload `serialized skip` (if defaultable), make the payload type serializable, or …"*. No verdict changes; the recorded breaking-change classification is the one consequence registration adds, and it records current practice.

§17.11 stays the home and gains the one-line definition plus its per-kind value table (unchanged content); the other sections reduce to the name and a pointer:

- **§7.9 *Default initialization*** becomes, in place of two enumerations:
  > "A field or local whose type is **defaultable** ([§17.11](#sec-the-default-operator)) satisfies the must-initialize rule vacuously and is implicitly initialized to that default; an explicit initializer is permitted and overrides it. A field or local whose type is **not** defaultable — a non-nullable reference type, a choice, or a value type with a non-defaultable component — requires an explicit initializer (for a field: a field initializer or assignment in every constructor; for a local: assignment before first read)."
  The `var`-declaration paragraph reduces to one sentence reading the same property.
- **§15.1 *Initialization*** retires *zero-initializable*:
  > "When the element type is **defaultable** ([§17.11](#sec-the-default-operator)), an inline-array declaration without an explicit initializer is filled with that element default per slot; otherwise an explicit initializer is required at declaration."
  §17.7's `new`-operator sentence (*"null fill at declaration when the element type is zero-initializable"*) takes the same respelling.
- **§14.2's `new()` row** cites it: *"Class types with a `public` parameterless constructor; **defaultable** value types ([§17.11](#sec-the-default-operator))."* Its prose keeps the two arms' reasoning and drops its enumeration; the sentence *"**Choices never satisfy the value-type arm**: `default(C)` is undefined for every choice type"* becomes a consequence of the property rather than a separately-stated exception.
- **§9.8's `serialized skip`** and **§22.7's watermark** name the registered property (§22.7 already does).
- **Appendix C.1's two `default(T)` rejection rows** and **C.4's `new()` row** cite the property; per proposal 0094's cap they are already one-sentence rows.

### 4.4 The §7.9 choice omission, fixed by construction

§7.9's *Default initialization* enumerates value kinds twice — *"primitives, records, enums, inline arrays, the unit type"* and *"records with non-nullable reference fields, inline arrays with non-nullable reference elements, owning inline arrays with non-nullable element type, enums with no declared zero-valued constant"* — and **omits choices from both**. Since §17.11 and §35.10 make `default(C)` a compile error for **every** choice type, a non-nullable choice-typed field always requires an explicit initializer; §7.9's text simply never says so.

This is an **omission in an enumeration, not a wrong verdict**: §17.11 governs, and the compiler follows §17.11. The property fixes it structurally — there is no enumeration left to omit a kind from — and this proposal records it as the by-product it is. The omission also **extends by recursion**, which the enumerations equally never say: since a choice-typed component is never defaultable, a **record with a choice-typed field, and an inline array with a choice-typed element type, are not defaultable either** — §7.9's negative list names non-nullable *reference* components only, while the recursive clause of the drafted row ("a value type with a non-defaultable component") covers the choice-component case by construction. **Verified at the snapshot and re-verified at v0.256** ⧗; no third carrier states the choice case for §7.9's rule.

### 4.5 Retired synonyms, recorded

The changelog entry records these as retired spellings of one property, so a reader of pinned history can map them:

| Retired spelling | occurrences ⧗ | where |
|---|---|---|
| the "well-defined default" family — "well-defined default (value)", "has no well-defined `default`", "`default(T)` is (not) well-defined", "its `default` is well-defined" | 11 | §7.9 ×2 sites (4 occ), §9.8 ×1, §14.2 ×2 sites (3 occ — incl. the "has no well-defined `default`" clause the snapshot's narrower patterns missed), §17.7 ×1 (the `Guid`-satisfies-`new()` sentence — likewise missed), §17.11 ×1 (the closing rule-2 sentence), Appendix C.4 ×1 (the `new()` row — likewise missed) |
| "well-defined zero value" | 2 | §17.11's record and inline-array bullets |
| "zero-initializable" | 3 | §15.1 ×2, §17.7 |
| "zero-initialize(s\|d)" (prose and fenced-comment text in §15.1's examples) | 5 | §15.1 (1 prose + 4 inside the two fences of §7 item 5) |
| "no principled zero" | 3 | §17.11's choice bullet, §35.10, rationale §9.6 |

**How the sweep must run.** The snapshot swept by three exact phrases and missed three sites (§14.2's "has no well-defined `default`", §17.7's "its `default` is well-defined", C.4's `new()` row) — the phrase family conjugates. The train's sweep is therefore the **single pattern `well-defined`, with hand-classified residue**: after the train, the pattern's spec/rationale/stdlib hits must contain **zero** predicate uses; the surviving residue is the ordinary-English uses, measured at v0.256 as **10 occurrences** ⧗ (spec 6 — §2.8.4's assembly semantics, §10.5's enum-box wiring, §15.4's "well-defined empty value", §22.12's exit-disposal order, §30.13's execution-domains sentence, Appendix D's "well-defined inner language"; rationale 2 — §5.15, §9.13; stdlib 2 — §3.3, §4.14 `Span<T>`), none of which reads the predicate.

"Default initialization" survives as §7.9's **subsection heading** — it names a mechanism, not the predicate — and the §17.11 bullet lists survive as the operator's *value* table, which is what §17.11 is for.

### 4.6 Migration count (B5)

**21 edit sites, 26 occurrences** ⧗, distributed: spec 20 (§7.9 ×2, §9.8 ×2, §14.2 ×2, §15.1 ×3 — the prose sentence and the two fences of §7 item 5 — §17.7 ×2, §17.11 ×4, §22.7, §35.10, Appendix C.1 ×2 — the two `default(T)` rejection rows, which carry no retired spelling but cite the property — and C.4), rationale 1 (§9.6). One new §3.6 row (bringing the table from ten properties to eleven). No stdlib site, no guide site, no test edit. `defaultable` appears **once in `src/` — and that once is a shipped diagnostic string, not a comment** ⧗: `src/sema/TypeResolution_Checks.cpp:7501`, the §9.8 conformance diagnostic (*"mark the payload `serialized skip` (if defaultable), …"*) — the compiler already speaks the property's name to users, which strengthens the case for registering it (§4.3). `zero-initializable` appears zero times in `src/` and `tests/`.

---

## 5. Corpus evidence (both parts)

- **Documents:** §3.6 = 816 words ⧗ / 10 property rows; §35.4 = 943 words ⧗; §17.11 = 487 words ⧗. B4 touches 24 rename sites (51 occurrences) plus 20 verb respellings and 6 slips ⧗ (§3.7); B5 touches 21 edit sites (26 occurrences) plus one new §3.6 row ⧗ (§4.6).
- **Compiler and tests, read-only:** B4 costs **zero diagnostic-string edits** (§3.4) and retires two live homonyms from the shared vocabulary. B5 costs zero code edits — and the one `src/` occurrence of `defaultable` is a **shipped diagnostic already naming the property** (`src/sema/TypeResolution_Checks.cpp:7501` ⧗, §4.6): the registration makes the spec catch up with the compiler's own vocabulary, not the reverse. Comments in `src/`/`tests/` drift to the new names when their code is next touched, per the ordinary `// per §…` convention — the ratchet is that no *new* comment or diagnostic uses a retired spelling.
- **Stdlib:** untouched by both parts — the Q1 subject split (§3.6) keeps its three verb sentences as ordinary English, and its one "borrow-carrying" occurrence is a §3.5 slip, respelled to *borrow-bearing record* (§3.4, one sentence).

## 6. Migration and compatibility (changelog house categories)

- **Newly rejected / newly warned:** nothing.
- **Newly defined:** `defaultable`, as a §3.6 derived type property — a name for a predicate five sections already state and two sections already use by name.
- **Renamed:** the §35.4 census → **holds-borrows**. No rule keys on the name; §3.6 rule 3 (public-contract) is unaffected because the *property* is unchanged, only its spelling.
- **Corrected in passing:** §7.9's *Default initialization* enumerations omitted choices (§4.4); six `borrow-carrying` label slips (§3.5) ⧗. Neither changes a verdict.
- **Newly stated (not newly true) — the registration's interface-classification consequence:** attaching §3.6 rule 3 to `defaultable` **classifies a defaultable-flipping declaration edit as a breaking interface change** (§4.3) — adding a non-nullable reference field to a record, or removing an enum's zero constant, was always observable through `default(T)`/`new()`/§7.9 at use sites, but the spec never *recorded* the edit as breaking. What migrates is the recorded classification of such edits, not any program: no program compiles differently.
- **Runtime behaviour:** unchanged everywhere.
- **Doc-only:** all of it. **Zero semantic migration** — with the rule-3 consequence above stated rather than smuggled: the one thing that changes beyond spelling is a contract *classification*, and it records current practice.

## 7. Mechanical validation, as a landing condition

1. **`scripts/check_docs.sh`** (full) green before and after.
2. **Vocabulary sweeps** over the nine normative documents after the train:
   - `census` → returns **only** rationale §8.2's §19.6 homonym (1 occurrence). Any other hit is an unmigrated site.
   - `borrow-carrying` → returns **only** §9.7's field/value predicate (15 ⧗ — §35.11's payload clause and §35.3's two value-level reads included) and §28.3's frame descriptor (4) = 19 occurrences. The 16 census-sense and 6 slip occurrences are gone.
   - `zero-initializ` → returns **zero** occurrences.
   - `well-defined` → the **single pattern, hand-classified** (§4.5 — exact-phrase sweeps provably under-catch: three sites conjugate the family): zero predicate uses; the surviving residue is the 10 ordinary-English occurrences §4.5 lists (re-measured at the train's HEAD), each hand-confirmed as not reading the predicate.
   - `holds-borrows` / "holds borrows" → the union of the sites §3.7 counts, plus §3.6's 20 verb respellings; the 6 kept "carries borrows" (§3.6's non-choice subjects) still return under a `carr(y|ies) borrows` sweep, each hand-confirmed non-choice.
3. **§3.6 table invariant:** eleven rows; every row's *Defined in* column resolves to a section that contains the property's definition; **no two row names share a hyphenated component**. (This is the check that would have caught the collision when it was introduced, and it is cheap enough to add to `check_docs.sh` — put to the coordinator as **Q4**.)
4. **The §3.6-rule-4 diagnostic-sentence test:** for each of the two properties, write the diagnostic sentence the rule demands and confirm it is a grammatical clause naming the property in the table's words. `holds-borrows` → *"`Loaded<Response>` holds borrows at this instantiation."* `defaultable` → *"`Status` is not defaultable: a choice type has no default (§35.10)."*
5. **Fragment extraction before and after.** `scripts/extract_doc_examples.py` is run twice — over the pre-edit snapshot, materialized as a git worktree of the train's base revision, and over the post-edit tree — into two scratch trees under `/var/tmp/bezel-scratch`, and the two fragment sets are diffed. §15.1's `zero-initialized` appears inside fenced comment text in **two fences** ⧗ — the declaration example (`// 16 zero-initialized bytes`, `// 9 zero-initialized floats`) and the `Vertex` example (`// 3 zero-initialized Vertex records (if Vertex zero-initializes)`) — so **exactly those two fences must change**, they are the **only two permitted fragment diffs**, and both must be comment-text-only. Any other diff is a defect. The train states both explicitly in the changelog entry.

## 8. Alternatives, costed

- **Alt A — rename only (B4), leave `defaultable` unregistered.** *For:* half the surface; B5's predicate is already correct everywhere, only redundantly stated. *Against:* §17.11's own closing sentence already invokes "the §3.6 rule 2 pattern" for a property §3.6 does not list, and §22.7 already uses the word normatively — the register is already inconsistent with the text, and every future `default`-touching rule adds a sixth spelling. **Not recommended.**
- **Alt B — register `defaultable` only (B5), keep the census name.** *For:* B5 is the lower-risk half. *Against:* §3.6's table is the artifact both items edit, and the collision is *in that table*; adding an eleventh row beside two colliding rows is the worse table. **Not recommended.**
- **Alt C — decline B4; add a disambiguating sentence to §3.6.** *For:* one sentence, zero churn. *Against:* the sentence already exists — twice, one in each colliding row — and the collision survives it. A third statement of "these two are different" is evidence the names are wrong. **Not recommended.**
- **Alt D — rename `borrow-bearing` instead, keeping "borrow-carrying (the census)".** *For:* symmetric solution to the same collision. *Against:* measured, `borrow-bearing` has **144 occurrences across the normative documents, 244 in `src/`, and 394 in `tests/`**, of which **55 `src/` sites are diagnostic-string fragments**; `borrow-carrying`'s census sense has 16 normative occurrences ⧗ and **zero** diagnostic occurrences. Renaming the 144-occurrence, diagnostic-bearing term to fix a collision the 16-occurrence term causes is the expensive direction. **Not recommended**, and the measurement is why.

## 9. Questions (drafting seams only — the rulings behind this proposal are settled)

- **Q1 — the verb (§3.6).** Respell the **20 choice-subject** "carries/carry borrows" occurrences to "holds/hold borrows" so name and prose are one string (recommended), or keep the existing verb and let the property name differ from the prose? The 6 non-choice-subject occurrences keep "carries borrows" as ordinary English under either answer (§3.6's subject split — respelling them would misapply the property's name), and the stdlib stays untouched as §5 states.
- **Q2 — §35.4's paragraph heading.** The census's defining paragraph is headed **"The borrow-carrying census (v0.252, #3110)"**. Becomes **"Holds-borrows (v0.252, #3110)"** (recommended — the heading names the property), or **"The held-borrows property"**? Note that proposal 0093 §4 restructures this same paragraph census-first; the §35.4 single-edit guard (§10, mirroring 0093 §10 item 3) makes the two edits one edit.
- **Q3 — `defaultable` and nullability (recorded reading, not an open question).** The drafted row makes every nullable type `T?` defaultable (its default is `null`) — spanning nullable *reference* and nullable *value* types alike. This is not a choice this proposal makes; it is what the two carriers jointly say today: §17.11's *"Reference types: `null` if the location is nullable"* and §7.9's *"nullable reference fields are implicitly initialized to `null`"*. The row **records** that reading; the train's job is to verify the two carriers still say it at its HEAD, not to rule anew.
- **Q4 — a `check_docs` extension.** Add §7 item 3's §3.6 table invariant (no two property names sharing a hyphenated component; every *Defined in* resolving) as a mechanical check, in the shape proposal 0087's Q3 check landed (recommended)?
- **Q5 — where B5's by-product correction is recorded.** The §7.9 choice omission (§4.4) is stated in the changelog entry as a correction (recommended), or filed as its own defect issue first and corrected on the defect train? It changes no verdict, which argues for the first.

## 10. Graduation-train scope

**Part I — the rename.** Order: §3.6's row first (the definition of record), then §35.4, then the remaining downstream sites (§3.7's distribution ⧗).

> **The §35.4 single-edit guard (post-gate ordering ruling; mirrors 0093 §10 item 3).** §35.4 is edited **once, ever**, across proposal 0093 and this one. When the two trains share a revision, 0093's census-first restatement lands written directly in this proposal's name and this train's §35.4 step is subsumed by it; when 0093 graduates alone first, 0093 **skips §35.4 entirely** and this train executes the census-first restatement (0093 §4.2) and the rename **in one edit**. Under neither ordering does §35.4 get edited twice.

- **spec §3.6** — rename the row; delete the two cross-row disambiguating clauses in the **borrow-bearing** and **choice classification** rows (they exist only because the names collide, and after the rename they say nothing a reader needs).
- **spec §35.4** — per the guard above: the defining paragraph and heading (Q2); the "A borrow-bearing choice always carries borrows…" sentence; the move-only paragraph's borrow-carrying-owning-choice clause.
- **spec, downstream** ⧗ — §9.7's runtime-managed cross-reference paragraph; §15.3's move-only field-placement rule; §22.16's `old(E)` paragraph (the fused site, §3.5); §28.3 (*The consume-site freeze* live-local clause, **K5.5**, the *Producers* move-only row — and, **checked, not renamed**: the two *Deferred-activation* frame-descriptor paragraphs); §30.1's three async rules; §30.14's sendability rule; §35.3 ×2; §35.11's payload clause (**checked, not renamed** — the field-level predicate); §35.12's transfer-closure clause. (The earlier draft's §28.5 entry is struck — zero occurrences ⧗.)
- **Appendix C** — C.1 (the boxing row's inline-array slip), C.3 (the choice-census row; the static-field row — its census reference and its inline-array slip), C.6 (the async-parameter row, the suspension-point row).
- **rationale** — §3.14's v0.252 choice record (7 census-sense occurrences in one paragraph ⧗); the two `borrow-carrying record` slips (§5.15's v0.188 retirement record; §15's Strings FFI sentence) ⧗.
- **stdlib** — §3.4's `borrow-carrying record` slip → *borrow-bearing record* ⧗. One sentence.
- **Guide 04** — §5 *Domains share nothing* (one census reference, in the sendability paragraph).

**Part II — `defaultable`.** Order: §3.6's new row and §17.11's definition first, then the reductions.

- **spec §3.6** — add the row (ten properties → eleven).
- **spec §17.11** — the home: one definition sentence naming the property; the existing per-kind value list stays as the operator's value table; the rejection list becomes the property's negative half; the closing §3.6-rule-2 sentence now cites a registered property.
- **spec §7.9** — *Default initialization*'s two enumerations reduce to the property (§4.3); the choice omission is thereby fixed (§4.4); the `var`-declaration paragraph reduces to one sentence.
- **spec §15.1** — *Initialization* retires *zero-initializable*; the fenced example comment respells (the one permitted fragment diff, §7 item 5).
- **spec §17.7** — the `new`-operator null-fill sentence.
- **spec §14.2** — the `new()` table row and its prose enumeration; the choice sentence becomes a consequence.
- **spec §9.8** — `serialized skip`'s "no well-defined default" → "not defaultable"; the wire-shape sentence's existing `defaultable` now resolves.
- **spec §22.7** — the watermark sentence's existing `defaultable` now resolves.
- **spec §35.10** — the choice bullet cites the property.
- **Appendix C** — C.1's two `default(T)` rows, C.4's `new()` row.
- **rationale** — one record sentence for the unified property and the retired spellings (§4.5's table is the record's content).

**Changelog:** one entry covering both parts, doc-only, carrying §4.5's retired-spelling table, §3.7's and §4.6's counts, the §4.4 correction, the §3.5 slips, and the §7 sweep results as the acceptance record.

**Compiler:** none required. The ratchet (§5) is that no new comment or diagnostic uses a retired spelling.

## 11. Interactions

- **Proposal 0093** (same basket) restructures §35.4 census-first and restates K5's criterion, whose clause 5 names the census. Governed by the **§35.4 single-edit guard** (§10; post-gate ordering ruling): one §35.4 edit ever — written in this proposal's name when the trains share a revision, executed wholly by this train otherwise. 0093's §4.2 draft carries the matching bracketed note and conditional scope entry.
- **Proposal 0094** (same basket) caps Appendix C; C.3's choice-census row and C.6's two census rows are in both scopes. **Ruled program order: 0093 → 0095 → 0096 → … → 0094 (re-derived)** — this proposal graduates *before* 0094's train, so this train respells the rows as they stand and 0094's later cap writes the capped rows in the new name directly.
- **Proposal 0096** (iterable announcement unification) — no contact with the census, `defaultable`, or any B4/B5 vocabulary surface **except Appendix C.3**, which is a shared edit surface of five accepted proposals (0093, 0094, 0095, 0096, 0101). Composition rule, per the program order: 0093 first, 0096 before 0094's stage-2, this proposal's rename at its own train — and **each train re-reads C.3 at its own HEAD** rather than trusting any proposal's snapshot of it.
- **Proposal 0088** reserves `sourced` as a possible return-position marker; §3.3 records that this is why `sourced` was not a rename candidate.
- **Proposal 0089** (graduated v0.245) retired §19.6's determination rule; its "census prohibition" is the homonym §3.4 measures in `src/` and `tests/` and the one occurrence §7 item 2 expects to survive in the rationale record.
- **Proposal 0087** (statics vocabulary, graduated v0.245) is the precedent for both halves: one property derived once, the curated enumerations reduced to reads of it, a mechanical check added with the train. §7 item 3's proposed `check_docs` extension is that pattern (Q4).
