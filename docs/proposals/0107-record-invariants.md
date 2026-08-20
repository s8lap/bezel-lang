# Proposal 0107 — Constructor-only records: closing the record fabrication hole

| | |
|---|---|
| **Status** | under-review — **redesigned draft (2026-08-20)**, replacing the design scoped into v0.264 and withdrawn from it before landing. The withdrawn design is recorded below at [§12](#12-revision-record) and [§13](#13-revision-record--the-v0264-withdrawal), quoted forward unchanged (their internal §-references are to the withdrawn text, which this file no longer carries); it is the reason this proposal exists in its present form and is not re-litigated here. **This redesign proposes no `invariant` member in record bodies.** §22.16's out-of-scope deferral for record invariants, and `DECLINED.md`'s *Record invariants* row, are deliberately left **standing**. |
| **Tracking issue** | [#3282](https://github.com/s8lap/bezel-lang/issues/3282) (redesign tracker). [#3281](https://github.com/s8lap/bezel-lang/issues/3281) — the blocker #3282 records — is **closed and ruled** (v0.265); this redesign has no contact with it (this proposal's §5.9). |
| **Target module(s)** | none — **spec-only, and deliberately so**. The normative surface is one new short section (§9.9), one row and one clause in §3.6, two clauses in §17.11, one paragraph in §9.5, two sentences in §9.8, one row in §2.8.6, and four Appendix C lines. **No grammar production changes and Appendix A is untouched** (this proposal's §5.10); **no stdlib document changes** (this proposal's §6, and §8(g) for the stdlib-amending alternative declined there); no guide changes (this proposal's §9). |
| **Written against** | spec/stdlib/rationale/changelog **v0.265** (HEAD `03c3ffc`). Every quotation was re-verified against that tree — not carried from the withdrawn draft, which was written against v0.262. |
| **Currency** | v0.265 (`03c3ffc`). **Quoted:** spec §2.8.2, §2.8.6, §3.6, §7.9, §9.1–§9.5, §9.7, §9.8, §14.2, §17.11, §22.3, §22.5, §22.7, §22.8, §22.12, §22.15, §22.16, §23.1, §27.8, §31.4, §32.13, Appendix A.3/A.4, Appendix C.1/C.5/C.8/C.15; stdlib §1, §2.12, §2.15, §4.14; process §2, §9. **Cited without quotation:** spec §5.1, §8.6, §9.6, §13, §15.1, §15.1.3, §19.3, §19.6, §21.10, §30.14, §32.5, §32.10, §35.4, §35.10, §35.11. Re-verification before any ruling must cover §3.6, §9.5, §9.8, §17.11, §22.16, and Appendix A.3. |
| **Origin** | Slate item **i** of [proposal 0106](0106-language-lessons-survey.md) §9 (*"Record invariants + non-defaultability"* — its *"first-rank finding"*), opened by owner direction 2026-08-20. The first design was withdrawn the same day after three adversarial rounds ([§13](#13-revision-record--the-v0264-withdrawal)); this is the redesign #3282 asks for, answering its Q1 (frame), Q2 (the constructor interval), and Q3 (whether the instrumented invariant is the right mechanism at all). |

*Reference convention: unprefixed §-references are **language-spec** references at v0.265; **stdlib**, **rationale**, and **process** references are marked as such. References to this document's own sections are always written "this proposal's §N" — its self-numbers are all real spec sections, so a bare §N is never a self-reference. 0106's sections are cited "0106 §N". Quotations are verbatim; **bold inside a quotation is the source's own**; an ellipsis marks every truncation. **▲** marks candidate surface that exists in no landed document; anchored links (`#sec-…`) appear only inside ▲ blocks, where they are the anchors the text would carry once pasted into the spec.*

---

## 0. What this proposes, in one page

**One derived type property, two consequences, one declaration rule.**

**(a) Mint `constructor-only` as a §3.6 derived type property**, defined in a new §9.9: *a record that declares a constructor carrying a contract clause — `requires` or `ensures` ([§22.16](#sec-contract-clauses)) — is **constructor-only***. Computed from the declaration; never annotated.

**(b) Consequence 1 — such a record is not defaultable.** One clause on §3.6's *defaultable* row and two in §17.11. This closes, with no further text, every fabrication route 0106 named plus four this proposal found: `default(R)`; implicit default initialization of `R`-typed storage (§7.9); `[N]R` default slots (§17.11, §15.1); `new T()` under `where T: new()` (§14.2); stdlib §1's Try shape (c); §9.8's `serialized skip`; stdlib §4.14's one-argument `OwningBuffer<R>(length)`; and §22.5's `default(T)` default-value form. §9.8's `serialized optional` needs one companion sentence (this proposal's §5.4).

**(c) Consequence 2 — `with` over such a source is a compile error.** §9.5 states that `with` *"copies field values directly; it does not invoke the record's constructor"* — so `with` is the one remaining language-level producer of a record value that met no constructor. Rejecting it is what makes the property's claim true rather than nearly true, and it is not new policy: §9.5's own next sentence already prescribes the remedy for exactly this population.

**(d) One declaration rule — a record declaring positional fields may not declare a clause-bearing constructor.** §9.3's auto-generated primary constructor can carry no clause (A.3's `RecordDecl` admits no `ContractClause`, exactly as `ClassDecl` admits none on a class primary constructor), so the combination would flip a client-visible property on the strength of one door while leaving the type's most natural door unvalidatable. A compile error at the record declaration, naming two remedies.

**What the property claims, exactly — and what it does not.** *Every value of a constructor-only record is produced by a call to one of its declared constructors, or is a copy of such a value.* It is a **provenance** property, not a validity property. It does not claim that any predicate holds of any value at any instant; it does not read what a constructor's clauses say, whether every constructor declares one, or whether the clauses relate arguments to components. This sentence is normative and is the reason the design survives attack; it is stated in §9.9's own text, not left to a reader.

**What this does *not* propose, and why it matters.** No `invariant` member in record bodies. The instrumented design is recorded as **declined-with-reasons** at this proposal's §3, with the five forges that killed it. §22.16's deferral stands, unstruck.

**Cost, stated honestly.** A record that adopts a constructor clause loses `default`, `with`, `where T: new()`, Try shape (c), `serialized skip`/`optional`, `OwningBuffer<R>(length)`, and the `default(T)` optional-parameter form — and there is no in-constructor way to opt out (this proposal's §7.2). The positional form §27.8 prescribes becomes unvalidatable (this proposal's §7.3). Landed reclassification: **zero**, verified four ways (this proposal's §7.1).

---

## 1. The defect

### 1.1 The prescribed shape, and the half it cannot state

§27.8 prescribes the carrier: *"For genuinely distinct types (`UserId` and `OrderId` should not be interchangeable despite both being `int`), use a single-field record"*.

§9.5 removes the constructor's guarantee for any record whose storage is externally nameable: *"`with` copies field values directly; it does not invoke the record's constructor. A constructor that validates cross-field invariants (e.g. `Width > 0 and Height > 0`) is not re-run during `with`."*

And §17.11 removes it again, from the other side: *"Records all of whose declared fields are defaultable: the record value with each field at its zero value."* A `Port` whose validity is `0 <= V <= 65535` is defaultable because `int` is, so `default(Port)` is a `Port` that met no constructor.

So the type the spec tells you to write is the type whose one guarantee the spec then falsifies — 0106 §5.3's finding, in its words: *"**Bezel's own recommended newtype shape cannot state its own validity**"*.

### 1.2 The half the language already has

What it does have, and what the withdrawn design overlooked while building a new mechanism beside it, is **constructor-exit checking on records, today**. §22.16 is unambiguous:

- Scope admits *"constructors"* and *"record and enum methods"*; only the `invariant` member is class-only.
- *"**`ensures`** clauses evaluate on every **normal-completion path** — … **a constructor body's completion** …"* and *"on a constructor, `ensures` evaluates at body completion, where every [§7.9](#sec-definite-assignment-for-non-nullable-fields)-required field is assigned"*.
- An `ensures` clause *"may read … `this`, fields, statics, and plain parameters as borrows"* (`result` and `old` are compile errors in constructor clauses).
- §22.15 classifies *"an **auto-property getter**"* as pure, and §22.16's clause typing admits a call to a pure member — so `ensures(Value >= 0 and Value <= 65535)` on a record constructor is admissible clause text.

That is precisely the *"constructor-exit"* half of §22.16's own deferral — *"record invariants (constructor-exit plus a `with`-update instrumentation point — a later design)"* — and it is **landed**. The half that is missing is not an instrumentation point. It is that the type has other producers, and the language does not know that some records should not have them.

### 1.3 The producers, and why the list is closed

§9.8 enumerates the constructor-bypassing constructions itself, in one sentence, while describing its own: a record with no explicit constructor *"is constructed directly from the read components at the storage level — the same constructor-bypassing construction that `with` ([§9.5](#sec-non-destructive-update-with)) and `default(T)` ([§17.11](#sec-the-default-operator)) already perform"*.

**Three.** Everything else that produces a record value is a constructor call or a copy of an existing value (assignment, argument binding, return, an `old` capture, a collection-slot store, boxing and unboxing, a pattern's field read — §19.3's patterns *destructure; they never construct*). Every site in the language that supplies a `default(T)` is reachable from §17.11's own catch-all — *"any other site that supplies a `default(T)`"* — and the sites were enumerated for this proposal rather than assumed (this proposal's §4).

This is the structural difference from the withdrawn design, and it is why the redesign's soundness argument is a paragraph where the withdrawn one was three failed rounds. The withdrawn §2.3 quantified over *"every route by which an invariant-bearing record's storage can come to hold a value"* — every store, escape, and alias, a set the compiler does not enumerate, falsified by exhibiting a sixth ([§13.2](#132-round-2-forge-a--the-constructor-escape-and-why-227s-gate-is-vacuous-here), [§13.3](#133-round-2-forge-b--readonly-storage-is-assignable-in-any-constructor-of-the-declaring-record)). This proposal quantifies over **producers**, and the spec wrote that list itself.

---

## 2. The design

### D1 — `constructor-only`, a §3.6 derived type property ▲

> ▲ A record type is **constructor-only** — a derived type property ([§3.6](#sec-derived-type-properties)) — when it declares a **constructor carrying a contract clause** ([§22.16](#sec-contract-clauses)): a `requires` clause, an `ensures` clause, or both, on any constructor of the record, of any visibility. The property reads the declaration only: it does not read what a clause says, whether every constructor declares one, or the clause's enforcement tier. It is constant across the instantiations of a generic record, no substitution being able to add or remove a clause.

**Why the clause is the trigger.** The language has no other structural signal that an author intends a record's construction to be a checkpoint, and §3.6's closing rule forbids inventing one beside the mechanism: *"New rules added to this specification that need a structural classification of types must define it as a derived type property under the rules of this section, rather than introducing a parallel mechanism."* A constructor clause is the author's own statement that this constructor is a gate.

**Why both clause kinds.** They state different things and an author needs both spellings. A `requires` constrains **arguments** — *"a **constructor's** `requires` may not observe `this` … it reads parameters and statics only"* (§22.16) — and is **always-on**: *"an unproven `requires` clause is compiled as a runtime check in every build mode; it is never elided"*. An `ensures` constrains the **constructed value**, at body completion, and runs under checked-contracts mode: *"in **unchecked (release) mode** — the default — unproven `ensures` and `invariant` clauses are not evaluated"*. For a newtype wrapper the honest clause is usually `ensures`, because it says what the *value* is; the mode difference is real and is stated in §9.9's text so an author chooses knowingly rather than discovers.

```bezel
public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0 and value <= 65535)     // always-on; constrains the argument
    ensures(Value >= 0 and Value <= 65535)      // checked-contracts mode; constrains the value

    Value := value;
  end constructor;
end Port;
```

*(The parameter is spelled `value`, not `v`, deliberately: §9.8's synthesized `Deserialize` matches constructor parameters to components by name, *"compared **case-insensitively**"*, so `value` binds `Value` and the type stays serialization-eligible.)*

### D2 — What the property claims, normatively ▲

> ▲ Every value of a constructor-only record is produced by a call to one of that record's declared constructors, or is a copy of such a value. This is a claim about **provenance**, not about validity: the property does not assert that any predicate holds of any value at any instant, and a clause's truth at construction is not carried forward. In particular, the property does not prevent a constructor from producing a value its own clauses do not describe, does not restrict what the declaring record's own code may write ([§22.8](#sec-field-initializers)'s and [§23.1](#sec-auto-properties)'s `readonly` assignable context is a **constructor body**, not an instance), and does not reach values produced or altered outside the language ([§32.10](#sec-boundary-contract)).

This paragraph is normative and load-bearing. It is what a reader of §17.11's diagnostic will consult, and it is the difference between a design that survives attack and one that invites it — the withdrawn design's completeness claim was falsified three times by programs a reader would have believed impossible ([§13](#13-revision-record--the-v0264-withdrawal)).

### D3 — Consequence 1: not defaultable ▲

§3.6's *defaultable* row, amended (additions in **bold**):

> ▲ **defaultable** — *"… an enum **declaring a zero-valued constant**, a record all of whose declared fields are defaultable **and which is not constructor-only ([§9.9](#sec-constructor-only-records))**, an inline array whose element type is defaultable … Not defaultable: non-nullable reference types; **every choice type** ([§35.10](#sec-choice-default-with)); an enum with no zero-valued constant; **a constructor-only record ([§9.9](#sec-constructor-only-records))**; and any record or inline array with a non-defaultable component"*.

§17.11, correspondingly — one clause in the records bullet and one entry in the compile-error list, spelled on the model of the enum entry already there:

> ▲ *"Records all of whose declared fields are defaultable **and which are not constructor-only ([§9.9](#sec-constructor-only-records))**: the record value with each field at its zero value. …"*
>
> ▲ (compile-error list) *"A **constructor-only** record ([§9.9](#sec-constructor-only-records)). Every value of such a record is produced by a call to one of its declared constructors, and `default(T)` calls none; a fabricated zero value would be a value of the type with no constructor behind it. For such a record, `default(R)`, `new T()` at such a substitution ([§14.2](#sec-constraints)), default initialization of `R`-typed storage ([§7.9](#sec-definite-assignment-for-non-nullable-fields), [§15.1](#sec-inline-arrays)), a default-value expression spelled `default(R)` ([§22.5](#sec-optional-parameters-and-named-arguments)), and any other site that supplies a `default(T)` ([§9.8](#sec-serialization-synthesis)'s `serialized skip` and `serialized optional`) are rejected. The remedy is a constructor or factory call — or, where a zero value is genuinely wanted, `R?` and `default(R?)`. The property claims provenance, not validity ([§9.9](#sec-constructor-only-records))."*

The second paragraph is a near-transcription of §17.11's existing enum paragraph. The enumeration of affected sites is not new work: **the spec has already written it once**, for the structurally identical case.

### D4 — Consequence 2: `with` is rejected ▲

> ▲ (§9.5, appended to the paragraph beginning *"`with` copies field values directly"*) *"A `with` expression whose source expression's static type is a **constructor-only** record ([§9.9](#sec-constructor-only-records)) is a **compile error** at the `with` expression, naming the property and the source type. Such a record's values are produced by its declared constructors; `with` invokes none, so the copy it produces would be a value of the type with no constructor behind it. The diagnostic names the remedy this section already recommends: a named method for constrained updates, whose body constructs the result. The rejection is unconditional and independent of [§13](#sec-visibility) accessibility — it reaches the declaring record's own code, where every target is accessible."*

**Why rejection and not instrumentation.** The withdrawn design instrumented the `with` point because it had a runtime predicate to evaluate there. This design has none, and there is no honest third option: leaving `with` standing makes D2's sentence false, and §22.16's mode rules would have put any check it *could* run out of the default build in any case. Rejection also retires a rule rather than adding one — §9.5's advice paragraph becomes enforced for the population it addresses instead of remaining a sentence a reader may miss.

**Why unconditional rather than external-only.** §9.5's accessibility rule (*"A `with` target must be **accessible at the use site under the member's own declared accessibility**"*) already lets an author block external `with` by making storage `private`. That workaround is no longer needed for a constructor-only record, and an inside-the-record `with` produces a value with no constructor exactly as an outside one does. One rule, no per-site reasoning.

### D5 — The positional door is closed at the declaration ▲

> ▲ (§9.9) *"A record declaring **positional fields** ([§9.1](#sec-positional-records), [§9.3](#sec-combined-form)) may not declare a constructor carrying a contract clause: a **compile error at the record declaration**. The auto-generated primary constructor can carry no clause, so the combination would leave the record's generated constructor — the form `new R(…)` most naturally reaches — outside every gate the declared constructor states, while flipping a property clients depend on. The diagnostic names two remedies: declare the storage as auto-properties in a body record ([§9.2](#sec-body-records)) so every constructor is declared, or state the condition on a **static factory method** ([§22.16](#sec-contract-scope) admits clauses on static methods) where the type is not meant to be constructor-only."*

**The ground, and its symmetry.** A.3: `RecordDecl ::= Visibility? "record" Identifier GenericParams? PositionalFields? Inheritance? GenericConstraints* ( ";" | RecordBody )` — no `ContractClause`. `ClassDecl` carries none on `PrimaryCtorParams` either. Neither kind's primary constructor can carry a clause, so this rule adds no asymmetry between records and classes; it makes visible, at the declaration, a combination that would otherwise be silently ineffective at the type's main door. The grammar-slot alternative that would remove the residual is declined at this proposal's §8(c) and put to the owner as Q4.

### D6 — Validation stays on the constructor; no new construct

The design adds no clause kind, no member kind, no modifier, and no type kind. An author states the type's validity with the clauses §22.16 already admits on a constructor, at the point §22.16's own deferral already named. What the design supplies is the missing half: that such a record has no other producers.

### D7 — Companions on the serialization path

Two, both stated at this proposal's §5.4 and §6: §9.8's `serialized optional` defaultability guard (a repair of a **pre-existing** gap, owed regardless of this proposal), and the decode band for a contract clause answering `false` inside the synthesized `Deserialize`.

### D8 — What the design deliberately does not restrict

No restriction on writable storage at any visibility; no restriction on `this` observation in constructors; no restriction on borrow-bearing records; no clause-frame restriction. Each is declined with its reason at this proposal's §3.

---

## 3. What is **not** proposed

This section is the substance of the redesign, not its margin. The design the project spent three adversarial rounds on is recorded here as **declined-with-reasons**, so that reviving it requires new evidence against the stated reasons rather than a fresh enthusiasm.

**(1) `invariant` members in record bodies — declined.** The withdrawn design's (a). Its soundness rested on an *instrumentation-completeness* claim — that the instrumentation points plus a copy are the only ways a value of an invariant-bearing record comes to exist. That claim was attacked in three independent rounds and each round found a new admitted forge or reopened a closed one ([§13](#13-revision-record--the-v0264-withdrawal)). Five forges are on file and every one is still live at v0.265:

- **Round 1** ([§13.1](#131-round-1--non-public-writable-storage)) — a `private` settable auto-property written from a **static factory of the declaring record**, where §13's visibility rules make the private member fully accessible. Three variants of the same shape. Repaired by barring writable storage at any visibility; rounds 2 and 3 went past the repair.
- **Round 2, forge A** ([§13.2](#132-round-2-forge-a--the-constructor-escape-and-why-227s-gate-is-vacuous-here)) — a copy of `this` escaping from inside the constructor, legal **from the first statement**: §22.7 rule 1 gates on *"every field of the declaring class that [§7.9] requires to be explicitly initialized"*, and §7.9's *Default initialization* subsection makes that set **empty** for an all-primitive record.
- **Round 2, forge B** ([§13.3](#133-round-2-forge-b--readonly-storage-is-assignable-in-any-constructor-of-the-declaring-record)) — a write to another `R` value's `readonly` storage inside any constructor of `R`: §22.8 says *"in a constructor body"*, §23.1 says *"in a constructor"*, §22.3 says *"outside its assignable context"* — all **positional**, none per-instance. A retroactive falsification of a fully constructed, already-checked value.
- **Round 3** ([§13.4](#134-round-3--the-97-amendment-reopened-the-store-it-was-written-to-close)) — an interface-typed field escaping borrow-bearing classification, readmitting §9.4's mediating store. Filed as #3281 and **ruled at v0.265**: the predicate reads declared field types, so this door is now shut — *and the ruling's own note is why it is not enough*: declared-type at §9.7 is **necessary but not sufficient**; §35.4 had to be amended too, because without it an interface reaches the record through a choice payload. A restriction that needed two independently-defective sections repaired to become sound is a restriction whose next unaudited neighbour is the next round.
- **§13.5's frame question** — a clause reading mutable statics goes false with **no store, no copy, and no instrumentation point anywhere**. And a strictly wider form found after the withdrawal, which refutes the obvious repair: a `static readonly` **class-typed singleton** with a settable auto-property falsifies a record clause with **no mutable static in the program at all** (§22.12 rule 4 admits the declaration with a warning; §22.15 rule 1 admits the read; §22.15 classifies the auto-property getter as pure so clause rule 6 admits the call). A restriction phrased *"no static reads"* or *"no mutable static reads"* is refuted before it lands.

Closing all of that needs, before a line of it is written, a clause-admissibility rule strong enough to make the frame problem structurally impossible — every read rooted at the receiver, traversing only value-typed storage — which is a **second effect lattice** beside purity, or a curated allowlist, or the loss of `Value in 0..65535` and every `pure` helper; **and** a constructor-interval closure covering all three of forge A, forge B, and §22.7 rule 3's ungated lambda-capture route. Neither exists. And the mechanism the class rule leans on is one the record form deliberately deletes: §13.6's structural lesson — *"the class subsection's soundness rests on a **re-entry point** (public-method entry/exit re-checking) that the record form deliberately has none of"* — and restoring it does not work either, because §22.16 says *"**Getters are exempt**"* and a §27.8 wrapper record's only public member **is** a getter, so entry checking would fire nowhere on the motivating population.

**Reopening bar:** a clause-admissibility rule that closes the frame class without minting a second per-body effect fact, **together with** a constructor-interval closure that names and closes all three escape routes — not a demonstration that the syntax reads well.

**(2) Restriction (a′) — a borrow-bearing record may not declare an invariant member — not carried forward.** It exists only to bound a standing clause's frame. There is no standing clause. Its narrower expression-level variant, recorded at [§8(f)](#8-alternatives-declined) as *"the liftable refinement"*, is likewise not carried forward, for the same reason. #3282 records this redesign as blocked on #3281; the block is discharged **by removal**, not by the ruling, and the ruling stands for whoever revives the invariant.

**(3) A restriction on writable storage — declined.** The round-1 repair. Under this design it would buy **partial coverage of a guarantee the design does not make**: forge B falsifies a value through storage the repair leaves writable, and forge A escapes a value the repair never sees. A restriction that closes some of the ways a value can be made invalid, in a design that does not claim values are valid, is cost with no corresponding claim. Stated positively: the property is provenance, and provenance is unaffected by any store.

**(4) A restriction on `this` observation in a constructor — declined.** #3282's Q2 candidate. Same reason: it protects a claim this design does not make. (One incidental effect is worth recording rather than selling: D3 makes every *sink* for such an escape — a `ref` parameter, a local, a class field of type `R` — require a definitely-assigned value of `R` first, so the escape can no longer be aimed at storage that never held a constructed value. That narrows the forge; it does not close it, and nothing here depends on it.)

**(5) An FFI carve-out — declined.** See this proposal's §8(f).

**(6) Instrumenting `with` instead of rejecting it — declined.** See this proposal's §8(e).

---

## 4. The soundness argument, and what it leaves open

**Every producer of a value of record type `R`, swept and classified.** The list below was derived from the documents — every occurrence of `default(T)`-supplying text in both normative documents was read, not assumed — rather than reconstructed from 0106's four routes.

| Producer | Citation | Under this proposal |
|---|---|---|
| `new R(…)` | §22.7 | **The producer.** Its clauses evaluate per §22.16 |
| `default(R)` | §17.11 | **Rejected** (D3) |
| Implicit default initialization of `R`-typed storage | §7.9 *Default initialization* | **Rejected** — the field or local *"requires an explicit initializer"* by the same paragraph |
| `[N]R` inline-array slots | §17.11, §15.1 | **Rejected** (D3) |
| `new T()` at a value substitution under `where T: new()` | §14.2 (*"satisfies the constraint exactly when it is **defaultable**"*) | **Rejected at the instantiation site**, §3.6 rule 2 |
| A default-value expression `default(R)` | §22.5 (*"be a literal, constant, or `default(T)`"*) | **Rejected** (D3). The site stays open through §22.5's *"call static methods, including factory methods"* |
| stdlib §1 Try shape (c) | stdlib §1 (*"with `default(T)` assigned on the false path"*) | **Unwritable**; shape (a) forced, on shape (a)'s own stated reasoning |
| stdlib §4.14 `OwningBuffer<R>(length)` | stdlib §4.14 (*"For types without a default, only the two-argument constructor with an explicit init function is admitted"*) | **Inadmissible, with zero new text** |
| §9.8 `serialized skip` | §9.8 (*"whose type is not defaultable … is a compile error at the conformance declaration"*) | **Rejected, with zero new text** |
| §9.8 `serialized optional`, and a choice payload field marked `optional` | §9.8 — **the bullet carries no defaultability sentence** | **Rejected by one new companion sentence** (this proposal's §5.4). A pre-existing gap |
| §9.8's constructor-less storage-level construction | §9.8 | **Structurally unreachable.** A constructor-only record declares a constructor, and D5 forbids the combined form, so §9.8 always takes its explicit-constructor branch |
| `with` | §9.5 | **Rejected** (D4) |
| A copy — assignment, argument binding, return, an `old` capture, a collection-slot store, boxing/unboxing, a pattern's field read (§19.3 destructures, never constructs) | — | **Not a producer.** Reproduces a value whose provenance is intact |
| Foreign code | §32.13's direct admission in *"parameter, return, `ref`/`out`, and callback positions"*; §32.5 by-address; §15.1.3 inline-array by-address | **Not closed. Recorded** under §32.10's boundary contract (this proposal's §5.7, §8(f)) |

**Two residuals, stated rather than closed.**

1. **Multi-constructor selection at §9.8.** Where a constructor-only record declares two explicit constructors and the one §9.8 selects — *"the *unique* explicit constructor whose parameter list binds exactly the serialized components"* — carries no clause, the wire path constructs through an unclause-bearing door. Provenance holds; validation does not. This is the author's declaration to make, and D2's sentence already disclaims it. Not legislated.
2. **§22.7's watermark, for non-defaultable value-typed fields.** §22.7 fixes the pre-construction bit pattern for *"every reference-typed field of the allocation … to `null` and every **defaultable** ([§17.11](#sec-the-default-operator)) value-typed field to its [§7.9](#sec-default-initialization) default"* — and states none for a **non-defaultable** value-typed field. Reachable only inside §22.7's own declared residual (its rule 3's ungated closure route; the section says its checks are *"**not a completeness guarantee**"*). This is **pre-existing and independent of this proposal** — it holds today for a choice-typed field, a zero-constant-less-enum-typed field, and a record field with a non-nullable class component — and D3 adds constructor-only records to that population without changing the rule. It is not this proposal's to fix and is not fixed here; it is a §22.7 clarification worth its own spec-defect issue (this proposal's §11, *Deliberately not asked*).

**Why this enumeration is closed where the withdrawn one was not.** It ranges over **producers of a value**, a set the spec enumerates itself — §9.8: *"the same constructor-bypassing construction that `with` ([§9.5]) and `default(T)` ([§17.11]) already perform"* — and every `default(T)` site is reachable from §17.11's catch-all. The withdrawn §2.3 ranged over every way *storage can come to hold a value*, which includes every store, escape, and alias; that set is not enumerated anywhere, and three rounds found three members of it the argument had not.

---

## 5. Consequences, worked

| Site | Today | Under this proposal | Mechanism |
|---|---|---|---|
| `default(R)` | a fabricated value | **compile error** | §17.11's non-defaultable list (D3) |
| default initialization of `R`-typed storage; `[N]R` slots | implicit zero value | **compile error unless explicitly initialized** | §7.9 *Default initialization*; §17.11 — no new rule |
| `where T: new()` at `T = R` | admitted; `new T()` fabricates | **rejected at the instantiation site** | §14.2's definition + §3.6 rule 2 (this proposal's §5.1) |
| `where T: value` at `T = R` | admitted | **admitted, unchanged** | §14.2's `value` row (this proposal's §5.2) |
| `p with(Value := -1)` | admitted; no constructor, no check | **compile error** | §9.5 (D4) |
| Try shape (c) over `R` | `default(R)` on the false path | **unwritable; shape (a) forced** | stdlib §1 (this proposal's §5.3) |
| `serialized skip` on an `R` component | already an error for non-defaultables | **error, by the existing rule** | §9.8's skip guard |
| `serialized optional` on an `R` component | **unguarded — fabricates** | **error, by a new guard** | this proposal's §5.4 — new text |
| stdlib §4.14 `OwningBuffer<R>(length)` | admitted iff `R` defaultable | **inadmissible; `init`-function form required** | existing stdlib text, no change |
| `method M(p: R := default(R))` | admitted | **compile error**; `:= R.Create(…)` remains | §22.5 + §17.11 |
| positional fields + a clause-bearing constructor | admitted; the primary bypasses the clause | **compile error at the declaration** | D5 |
| synthesized `Deserialize` of `R` | constructor-less branch reachable | **explicit-constructor branch always; band ruled** | §9.8 (D5 makes the other branch unreachable) + this proposal's §6 |
| auto-equality / hashing / `ToString` / serialization eligibility | — | **unchanged, with no amendment** | §9.2 (this proposal's §5.5) |
| patterns and exhaustiveness (§19) | — | **no interaction** | patterns destructure, never construct |
| artifact interface; §2.8.6 verdicts | — | carried with no §2.8.2 change; **one §2.8.6 row owed** | this proposal's §5.6 |
| FFI (§32.13) | — | admissible; **residual recorded** over the whole position list | this proposal's §5.7 |
| borrow-bearing records | — | **no interaction; no restriction** | this proposal's §5.9 |
| grammar (Appendix A) | — | **untouched** | this proposal's §5.10 |

### 5.1 `where T: new()` — the rejection lands where §3.6 says it should

No new rule. §14.2 defines value-type satisfaction *as* defaultability; the record stops being defaultable; the constraint stops being satisfied. §17.11 already closes with the general sentence: *"A substitution that is not **defaultable** — an enum with no zero-valued constant included — is rejected at the instantiation site ([§3.6] rule 2's per-instantiation evaluation; the diagnostic names the property per rule 4)."* The structural reason rule 2 demands is one phrase: *R is constructor-only*.

### 5.2 Generics beyond `new()` — the position is already inhabited

`where T: value` is unaffected: its row admits *"Non-nullable value types — records, enums, choices ([§35]), primitives, inline arrays (all forms), the unit type"* — membership by kind, no defaultability conjunct. The cell *value-satisfying and never defaultable* is already occupied: §14.2, *"**Choices never satisfy the value-type arm** — a consequence of the property: no choice is defaultable"*. This proposal adds a second inhabitant to an occupied cell rather than opening an empty one, so every rule that had to cope with such a type has already coped. `with` needs no generic rule at all: §9.5 requires a **record-typed** source, and a bare `T` is never statically record-typed under any constraint this revision has.

### 5.3 Try shape (c) → shape (a)

Shape (c)'s definition *is* `default(T)` on the false path, so a constructor-only `R` cannot inhabit it: §22.3 requires every `out` assigned on every normal return path and there is no value to assign. The author writes shape (a) — *"`TryX(..., out item: T?): bool`"* — whose false path assigns `null`, with the `when true` postcondition narrowing in the true region. **No stdlib edit is required**, because no landed stdlib member is affected: shape (c)'s enumerated inhabitants are the primitive `TryParse` family, `Convert.TryTo*`, the `DateTime`/`DateOnly`/`TimeOnly`/`Guid` parsers, and the §4.14/§4.15 `TryRead*` accessors — every one over a type with no predicate. The forcing applies to future surfaces, in the direction stdlib §1 already reasons: shape (a) exists for payloads with *"no `default` to assign on the false path"*.

### 5.4 §9.8's `serialized optional` guard ▲ — mandatory, one sentence

§9.8's customization vocabulary has two `default(T)`-supplying members and only one is guarded. `skip`: *"A `serialized skip` component whose type is not defaultable ([§17.11]) is a compile error at the conformance declaration"*. `optional`: *"On read, a missing field yields `default(T)` instead of raising `SerializationException`"* — **and the bullet carries no defaultability sentence anywhere**.

> ▲ (in §9.8's `serialized optional` bullet) *"A `serialized optional` component whose type is not defaultable ([§17.11](#sec-the-default-operator)) is a compile error at the conformance declaration (a missing field would have no value to supply). The same holds for a choice payload field marked `serialized optional`."*

The diagnostic site is the conformance declaration, identical to `skip`'s, for §9.8's own stated reason. This is a **repair of a pre-existing gap**, not a new prohibition: the same hole exists today for an enum with no zero-valued constant and for a choice payload, and §17.11's catch-all arguably already reaches it with no diagnostic site fixed. It is owed under any design that makes another value type non-defaultable, and it is owed today. The remedy the diagnostic names is `R?`.

### 5.5 §9.2 auto-equality — a verified zero, and a trap the withdrawn design had and this one does not

The withdrawn design's §4(1) called §9.2's membership list *"a trap if omitted"*: an `invariant` member is none of §9.2's four admitted member kinds, so adding one would silently strip auto-equality **and**, through §9.8's *"exactly the auto-equality membership conditions of [§9.2]"*, serialization synthesis. **That trap does not arise here.** §9.2's list already reads *"— A constructor — regardless of what its body does"*, and a contract clause sits *"between its signature and its body"* (§22.16), not among the members. A record that becomes constructor-only stays auto-hashable, stays hashable, stays serialization-eligible, and stays a valid §19.3 constant-pattern slot type. **No §9.2 amendment is required.** Checked against §9.2's list at v0.265.

### 5.6 The artifact interface and change classification

**§2.8.2 needs no amendment.** Clause 1 already carries *"the contract clauses and `invariant` members ([§22.16]; carried as **contract records** per the paragraph below)"* and *"the **derived type properties** ([§3.6]): carried as computed verdicts for non-generic public types, and as the structural facts needed for per-instantiation evaluation … for public generics"* — a generic statement over the table, so a new row is carried by the sentence already there. The withdrawn design's §4(2) amendment (extending the *contract-record* sentence from *"for every class"* to *"for every class or record"*) is **not needed**, this proposal admitting no record `invariant` member.

**§2.8.6 needs one row ▲ — mandatory, and owed under any spelling of this design.** This was checked rather than assumed, and the naive answer is wrong. The row is not a cost of minting the property: a design stating the same condition as a plain rule owes an equivalent row, keyed on the condition instead of the name. For a record that *was already* non-defaultable — `record Name(S: string)`, `string` being a non-nullable reference type (§17.11) — gaining constructor-only produces **no** *derived-property verdict lost* delta, yet withdraws `with` from every client. The existing *"derived-property verdict gained (sendable, trivial-dispose, hashable, C-compatible)"* row's parenthetical is a closed list that does not reach it, and no other row does either.

> ▲ (§2.8.6, contract-and-property table) *"| **constructor-only** gained or lost | **breaking** | Gained: `default(T)`, `where T: new()`, and `with` are withdrawn at the type ([§9.9](#sec-constructor-only-records)). Lost: a guarantee clients relied on is withdrawn, as for `invariant` removed. |"*

Everything else classifies correctly with no change: adding the clause fires *"`requires` clause added — **breaking**"* or the `ensures`-added row, and where the record was defaultable it also fires *"derived-property verdict lost — **breaking**"*; §2.8.6's severity order takes the maximum. The practical guidance for a library author is worth one sentence in a later guide pass: **add the clause when the type is born**, not in v2.

### 5.7 FFI

§32.13's typed-handle admission — *"A C-compatible record with exactly one field, of *integer or pointer* scalar type … is FFI-compatible **directly** — parameter, return, `ref`/`out`, and callback positions"* — is exactly the wrapper shape this proposal serves, so constructor-only records will cross the boundary, and the residual is not remote. The classification itself is unaffected (C-compatibility is computed from field order and field types). The residual is stated over that whole position list, plus by-address C-compatible records (§32.5) and by-address inline-array components (§32.5/§15.1.3), rather than legislated away: it falls on the programmer-responsibility side of §32.10's boundary contract, and stdlib §10's `import System.Interop` audit posture is unchanged. The carve-out alternative is declined at this proposal's §8(f).

### 5.8 Modes, and where each half is enforced

Stated at the front rather than buried, because the two halves differ:

- The **static** half — non-defaultability, the `with` rejection, D5 — is a §3.6 derived property computed from structure at compile time. **It holds in every build mode.** It is not a check that can be turned off.
- The **dynamic** half is whatever the author's constructor clauses do, under §22.16's existing rules, unchanged: a `requires` is *"outside the mode entirely"* and evaluates in every build; an `ensures` runs *"under checked-contracts mode"* and is not evaluated in the default release build. An author who wants an always-on gate writes `requires`; one who wants to constrain the constructed value writes `ensures` and accepts the mode. §9.9's text says this in one sentence so the choice is made rather than discovered.

No new tier machinery, and no tier question: every clause here is an ordinary constructor clause discharged at *"the **declaring body**"* (`ensures`) or *"**every call site**"* (`requires`), exactly as §22.16 already fixes. The withdrawn design's open `static invariant`-at-the-`with`-point question does not arise, there being no `with` point and no invariant.

### 5.9 Borrow-bearing records — no interaction, and #3281 discharged

A borrow-bearing record may be constructor-only; nothing restricts it. Consequences are the ordinary ones and one is worth naming: §9.7 admits the **null-source** form of a borrow-bearing record in a static field expressly so that sentinels like `CancellationToken.None` exist, and where such a sentinel is produced by `default(R)` today it would need a constructor or factory. Landed incidence: zero — the standard library declares no contract clause on any member (this proposal's §7.1).

#3282 records this redesign as **blocked on #3281**, because restriction (a′) is sound only under the declared-type reading. (a′) is not carried forward, so the block is discharged **by removal**. #3281 was nonetheless ruled and landed at v0.265 — the predicate reads declared field types; *"boxed value types"* struck from both of §9.7's enumerations; §35.4 and §28.11 brought to currency — and the ruling's own sharpening is worth carrying into this file for whoever revives the invariant: declared-type at §9.7 is **necessary but not sufficient**, and declared-type at §9.7 **and §35.4** is what would make (a′) sound, because without §35.4 an interface reaches the record through a choice payload.

### 5.10 Grammar, patterns, and the rest — verified zeros

**Appendix A is untouched.** No production changes and no grammar comment changes: the design admits no new syntax, and A.4's `InvariantDecl` comment (*"class bodies only; rejected semantically in records and static classes (§22.16)"*) remains correct, this proposal admitting no record invariant. **Patterns (§19)**: §19.3 destructures and never constructs, so no pattern is a producer; §19.6's exhaustiveness counts enum constants, choice cases, and closed-hierarchy leaves, none of which a record is. **§30.14 sendable, §28.11 element category, §32.13 C-compatible, §9.7 borrow-bearing, §9.2 auto-hashable, §9.8 serialization-eligible** — none reads or is read by `constructor-only`; every verdict is unchanged. **§21.10 method-local records** take the rule unchanged, being ordinary nominal records.

### 5.11 Appendix C rows ▲

Four lines, three of them mirroring lines already there.

> ▲ **C.1** — *"`default(T)` is a compile error for a **constructor-only** record ([§9.9](#sec-constructor-only-records)) — a record declaring a constructor that carries a contract clause — as are `new T()` at such a substitution, default initialization of `R`-typed storage, and a `default(R)` default-value expression; such a record does not satisfy `where T: new()`."* (Spelled on the model of the enum line immediately above it.)
>
> ▲ **C.1** — *"A `with` expression whose source's static type is a constructor-only record is a compile error ([§9.5](#sec-non-destructive-update-with), [§9.9](#sec-constructor-only-records))."*
>
> ▲ **C.5** — *"A record declaring positional fields does not declare a constructor carrying a contract clause ([§9.9](#sec-constructor-only-records)): the auto-generated primary constructor can carry none."*
>
> ▲ **C.8** — *"A `serialized optional` component whose type is not defaultable ([§17.11](#sec-the-default-operator)) is a compile error at the conformance declaration; a contract clause evaluating `false` during the synthesized `Deserialize`'s construction step raises `SerializationException` with the violation recorded in its `CauseChain` ([§9.8](#sec-serialization-synthesis))."*

**C.15 needs nothing.** Its line *"`invariant` members appear only in classes"* stays true and stays wanted.

---

## 6. The deserialize band — resolved

**The question.** When the synthesized `Deserialize` constructs a record and the selected constructor's `requires` or `ensures` reports `false`, which exception reaches the caller: `ContractViolationException` (what §22.16 raises) or `SerializationException` (what every other structural fault of that read raises)?

**The resolution: `SerializationException`, raised by the synthesized code, recording the `ContractViolationException` as its cause.**

> ▲ (§9.8, *Synthesized `Deserialize`*) *"A contract clause evaluating `false` during the construction step — a `requires` or `ensures` clause of the selected constructor ([§22.16](#sec-contract-clauses)) — raises `SerializationException` from the synthesized code, by the same implementation-internal route as the missing-, duplicate-, and mismatched-component faults above, with the `ContractViolationException` recorded in the raised exception's `CauseChain` in [§31.4](#sec-standard-exception-types)'s snapshot form (type name, message, and stack-trace text — so the clause kind, the member, and the clause's source text survive into logs and `ToString()`). The input is data, not a caller's broken promise: a decoded value that fails the type's own validation is a structural fault in the input, classified with the faults above. A clause that **faults** rather than evaluating `false` is not covered by this rule and propagates as itself ([§22.16](#sec-contract-clauses)) — the failure is a defect in the contract, and the original exception is its diagnosis."*

**Why.** Four reasons, in decreasing weight.

1. **stdlib §1's band discipline puts it there.** Band 1's surface *"is never a recovery surface: its exception exists to report a bug, not to be branched on"*, and nobody's correct-caller obligation covers the contents of a byte stream. stdlib §2.15 already classifies this surface's read-side conditions in band 4: *"they are band-3-shaped — expected at the decode boundary, information-bearing — but no value-like error-payload type yet exists to carry them … so the typed exception is the mechanism"*.
2. **§22.16's own boundary sentence says so.** *"input whose invalidity is a normal outcome at a trust boundary is handled with Try shapes, nullable returns, or `choice` returns, never with `requires`; a `requires` states what a correct caller already guarantees, and its violation is a bug report, not a result."* Leaving the raw `ContractViolationException` at the decode boundary makes the deserializer a caller that violated a precondition it could not have checked.
3. **It makes one read fault one way.** A caller decoding untrusted JSON writes one `catch SerializationException`: missing field, duplicate field, type mismatch, unknown field under strict policy, and *invalid value* all arrive through one arm, each locatable through the same `Offset`/`Line`/`Column` surface. The alternative forces the caller additionally to catch the language's bug-report exception, which §31.4 forbids using for control flow.
4. **It needs no new declared surface, the raising being already compiler-internal.** Re-verified at v0.265: §31.4's `(message, cause)` chaining constructor is declared on **`Exception`**, constructors are not inherited (*"User subclasses chain by calling the base constructor"*), and `SerializationException`'s declared surface in stdlib §2.15 is three readonly properties and **no constructor at all**. So the wrap cannot be spelled as a user-visible constructor call without amending the stdlib — and it does not need to be: §9.8's synthesized `Deserialize` already raises `SerializationException` for missing and duplicate components with no constructor named anywhere, so the candidate text says *"by the same implementation-internal route"* and normatively fixes what the raised instance carries. The read side needs nothing new: *"the `(message, cause)` constructor **borrows** `cause` … and copies its type name, message, stack-trace text, and its own `CauseChain` into the new exception's `CauseChain`"*, and `CauseChain` is an inherited property.

**Under this design the case is cleaner than it was.** The withdrawn design had to cover a record *invariant* at a synthesized instrumentation point. Here the failing clause is always a **constructor clause arriving with data from the wire**, which is exactly the case reason 2's sentence contemplates.

**The cost, stated.** This changes behaviour that exists today: a record whose explicit constructor carries a clause currently surfaces `ContractViolationException` through `Deserialize`, and would now surface `SerializationException`. That is the pre-existing misfit 0106 §6.2(d) residual (4) named; fixing it is the point, and stdlib §1's scoping sentence — *"an existing spelling stands until its surface is re-ruled"* — makes re-ruling the sanctioned move. Corpus incidence: **zero** (this proposal's §7.1).

**Confidence: medium-high.** The band classification is high-confidence. The contestable half is the *conversion*: it would be the first place in the language where a `ContractViolationException` is not propagated. Put to the owner as **Q2**, with §8(g)'s stdlib-amending alternative named.

**Separability.** This ruling is a repair of a pre-existing misfit and does not depend on the rest of the proposal; it may land separately, and the rest of the proposal is correct without it (a failing wire-borne clause would then surface as `ContractViolationException`, as today).

---

## 7. Costs, migration, and the corpus

### 7.1 The corpus — reclassification is zero, verified four ways

Searched at `03c3ffc`, v0.265. The question is which landed records declare a constructor carrying a contract clause of either kind.

| Population | Count | Detail |
|---|---|---|
| Records in `Bezel_Standard_Library.md` with a clause-bearing constructor | **0** | The library declares **no contract clauses at all**, on any member of any kind: the document's only `requires(` occurrence is §5's prose about `Range<T>.Contains`, and it contains no `ensures(` |
| Records in `Bezel_Language_Spec.md` | **0** | Every clause site is §22.16's `Account` **class** example |
| Records in the six guides | **0** | Guide 6's clause sites sit on class members and free methods; `DivMod`'s clause is on a **free static method** beside `record QuotRem(Quot: int, Rem: int)`, not on the record |
| `tests/conformance/cases/*.bzl` (571 cases) | **0** | 7 files contain `requires(`, 4 contain `ensures(`; each read individually — all on class members or free methods (`guide6_0000`–`guide6_0011`, `spec_0160`) |
| Records affected by **D5** (positional fields + a clause-bearing constructor) | **0** | Follows from the four above |
| Types losing `default(T)` / `where T: new()` / Try (c) / `serialized skip`/`optional` / `OwningBuffer<T>(length)` / `with` | **0** | Follows |
| Single-component wrapper records over a scalar — §27.8's prescribed shape | **5** | `Counter(Value: int)`, `BitMask(Bits: ulong)`, `TriBool(Value: int)`, `UserId(Value: int)`, `OrderId(Value: int)` — candidates a later guide pass could *add* a clause to, none affected by the landing |
| Corpus uses of `serialized optional` on a component | **1** | `stdlib_0021`'s `LastSeen: DateTime? … optional` — over a nullable component, legal before and after §5.4 |

**Nothing breaks at the landing.** The design lifts no rejection and adds none to any program that compiles today, since no record anywhere carries a constructor clause. Breakage is opt-in in every direction. Two changes are *not* opt-in and are named: §5.4's `serialized optional` guard (incidence zero) and §6's band ruling (incidence zero).

Two readings of the zero are available and the honest one is stated: it may mean the shape is not needed, or it may mean nobody writes the hardened wrapper because its guarantee is currently false in eight spellings. This survey cannot decide between them; what it does establish is the migration fact.

### 7.2 The charge on a constructor clause — accepted, with reasons

The design **charges** for a contract clause on a record constructor. An author who adds `requires(scale <> 0)` for ordinary defensive reasons loses, for every client, `default(R)`, implicit initialization of `R`-typed storage, `[N]R` slots, `where T: new()`, Try shape (c), `serialized skip`/`optional`, `OwningBuffer<R>(length)`, `default(R)` as an optional-parameter default, and `with`. §3.6 rule 3 makes the flip *"a breaking change for clients relying on it, exactly as a signature change is"*. **There is no in-constructor remedy**: the property fires on any clause on any constructor.

Accepted, for four reasons, and the remedy is named in the normative text so the rule does not read as a tax on §22.16:

- **The remedy is one line and it is landed.** State the condition on a **static factory** — §22.16's Scope admits clauses on static methods — where the type is not meant to carry the property. §9.9's text names it; so does D5's diagnostic.
- **The direction is safe.** A false positive withdraws admissions; it never widens one. And §3.6 rule 4 obliges the diagnostic to name the property, the offending type, and the structural reason at the point of consequence, so the author is told, with a good message, rather than surprised.
- **Incidence is zero today** (§7.1), so nothing is reclassified and the cost is entirely prospective.
- **The alternative is a new construct.** A purpose-built record modifier is a grammar change and a second mechanism for a fact §3.6 already knows how to compute — the shape §3.6's closing rule and this proposal's §8(b)/§8(c) both decline. It is nonetheless the design's one genuinely contestable spelling and is put to the owner as **Q1**.

One thing this design improves over a `requires`-only trigger: because `ensures` also triggers, the author who means *"this type's values satisfy P"* has a clause that says exactly that, so the population that trips the property accidentally is smaller than it would otherwise be.

### 7.3 The positional form becomes unvalidatable — accepted, with reasons

D5 rejects positional fields beside a clause-bearing constructor, so the shape §27.8 prescribes — `record Port(Value: int);` — cannot be validated without moving to the body form:

```bezel
public record Port                              // instead of: record Port(Value: int);
  public readonly property Value: int;
  public constructor(value: int)
    ensures(Value >= 0 and Value <= 65535)
    Value := value;
  end constructor;
end Port;
```

Five lines instead of one — and exactly §9.2's own `Rectangle` shape, so it is idiomatic rather than a workaround. The alternative that would remove the cost is a `ContractClause` slot on `RecordDecl`, declined at §8(c) on grammar cost and class symmetry and put to the owner as **Q4**. Accepted here because the rejection is **diagnosed with two named remedies** rather than silent, which is the failure the rule exists to prevent: under the un-restricted rule, an author writes a clause, the property flips, and `new Port(-1)` still compiles through the auto-generated primary.

### 7.4 The residuals, restated in one place

Five, each recorded above, none silent: the FFI boundary (§5.7, §8(f)); §9.8's multi-constructor selection (§4); §22.7's watermark gap for non-defaultable value-typed fields, pre-existing (§4); the `ensures` mode split, so a release build checks only `requires` (§5.8); and the plain fact that the property is provenance and not validity — which is not a hole but the design's stated claim (D2).

---

## 8. Alternatives declined

**(a) Do nothing.** Priced at 0106 §6.2(d) and re-verified at this proposal's §1. The wrapper stays writable and its one guarantee stays false, now in eight spellings rather than four. **Declined.**

**(b) A purpose-built marker instead of the clause trigger ▲.** A record-level modifier — `constructed record Port … end`, or similar — flipping the property explicitly. It removes §7.2's charge entirely and makes the author's intent unmistakable. **Declined here**, on three grounds: it is a grammar production and a new contextual keyword for a fact §3.6's machinery already computes; §3.6 rule 1's *"Computed, never declared"* is at least in tension with a modifier whose only content is the property (whereas a contract clause has independent runtime effect, and the *defaultable* row's own *"an enum **declaring a zero-valued constant**"* precedent is a member-level, author-flippable clause in the same table cell); and it would mean a record could state a validating constructor *without* the property, which is precisely the silently-ineffective shape D5 exists to reject. **Put to the owner as Q1** — this is the design's weakest link and deserves an owner ruling.

**(c) A `ContractClause` slot on `RecordDecl` ▲.** `record Port(Value: int) requires(Value in 0..65535);`, the clause attaching to the auto-generated primary constructor (whose parameters are the positional fields, §9.1, so the names resolve). It removes §7.3's cost and keeps §27.8's prescribed one-liner. **Declined here**, on three grounds: it is the proposal's only grammar change, against a design that otherwise has none; §22.16's Scope would gain a clause position whose meaning is a *desugaring onto a constructor the author never wrote*; and **`ClassDecl` has the identical limitation** — a class primary constructor can carry no clause either (A.3) — so the slot would create an asymmetry between the two kinds where none exists today. **Put to the owner as Q4.**

**(d) Keep `invariant` class-only; promote the wrapper to a class.** Today's only complete answer. Declined on cost and on kind: a class costs an allocation, a disposal obligation (§28), reference semantics where the author wants value semantics, and it loses `with`, auto-equality, defaultability of *containing* types, C-compatibility (§32.13's typed-handle admission is records-only), and inline storage. §27.8 prescribes the record precisely because these values should not be heap objects.

**(e) Instrument `with` instead of rejecting it.** The withdrawn design's approach. **Declined**: with no standing predicate there is nothing to evaluate at the point, and any check it *could* run would be an `invariant`-shaped one — the whole declined mechanism of this proposal's §3 — evaluated in checked-contracts mode only, so absent from the default build. Rejection also enforces §9.5's own landed advice for the population it addresses rather than adding a rule beside it. Recorded as the one place this proposal is **subtractive** for adopting records; put to the owner as **Q3**.

**(f) Reject constructor-only records at FFI boundary positions.** It would close the one producer §4 leaves open, and the machinery is the same use-site rejection keyed on the same declaration-site predicate. **Declined** on three grounds: it is an *enumerate-the-boundary-positions* rule (§32.13's five positions, §32.5's by-address form, §15.1.3's inline-array form), which is the shape §13.6's structural lesson identifies as the withdrawn design's failure mode; §32.10's boundary contract already assigns the obligation to the programmer, and every other derived type property stops at the same line; and it would forbid exactly the typed-handle use §32.13's single-field admission exists for. Recorded rather than legislated, and the residual is stated over the whole position list at §5.7.

**(g) Amend the stdlib so §6's wrap is user-visible.** Give `SerializationException` a `(message, cause)` chaining constructor and have the synthesized code call it. **Declined** because the raising is already compiler-internal (§6, reason 4), so the amendment would buy nothing this proposal needs while making a spec-only change into a two-document one — but it is the coherent alternative if the owner prefers the wrap spellable in user code, and Q2 names it.

**(h) A separate `validated record` type kind ▲.** Declined on §3.6's closing rule (*"rather than introducing a parallel mechanism"*), on cost (a §3.1 row plus a case in every kind-dispatching rule in the document), and because the distinction is not about the *kind* — a `Port` is a record in every respect except that it has a gate. This proposal is that decline's positive form: the classification is a derived type property, as §3.6 requires.

**(i) Contract-carrying type aliases ▲.** 0106 §6.2(a) declines this independently and this proposal concurs: aliases see through, are module-internal, and offer no conversion boundary to check at, so the check would land *"by a new rule invented for the feature — precisely the parallel mechanism §3.6's closing rule forbids"*.

---

- **(j) Fold the rule into the existing `defaultable` property and state `with` as a plain §9.5 rule ▲.** No new §3.6 row: §17.11's *defaultable* computation gains a declaration-keyed disqualifier (coherent — the row already carries *"an enum **declaring a zero-valued constant**"*), and §9.5's rejection keys on the same syntactic condition. **Declined on cross-module carriage.** §2.8.2 carries derived type properties *"as computed verdicts"* but carries contract records only *"[f]or every member the interface describes"* — public and `protected` members. A record whose clause-bearing constructor is `private` or `internal` (A.3's `ConstructorDecl` requires a `Visibility` and admits both) therefore ships a *defaultable* verdict the consumer can read and a `with` predicate it cannot compute, which is the outcome §2.8.2's own parenthetical exists to forbid. Restricting the trigger to public constructors is refused by §14.2's landed principle — *"keying it to accessibility would let the same substitution satisfy the constraint in one module and fail it in another"* — and a bespoke carriage channel is the parallel mechanism §3.6's closing rule forbids. Coarsening `with` to *every* non-defaultable record is refused by `record Person(Name: string, Age: int)` (§9.1), whose `with` is §9.5's own example. **The single lever is D4:** dropping the `with` rejection (Q3's other arm) removes the property, §9.9, and the §2.8.6 row together, at the cost of the weaker claim §8(e) prices. **Put to the owner alongside Q3.**

## 9. Interactions

- **Proposal 0106 (the survey; accepted 2026-08-20).** This is its slate item **i**. The soundness half of its first-rank finding lands here in full; the *"record invariants"* half does not, and 0106's nomination of it is left standing as a recommendation.
- **§22.16's out-of-scope deferral, and `DECLINED.md`.** Both stand, **unstruck**. This proposal is not a substitute for record invariants; it is the fabrication half landed separately, and the deferral's row keeps its *"—(none stated)"* reopening bar. A graduation of this proposal should update that row's *authoritative record* pointer to name this file's §3 as the place the instrumented design is declined-with-reasons, and record §3's reopening bar there. That is a `DECLINED.md` index edit under process §9's fourth checklist line, not a change to the deferral itself.
- **#3281 (ruled, v0.265).** No contact; the block is discharged by removal (this proposal's §5.9).
- **#3284, #3291.** Surfaced by #3281's ruling and untouched here.
- **Proposal 0043 (behavioral contracts; graduated into §22.16).** Nothing 0043 decided is reversed: contract inheritance, tiers, clause typing, the `requires`-not-for-input boundary, and the always-on/`requires` posture all stand. This proposal adds **no** tier machinery — the withdrawn design's two-site `static invariant` discharge question does not arise, there being no invariant and no `with` point.
- **Proposal 0047 (mechanized core semantics).** No ownership-relevant construct: records carry no owned state (§9.7), clause evaluation is pure and borrow-only (§22.16), and the property is a compile-time classification. Mechanization scope unchanged; no axiom disposition touched.
- **Proposal 0105 (body-locality invariant; accepted).** Respected, and cited: 0105 §1 boundary 5 fixes that *"Contract clauses and default values are signature-side, not bodies"*, which is exactly why D1's predicate reads a declaration and never a body (this proposal's §10).
- **Proposal 0102 (small records) and §27.8's newtype guidance.** A later guide pass belongs with them — Guide 3 is where the constructor-only wrapper should be shown, and §27.8's *"use a single-field record"* sentence would gain a pointer, since D5 means the *positional* spelling it shows is not the validated one. Out of scope here (this proposal changes no guide) and named so it is not lost.
- **v0.263's `Range<T>.Contains` purity.** Directly favourable and now load-bearing for the readable spelling: `ensures(Value in 0..65535)` is admissible clause text exactly because that marker landed. stdlib §5 records the motive verbatim.
- **Future: operator synthesis for single-field numeric records (0106 §6.2(b)).** 0106 §6.4 sequences it *after* this proposal, as *"ergonomics over a currently-unsound foundation"*. Landing this is what makes that sequencing coherent; nothing here forecloses it.

---

## 10. Landing shape and classifications

**Body-locality classification (0105 §1): local.** Every rule this proposal adds is decided from one declaration's own text. D1's predicate reads whether the record's constructors carry contract clauses, and a contract clause is **signature-side, not a body** — §22.16 places clauses *"between its signature and its body"*, and 0105 §1's fifth boundary statement adopts exactly that vocabulary. D3's consequences ride §3.6 rule 2's existing per-instantiation shape (and are in fact substitution-invariant here). D4 is decided from the `with` expression's source type. D5 is decided from the record declaration's own header and member list. No rule consults another declaration's body, and none re-elaborates a body per use.

**Kernel classification (the v0.198 obligation): no contact.** No rule of §28 or §30.5 is amended, extended, or read. The property is a §3.6 classification consumed by §17.11, §9.5, §14.2, §7.9, and §9.8; records carry no owned state (§9.7); clause evaluation is pure and borrow-only (§22.16). The register stands unchanged.

**Change class (process §2): design change**, requiring owner acceptance of this proposal before a ruling-class graduation.

**Landing order**, one spec revision, no stdlib and no rationale-normative content:

1. §9.9 — the new section: the property, its claim, D5's declaration rule, the mode sentence, the factory remedy, and the residual list (this proposal's §2, §4).
2. §3.6's *defaultable* row and §17.11's two clauses (D3).
3. §9.5's `with` rejection paragraph (D4).
4. §9.8's `serialized optional` guard and the decode-band sentence (this proposal's §5.4, §6).
5. §2.8.6's one row (this proposal's §5.6).
6. Appendix C's four lines (this proposal's §5.11).
7. Changelog entry whose Migration note is this proposal's §7 (opt-in breakage only; two non-opt-in changes, both with zero corpus incidence), stating the body-locality and kernel classifications above.
8. A rationale entry recording *why* the property is derived rather than declared and *why* the instrumented invariant is declined — this proposal's §3 is its draft, and §3's reopening bar is the sentence to carry.
9. `DECLINED.md`: the *Record invariants* row's authoritative-record pointer updated to name this file's §3, with its reopening bar (process §9, fourth checklist line).

Implementation follows the repository's ordinary discipline (spec-derived tests first; each fix ships a spec-cited regression test). Natural test seams: the eight rejection sites of §4's table; the `with` rejection, inside and outside the declaring record; D5's declaration error and both its remedies; the §9.2 non-regression (a constructor-only record is still auto-hashable and still serialization-eligible); §9.8's explicit-constructor branch and the band; and the negative tests that a record *without* a constructor clause is unaffected in every one of those sites.

---

## 11. Questions for the owner

Four genuine forks; each carries a recommendation, so a silent yes is a complete answer.

- **Q1 — The trigger (D1, this proposal's §7.2, §8(b)).** Is a contract clause on a record constructor the right trigger, or should the property be flipped by a purpose-built record modifier? The clause trigger costs the charge at §7.2 with no in-constructor opt-out; the modifier costs a grammar production, a new contextual keyword, and re-opens §3.6 rule 1's argument — and it would admit a validating record *without* the property, the silently-ineffective shape D5 exists to reject. **Recommendation: the clause trigger, with the factory remedy named in the normative text.** Confidence medium; this is the design's weakest link.
- **Q2 — The decode band (this proposal's §6).** Should a contract clause answering `false` inside the synthesized `Deserialize` surface as `SerializationException` with the violation in its `CauseChain`, raised by the same implementation-internal route as the other structural faults (recommended) — or stay `ContractViolationException` with the mismatch recorded? A third option, if the wrap should be spellable in user code, is §8(g)'s stdlib amendment. **Recommendation: wrap, compiler-internal.** Confidence medium-high. This ruling is separable and may land on its own.
- **Q3 — Rejecting `with` (D4, this proposal's §8(e)).** Confirm that `with` over a constructor-only source is a compile error, rather than left legal with §9.5's advice unchanged. Rejecting makes the property's claim exactly true and enforces advice §9.5 already gives; keeping it makes the claim *"produced by a constructor, or by a `with` over such a value"*, which is a materially weaker sentence at exactly the point 0106 §5.3 says the wrapper's guarantee dies. **Recommendation: reject.** This is the one place the proposal is subtractive for adopting records, and the withdrawn design ruled the other way in a design that had a runtime check to offer instead.
- **Q4 — The positional form (D5, this proposal's §7.3, §8(c)).** Confirm that a record with positional fields may not declare a clause-bearing constructor, and that no `ContractClause` slot is added to `RecordDecl`. The cost is that §27.8's prescribed one-liner cannot be validated without moving to the body form; the slot would remove that cost at the price of the proposal's only grammar change and an asymmetry with `ClassDecl`, which has the identical limitation. **Recommendation: no slot; reject the combination.**

*Deliberately not asked:* whether §22.7's missing watermark for **non-defaultable value-typed fields** should be stated (this proposal's §4, residual 2). It is a pre-existing gap that already holds for choice- and zero-constant-less-enum-typed fields, it is reachable only inside §22.7's own declared residual, and fixing it is a §22.7 clarification with its own scope — it should be filed as a spec-defect issue on its own merits rather than ruled inside a design change.

*Also not asked:* whether record invariants should exist at all. §22.16's deferral and `DECLINED.md`'s row are left standing and this proposal takes no position beyond declining the *mechanism* attempted at v0.264, with reasons and a reopening bar (this proposal's §3).

---

## 12. Revision record

Drafted 2026-08-20; revised the same day after adversarial verification. The revision is recorded because two of the changes are corrections of substance, not polish:

- **The instrumentation-completeness argument was wrong.** The first draft claimed no record member body can falsify an invariant, citing §9.4's rejection of receiver-storage stores while overlooking the *mediating* store the same paragraph legalises. Restriction (a′) is the repair (this proposal's §2.2, §2.3, §8(f)).
- **A fifth fabrication route was missing.** `serialized optional` supplies `default(T)` with no defaultability guard (this proposal's §1.6, §4(3)).
- **The decode-band mechanism named a constructor `SerializationException` does not declare** (this proposal's §6, reason 4) — re-spelled as compiler-internal, which is also what keeps this proposal spec-only.
- Smaller corrections: the two-site `static invariant` discharge withdrawn as invented tier machinery (this proposal's §2.6); §3.3 rebuilt on the enum precedent alone; the enum/interface amendment corrected (syntax-unreachable, not semantically rejected — this proposal's §2.1); the FFI residual stated over §32.13's whole position list; `OwningBuffer<T>(length)` added; the Currency row's evidence restated as per-section byte-identity; 0106's status updated to accepted; five quotation truncations closed.

## 13. Revision record — the v0.264 withdrawal

This proposal was scoped into revision **v0.264**, its four §11 questions ruled by the coordinator under the owner's 2026-08-20 autonomous-work direction, and its design drafted into the normative documents. It was then **withdrawn before landing**, and every one of its edits reverted; v0.264 shipped proposal 0109 alone. Status returns to `under-review`.

The ground for withdrawal is narrow and worth stating precisely, because it is not "the feature is unwanted": record invariants remain proposal 0106's first-rank finding, `DECLINED.md`'s *Record invariants* row stands as a plain deferral against §22.16's out-of-scope item, and nothing here bears on whether the language should have them. What failed is **this proposal's soundness argument** — specifically §2.3's *instrumentation-completeness* claim, that the instrumentation points plus a copy are the only ways a value of an invariant-bearing record comes to exist. That claim was attacked in three independent adversarial rounds and **each round found either a new admitted forge or the reopening of one already closed**. A redesign should start from this section rather than from §2.

### 13.1 Round 1 — non-public writable storage

The first landed rule was §2.2's: instrument every setter, and bar a **`public`** writable field. The critique produced a working forge using only members the rule declined to instrument — a `private` settable auto-property, written from a **static factory of the declaring record itself**, where §13's visibility rules make the private member fully accessible:

```bezel
public record Rate
  public readonly property Value: int;
  private property Scratch: int;              // private, settable — not barred
  invariant(Value > 0);

  public constructor(v: int)
    requires(v > 0)
    Value := v;
    Scratch := 0;
  end constructor;

  // Same-type, so `private` is accessible. No instrumentation point is
  // reached: this is not a constructor, not a `with`, not a synthesized
  // Deserialize — and under the round-1 rule the setter of a *private*
  // auto-property was not instrumented either.
  public static method Forge(r: Rate): Rate
    var copy := r;
    copy.Scratch := -1;
    return copy;
  end Forge;
end Rate;
```

Three variants of the same shape were produced: a `private` writable **field**, a `ref` parameter of the record's own type, and a `private` **inline-array** component whose slot store lands in the component's own storage (§9.4's inline-array chain). The common mechanism is that the rule keyed on **visibility**, while the reachability that matters is the *declaring type's own* code, for which visibility is no boundary at all.

**The repair adopted:** §2.2 was replaced by the structural rule — a record declaring an `invariant` member **declares no writable storage at any visibility**, so no assignable location exists inside the value and there is no store to instrument anywhere. That repair is sound as far as it goes, and rounds 2 and 3 went further.

### 13.2 Round 2, forge A — the constructor escape, and why §22.7's gate is vacuous here

§2.3 asserted that construction composes with §22.7 exactly, borrowing the class rule's reasoning: at construction exit every §7.9-required field is definitely assigned, so evaluating the invariant is admitted by the observation gate. **The direction that matters is the other one** — not whether the invariant may be evaluated at exit, but whether a value may *leave* before exit. It may, and for an all-primitive record it may from the constructor's **first statement**:

```bezel
public record Pos
  public readonly property X: int;
  public readonly property Y: int;
  invariant(X > 0);

  public constructor(x: int, y: int, ref leaked: Pos)
    requires(x > 0)
    leaked := this;      // a COPY of `this`, with X = 0 — invariant false
    X := x;
    Y := y;
  end constructor;
end Pos;
```

The escape is legal, and the mechanism is exact. §22.7 rule 1 gates a synchronous observation of `this` — "`this` stored anywhere" included — on every field **"that §7.9 requires to be explicitly initialized"** being definitely assigned. §7.9's *Default initialization* subsection says a field whose type is **defaultable** "satisfies the must-initialize rule vacuously and is implicitly initialized to that default"; only a **non**-defaultable field "requires an explicit initializer". `X` and `Y` are `int`. **The gated set is therefore empty, and the gate admits the escape at every point in the body, including before any assignment.** The leaked copy holds `X = 0`, reaches no instrumentation point, and is indistinguishable at every later use from a value that passed one.

The gate is not vacuous for the *class* rule this argument was borrowed from — a class typically has non-defaultable fields — and, decisively, **§22.16's class subsection anticipates the escape and neutralizes it**: a leaked `this` re-entering a public method from outside the instance hits an instrumented public entry, which is why the class rule can afford to record the escape as a bounded residual. This proposal's record subsection **deletes that mechanism** — a record has no public re-entry point, by design, since it declares no writable storage — and never re-audits the inherited escape argument against its own removal of the thing that answered it. That is the single most transferable finding here.

### 13.3 Round 2, forge B — `readonly` storage is assignable in *any* constructor of the declaring record

The no-writable-storage rule admits exactly three storage shapes: auto-generated positional-field properties (§9.1), `readonly` auto-properties (§23.1), and `readonly` fields (§22.8). For the latter two, the permission that makes them writable during construction is stated **positionally, not per-instance**. §22.8: a `readonly` field "may be assigned only in a field initializer or **in a constructor body**". §23.1: a `readonly` auto-property "may be assigned only in a field initializer or **in a constructor**". Neither says *of this instance*. §22.3's assignable-location list carries the same qualification from the other side — "no `readonly` field **outside its assignable context** (§22.8)" — so inside that context the location is assignable, whoever holds it:

```bezel
public record Counter
  public readonly property Value: int;
  invariant(Value > 0);

  public constructor(v: int)
    requires(v > 0)
    Value := v;
  end constructor;

  // Any constructor of Counter is inside `readonly`'s assignable context.
  // The context is POSITIONAL, so `victim`'s storage is assignable here too.
  public constructor(v: int, ref victim: Counter)
    requires(v > 0)
    Value := v;
    victim.Value := 0;   // another instance, already constructed and checked,
                         //   made invalid — no instrumentation point reached
  end constructor;
end Counter;
```

This one is worse than forge A for the proposal's structure: it is not an escape of a *partly built* value but a **retroactive falsification of a fully constructed, already-checked one**, and it defeats the completeness argument at its root — the claim that no assignable location exists inside the value is false inside the declaring record's own constructors, which is precisely where the argument stopped looking.

### 13.4 Round 3 — the §9.7 amendment reopened the store it was written to close

§2.3's *Never instrumented* paragraph argues that the **mediating** store §9.4 legalises cannot be spelled on such a record, because a mediating store needs a class- or interface-typed field and §9.7 admits only **runtime-managed** reference components there, none of which declares a settable member. An interface-typed field is the hole: §9.7's list of what does *not* make a record borrow-bearing reads "`string`, function types, **boxed value types**", and a field declared as an interface type may at runtime hold a boxed value — so whether the predicate reads the **declared type** or the **held value** decides whether `record R(Handle: ISomething)` is borrow-bearing and therefore whether restriction (a′) excludes it.

The amendment written to close this added a *declared field types* sentence to §9.7 — but left **"boxed value types" standing in the enumeration immediately before it** (spec §9.7, first paragraph after the bullet list) **and again in §9.7's closing summary** ("a record whose only reference-typed fields are of runtime-managed types (`string`, function types, boxed value types)"). Two unamended neighbours contradicting the amendment is not a closed ambiguity; an interface-typed field still escapes borrow-bearing classification under the surviving text, restriction (a′) still fails to exclude the record, and the mediating store that §2.3 declares unspellable is readmitted.

**Recorded separately, for the next revision:** the underlying question — *does the borrow-bearing predicate read declared field types or held values?* — is a **real, pre-existing §9.7 defect**, independent of this proposal. It is being filed as a spec-defect issue rather than fixed here, because the half-fix above is what made it look closed.

### 13.5 The frame question the patches never reached

None of the three rounds' repairs touches this, and it may be the deeper problem. An invariant clause is admitted under §22.15's pure-body discipline, and §22.15 rule 1 admits **static field reads**, stating outright that "a pure member may read *mutable* state, so two calls with identical arguments may return different values". So:

```bezel
public record Window
  public readonly property Hi: int;
  invariant(Hi <= Limits.Max);   // Limits.Max is mutable static state
  ...
end Window;
```

`Limits.Max` changes; every existing `Window` value now violates its invariant, with **no store to the record, no copy, and no instrumentation point anywhere** — nothing for any instrumentation set, however complete, to catch. This is not a forge to be closed by another restriction; it is a question the design must answer first: **is a record invariant a standing property of every value of the type, or a construction-time claim about how a value was made?** §2.3's completeness argument is written as though the former; the instrumentation set can only ever deliver the latter. Every rule in §2 reads differently depending on the answer, which is why no patch in three rounds could be the right next step.

### 13.6 What a redesign inherits

- **Sound and re-usable:** the §3.6/§17.11 non-defaultability derivation (§3.3), whose enum-zero-valued-constant precedent is exact and which was verified under attack; the §4 companion list, in particular the §9.2 auto-equality membership trap, which a redesign must still carry; and the corpus survey of §7.
- **Answered but not landed:** the four §11 rulings (decode band; the §9.3 combined form as the spelling; checked-contracts mode; the structural form of restriction (a′)). They stand as coordinator rulings a redesign may reopen, not as settled text.
- **Must be re-derived from scratch:** §2.3's instrumentation-completeness argument, and with it the *Never instrumented* paragraph and §2.2's declaration restrictions, which exist only to support it. The two forges above and §13.5's frame question are the acceptance tests.
- **The structural lesson:** the class subsection's soundness rests on a **re-entry point** (public-method entry/exit re-checking) that the record form deliberately has none of. Any record design that borrows the class rule's reasoning must first re-audit every argument that mechanism was silently carrying.

---
