# Proposal 0107 — Record invariant members, and non-defaultability as a §3.6 consequence

| | |
|---|---|
| **Status** | under-review — draft (2026-08-20), awaiting owner review; revised 2026-08-20 after adversarial verification (this proposal's §12 records the revision) |
| **Tracking issue** | — |
| **Target module(s)** | none — **spec-only, and deliberately so**. The normative surface is §22.16 (a `Record invariants` subsection plus two sentence amendments), §3.6's *defaultable* row, §17.11, §9.2's membership list, §9.5, §9.8 (two additions: the `serialized optional` guard and the decode-band rule), §2.8.2's contract-record carriage sentence, and one Appendix A.4 grammar *comment*. **No grammar productions change** (this proposal's §2.1) and **no stdlib document changes**: the §6 decode-band ruling is spelled as compiler-internal construction precisely so that it needs none (this proposal's §6, and §8(g) for the stdlib-amending alternative declined there). |
| **Written against** | spec/stdlib/rationale **v0.262** (HEAD `8a03dd2` at authoring) — every quotation re-verified against that tree, not taken from proposal 0106's transcription |
| **Currency** | v0.262 (`8a03dd2`). **A currency check is owed at review.** v0.263 landed in the working tree while this was drafted, and it is **not** a small diff — it carries a `CancellationTokenSource` fact ruling amending §22.12 rule 4, §30.8, §28.13/§30.14, a new stdlib §6.1 Facts block and §6.8 rewrite, four rationale entries, a changelog entry, and two `spec-change-process.md` additions. The evidence that this proposal is unaffected is therefore **per-section byte-identity**, not diff size: §2.8.2, §2.8.6, §3.6, §7.9, §9.1–§9.5, §9.7, §9.8, §14.2, §17.11, §19.3, §22.15, §22.16, §31.4, §32.10, §32.13, and Appendix A.3–A.4 are byte-identical between `8a03dd2` and the v0.263 tree; the one section v0.263 touches that borders this proposal is §22.12, which this proposal does not cite. The one v0.263 change that *is* relevant is favourable (this proposal's §9). Re-verification before any ruling should cover that list plus stdlib §1, §2.15, §4.14. |
| **Origin** | Slate item **i** of proposal 0106 (*"Record invariants + non-defaultability"*, its §9 table — *"the first-rank finding"*), opened by owner direction on 2026-08-20. 0106 §6 is the design substrate; this proposal is the adoption vehicle its §6.4 recommends (*"Promote to its own proposal — the survey's first-rank finding"*). |

*Reference convention (0106's, carried forward): unprefixed §-references are **language-spec** references at v0.262; **rationale** and **stdlib** references are marked as such; references to this proposal's own sections are always written "this proposal's §N", and where two such references are conjoined the prefix is repeated, since its self-numbers are all real spec sections. 0106's sections are cited "0106 §N". Quotations are verbatim from the v0.262 documents; **bold inside a quotation is the source's own**; an ellipsis marks every truncation. **▲** marks candidate surface that exists in no landed document — every line of proposed normative text below carries it. Anchored links (`#sec-…`) appear **only inside ▲ candidate blocks**, where they are the spec-internal anchors the text would carry once pasted into the spec; prose outside those blocks uses bare §-references, as proposals 0102–0106 do. Syntax sketches are minimal and deliberately few (two code blocks in the whole document); a surface pass follows.*

---

## 0. What this proposes, in one page

Two changes, one of which is a consequence of the other, plus one restriction that makes the first sound.

**(a) Admit `invariant(E);` members in record bodies**, lifting the item §22.16 already records as out of scope — *"record invariants (constructor-exit plus a `with`-update instrumentation point — a later design)"* — instrumented at the two points that sentence names, plus the auto-property-setter point §22.16's class rule already fixes and the one storage-level construction §9.8 performs on the type's behalf. The grammar needs no new production: Appendix A.4's `InvariantDecl` is already a `Member`, and `RecordBody ::= Member* "end" Identifier? ";"`, so `invariant(E);` in a record body **parses today** and is refused only semantically (this proposal's §2.1).

**(a′) One declaration restriction, without which (a) is unsound: a record declaring an `invariant` member may not be a borrow-bearing record** (§9.7's derived property). §9.4 legalises the *mediating* store — a record member body may store **through** a class- or interface-typed field into the borrowed heap object — so on a borrow-bearing record an uninstrumented method can change state an invariant reads. Deeper: state reachable through a borrow is mutable by the borrow's *source* at any time, which no instrumentation point inside the record could ever observe, so an invariant over it would be a promise the language cannot keep. The restriction costs the motivating population nothing — `string`, function-typed, and boxed components are runtime-managed and do not make a record borrow-bearing (§9.7), so every scalar and string wrapper remains expressible — and it restores the completeness argument the instrumentation set needs (this proposal's §2.3).

**(b) Let non-defaultability fall out of §3.6**, by one clause on the *defaultable* row: a record declaring an `invariant` member is not defaultable. `default(T)` is then rejected by §17.11; `where T: new()` stops admitting the type by §14.2's own definition (*"A **value-type** substitution satisfies the constraint exactly when it is **defaultable**"*); `serialized skip` on such a component is already a compile error by §9.8's existing rule; and stdlib §1's Try shape (c) becomes unwritable for the type, with shape (a) forced by shape (a)'s own stated reasoning.

**Five fabrication routes, not four.** 0106 §6.2(d) named four (`default(T)`; `where T: new()`; Try shape (c); the §9.8 decode band). A fifth is named here for the first time: **`serialized optional`**, whose §9.8 bullet says *"On read, a missing field yields `default(T)` instead of raising `SerializationException`"* and which, unlike the `skip` bullet beside it, carries **no defaultability guard**. It needs one (this proposal's §1.6, §4(3)).

**What this buys.** Today §27.8 directs every author to model a distinct scalar as *"a single-field record"*, and §22.16 makes the one construct that says *"every value satisfies P"* class-only. The result is 0106 §5.3's finding, in its words: *"**Bezel's own recommended newtype shape cannot state its own validity**"*, and the escape (promote to a class) costs an allocation and a disposal obligation on a value that wants neither.

**What this does not propose.** No new type kind. No new conversion rule (§25.4 stands). No prover work, and no new tier machinery (this proposal's §2.6 records, rather than invents, the one open tier question). No change to the ownership rules. No stdlib member changes.

**Cost, stated honestly.** It is no longer "a rejection lifted and one clause": it is a rejection lifted, one §3.6 clause, one declaration restriction (a′), and six consequential amendments (this proposal's §4) — one of which, §9.2's membership list, is a *trap* rather than a nicety, and one of which, §9.8's `optional` guard, closes a hole that predates this proposal. One design question is genuinely contestable and is resolved here rather than deferred: which band the decode boundary reports a failing invariant in (this proposal's §6).

---

## 1. The defect

### 1.1 The prescribed shape, and the half it cannot state

Three landed rules meet at one point.

§27.8 prescribes the carrier: *"For genuinely distinct types (`UserId` and `OrderId` should not be interchangeable despite both being `int`), use a single-field record"* — and the alias is explicitly not it: *"A type alias is a pure synonym — it does not create a new type."*

§22.16 makes the invariant class-only, twice. In its Scope paragraph: *"record and enum methods (`old` over receiver fields captures by the value rules; the *invariant* member is class-only)"*. And in its out-of-scope list, as a **deferral with its design already sketched**: *"**Out of scope in this revision**, each recorded: record invariants (constructor-exit plus a `with`-update instrumentation point — a later design)"*. Proposal 0043 §9.6, the vehicle that shipped §22.16, records the same judgement in the same words: *"a record invariant collapses to constructor-exit checking plus a rule for `with`-update (spec §9.5's synthesized copy would need an instrumentation point) — a coherent later design, not attempted here."*

§9.5 removes the remaining constructor-based guarantee for any record whose storage is externally nameable: *"`with` copies field values directly; it does not invoke the record's constructor. A constructor that validates cross-field invariants (e.g. `Width > 0 and Height > 0`) is not re-run during `with`."*

So the type the spec tells you to write is the type the spec will not let you constrain. That is the gap. It is not an oversight — it is a recorded deferral, twice — and this proposal is its cash-out.

### 1.2 Producer 1 — `default(T)` fabricates

§17.11 defines `default(T)` over *"Records all of whose declared fields are defaultable: the record value with each field at its zero value"*, and §3.6's *defaultable* row states the same structurally: *"a record all of whose declared fields are defaultable"*. Neither reads any member other than the storage components. A `Port` whose validity is `0 <= V <= 65535` is defaultable because `int` is, and `default(Port)` is a `Port` that met no constructor.

The fabrication is not merely a `default(T)` expression. §7.9's *Default initialization* paragraph makes it implicit — *"A field whose type is **defaultable** … satisfies the must-initialize rule vacuously and is implicitly initialized to that default"* — and §17.11's inline-array bullet extends it to every slot of `[N]Port`. The producer is the whole default-initialization surface, not one operator.

### 1.3 Producer 2 — `where T: new()` makes `new T()` mean `default(T)`

§14.2, in one sentence that is both the constraint's definition and the hole: *"A **value-type** substitution satisfies the constraint exactly when it is **defaultable** ([§17.11]). Inside the generic body, `new T()` for a value substitution is equivalent to `default(T)`: for records this is the zero-valued record (admitting `record Point(X: int, Y: int)` while rejecting records with non-nullable reference fields)…"*

So a generic body that looks like it is *constructing* a `T` — the most construction-shaped expression in the language — is fabricating storage. There is no author-side defence: constraint satisfaction is *"a property of the type alone, checked wherever an instantiation occurs"*, and the wrapper satisfies it.

### 1.4 Producer 3 — Try shape (c) mandates the fabricating producer

stdlib §1's Try convention sanctions three payload-extracting shapes, and shape (c) is defined by the fabrication: *"**(c) `bool` + non-nullable value-typed out** — `TryX(..., out result: T): bool` with `default(T)` assigned on the false path (spec §22.3) — for APIs whose payload type is a **concrete value type**…"*

A validating wrapper's most natural surface is a parse: `Port.TryParse(s, out p)`. Under shape (c) the false path assigns `default(Port)` — an invalid `Port` handed to the caller, distinguished only by a `bool` the caller may ignore. The convention is not at fault: shape (c) is correct for `int`, where every bit pattern is a value. It is wrong for any type with a predicate, and the convention has no way to know which is which — because the language has no way to say it.

Note the shape the answer already has. Shape (a)'s stated reason for its nullable out is *"because a class payload has no `default` to assign on the false path (spec §17.11) and a generic payload may be instantiated by a class"*. The rule that forces (a) is already **keyed on non-defaultability**; it simply has no non-defaultable value type to apply to except a choice.

### 1.5 Producer 4 — the §9.8 deserialize band mismatch, and its coverage gap

§9.8's synthesized `Deserialize` does the right thing where an explicit constructor exists: *"If the record declares one or more **explicit constructors**, the synthesized `Deserialize` calls the *unique* explicit constructor whose parameter list binds exactly the serialized components, matched by name"* — and §9.2 states the consequence plainly: *"`with` bypasses constructors ([§9.5]); the synthesized `Deserialize` runs a matching declared constructor ([§9.8])."*

Two defects remain. First, the **band**: the fault surfaces as `ContractViolationException`, which §22.16 and §31.4 define as a bug report — *"Programs must not use it for control flow — it reports a bug (a caller bug for `requires`, an implementation bug for `ensures`/`invariant`), not an outcome"* — raised at a boundary whose every other structural fault raises `SerializationException` (*"A **missing** required component (one neither present nor `serialized optional`), a **duplicate** field name, a type mismatch, or a name not matching any enum constant raises `SerializationException` unconditionally"*). Resolved at this proposal's §6.

Second, the **coverage**: §9.8 continues — *"A record that declares **no** explicit constructor is constructed directly from the read components at the storage level — the same constructor-bypassing construction that `with` ([§9.5]) and `default(T)` ([§17.11]) already perform"*. 0106 §5.3 left this deliberately unsettled, calling the synthesized `Deserialize` *"arguably a fourth"* instrumentation point; the answer is **covered where a constructor exists, and not otherwise**, so the path needs its own naming, which this proposal gives it (this proposal's §2.4).

### 1.6 Producer 5 — `serialized optional`, which no rule guards

Not among 0106's four; surfaced in this proposal's own verification pass. §9.8's customization vocabulary has two `default(T)`-supplying members, and only one of them is guarded:

- *"**`serialized skip`** — the component is excluded from both directions entirely. … the synthesized `Deserialize` does not read it and supplies the component's `default(T)` ([§17.11]) to the constructor. A `serialized skip` component whose type is not defaultable ([§17.11]) is a compile error at the conformance declaration (the synthesized `Deserialize` would have no value to construct with)."*
- *"**`serialized optional`** — read/write tolerance for that component. … On read, a missing field yields `default(T)` instead of raising `SerializationException`; a present field is read normally."* — **no defaultability sentence anywhere in the bullet.**

An `R`-typed component marked `serialized optional` on *any* enclosing type is therefore a `default(R)` producer that nothing in §9.8 stops. §17.11's catch-all arguably already reaches it — its enum paragraph rejects *"any other site that supplies a `default(T)` (e.g. [§9.8]'s `serialized skip`)"* — but the parenthetical names only `skip`, no diagnostic site is fixed, and §9.8 itself is silent. The same gap exists today for an enum with no zero-valued constant, so this is a pre-existing under-statement rather than a hole this proposal opens; it becomes load-bearing here because §9.8 calls the combined spelling *"the common real-world case (an externally-named field that may be absent)"*. The fix is one sentence and it is priced at this proposal's §4(3); the same wording covers a choice payload field, where §9.8's choice subsection likewise guards only *"`skip` requiring a **defaultable** ([§17.11]) payload type"*.

### 1.7 The price of doing nothing, at this proposal's own scope

0106 §6.2(d) priced the status quo with a fully-hardened `Port`; this proposal reproduces only what it needs. The hardened shape closes the *externally reachable* holes by hand: `private` storage closes `with` outside the declaring record (§9.5: *"A `with` target must be **accessible at the use site under the member's own declared accessibility**"*), and an explicit constructor closes §9.8's storage-level path. Both are real and both work. What neither closes is producers 1–3 and 5, because none of them names a member: `default(T)` reads storage, `new T()` is defined as `default(T)`, and shape (c) and `serialized optional` are conventions and clauses over the same operator. The status quo's residual is therefore not verbosity. It is that **the wrapper's one guarantee is false**, in five spellings, and the author cannot make it true.

---

## 2. The design, part (a) — invariant members in record bodies

### 2.1 The grammar already parses it

Appendix A.4:

```
Member          ::= FieldDecl | MethodDecl | PropertyDecl | IndexerDecl
                  | ConstructorDecl | DestructorDecl
                  | OperatorDecl | NestedTypeDecl | ConstDecl
                  | PolymorphicMember | InvariantDecl
…
InvariantDecl   ::= "static"? "invariant" "(" Expression ")" ";"
                              /* class bodies only; rejected semantically in
                                 records and static classes (§22.16); the `;` is
                                 the ordinary member terminator. */
```

and A.3: `RecordBody ::= Member* "end" Identifier? ";"`.

`InvariantDecl` is a `Member`; a `RecordBody` is `Member*`. **The production set is unchanged by this proposal** — `invariant(V >= 0);` inside a record body is already a well-formed parse, refused by a semantic rule whose text the grammar comment quotes. What changes is that rule, and the comment that advertises it. That makes the *syntactic* footprint zero; it does not make the whole change one line, and this draft no longer claims it does (this proposal's §0, cost paragraph).

Note also what the comment's precision implies: `Member*` bodies are exactly class bodies, record bodies, and static-class bodies. `EnumMember ::= MethodDecl | ExpressionPropertyDecl` and `InterfaceMember ::= IfaceMethod | IfaceProperty | IfaceIndexer | IfaceStaticMethod | IfaceStaticOperator` admit no `InvariantDecl` at all, so an invariant in an enum or interface body is a **syntax** error, not a semantic rejection — which is why the amendments below leave static classes as the only named semantic rejection, and leave §22.16's interface-invariant deferral (*"interface-declared invariants (need interface carriage to transport)"*) untouched and live.

### 2.2 The candidate normative text ▲

To be added to §22.16 as a subsection immediately after *Class invariants*:

> ▲ **Record invariants.** A **record** body may declare `invariant` members, under this section's clause rules, in the context of the declaring record — its `private` storage readable, `old` and `result` rejected, the receiver a borrow ([§9.4](#sec-value-receivers-are-borrows)). The **record invariant** of `R` is the conjunction of `R`'s invariant members in declaration order; a record does not inherit ([§9.6](#sec-records-and-inheritance)), so no base conjunction arises. As for a class, the invariant is an *observable-boundary* property: it holds of every value of `R` that any code can name.
>
> **A borrow-bearing record ([§9.7](#sec-records-and-disposal)) may not declare an `invariant` member** — a compile error at the member, naming the borrow-carrying field. An invariant is a standing claim about a value, and a record's reference-typed fields are borrows of state its *source* may change at any time through paths this section instruments nowhere; a member body may itself store through such a field ([§9.4](#sec-value-receivers-are-borrows)'s mediating shape, whose worked instances are stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)'s span setters). Runtime-managed reference components — `string`, function types, boxed value types — do not make a record borrow-bearing ([§9.7](#sec-records-and-disposal)) and are unaffected: their values are immutable and unbounded in lifetime ([§28.9](#sec-runtime-managed-types), [§8.6](#sec-boxing)), so a clause reading them observes the value it was constructed with.
>
> **The instrumentation set** — the record invariant is evaluated at exactly these points, in checked-contracts mode:
>
> - **Construction exit**: the normal completion of a constructor of `R`, of any visibility — the auto-generated primary constructor of a positional record ([§9.1](#sec-positional-records)) included — after its body and its `ensures` clauses. A `this(...)`-chained-to constructor completing mid-chain is not an instrumentation point.
> - **`with`-update completion** ([§9.5](#sec-non-destructive-update-with)): after the copy's replaced components are stored and before the result value is produced. A `with` expression invokes no constructor, so this is a distinct point; a violation raises `ContractViolationException` at the `with` expression.
> - **Normal exit of every `public` auto-property setter** — the synthesized setter an external store through a named binding invokes ([§9.4](#sec-value-receivers-are-borrows)). (Assignments to the record's storage inside a constructor are backing-field stores, not accessor invocations, so construction does not double-check.)
> - **Storage-level construction performed by the language on the type's behalf**: the synthesized `Deserialize` of a record declaring no explicit constructor ([§9.8](#sec-synthesized-deserialize)), evaluated where that construction completes. Where the record declares an explicit constructor, §9.8 calls it and the construction-exit point above applies unchanged; §9.8 fixes how a violation at either point is reported.
> - **Never instrumented**: member *entry* of any kind, non-public setters, methods, block-form and indexer setters, and static members. The omissions are consequences, not choices. Entry checking is redundant: every producer of a value of `R` is an instrumented point or a copy of one, so no member can be entered with a value that has not already been checked. Methods and author-written setters cannot falsify the invariant: a store landing in the receiver's own storage is a compile error at the member body ([§9.4](#sec-value-receivers-are-borrows)), and the mediating store that section legalises needs a class- or interface-typed field, which an invariant-bearing record does not have (above). `set(owned)` cannot arise on a record ([§9.7](#sec-records-and-disposal)). Consequently the class rule's public-method points, and its recorded mutating-getter residual, have no record analogue.
>
> **Records carry no other producer of a value.** `default(T)` and `new T()` at a value substitution ([§17.11](#sec-the-default-operator), [§14.2](#sec-constraints)), default initialization of storage ([§7.9](#sec-definite-assignment-for-non-nullable-fields), [§15.1](#sec-inline-arrays)), and [§9.8](#sec-field-name-customization)'s `serialized skip` and `serialized optional` reconstructions do not construct a record declaring an invariant member: such a record is not **defaultable** ([§3.6](#sec-derived-type-properties), [§17.11](#sec-the-default-operator)), so each of those sites is rejected statically. A *copy* of a record value — assignment, argument binding, a `with` source, an `old` capture, a collection slot store — reproduces a value that satisfied the invariant where it was constructed, and is not an instrumentation point.
>
> **The FFI residual, recorded.** A record value produced or altered by foreign code reaches no instrumentation point. [§32.13](#sec-c-compatible-records)'s typed-handle admission is direct in **parameter, return, `ref`/`out`, and callback positions**, so an `extern` returning `R` by value, an `out R` or `ref R` written by native code, and a callback parameter alike mint or alter a value with no instrumented producer; the same holds for a C-compatible record passed by address ([§32.5](#sec-ffi-compatible-types)) and for an inline-array component passed by address at an `extern` parameter ([§32.5](#sec-ffi-compatible-types)/[§15.1.3](#sec-inline-array-ffi)), whose native writes land in the record's own storage. These stand with the obligations [§32.10](#sec-boundary-contract)'s boundary contract assigns to the programmer rather than to the compiler.
>
> **Tier.** A record invariant is **checked** by default, like any authored clause, and is discharged where this section already says an `invariant` is discharged — at the **declaring body**, which for a record means each of its constructors. `static invariant` parses and demands the proven tier as this section already provides; what a `static` record invariant would mean at the `with`-update point — the one instrumentation point with no declaring body — is **left open and recorded**, no clause admitted by the current prover floor being able to reach it.

Two sentence amendments in the same section ▲:

> ▲ §22.16 *Scope*: *"… record and enum methods (`old` over receiver fields captures by the value rules; the `invariant` member is admitted in **class and record** bodies — see *Record invariants* above — and remains a compile error in a static class)"*.
>
> ▲ §22.16 *Class invariants*, out-of-scope list: strike *"record invariants (constructor-exit plus a `with`-update instrumentation point — a later design);"*. The remaining items — interface-declared invariants (a live deferral pending carriage), static-class and static-state invariants, and `static invariant` parsing early — are unchanged.

### 2.3 The instrumentation set, derived rather than asserted

The set above is not a design choice per point; each point is forced, and the argument is worth stating because it is what keeps the feature small — and because the first draft of this proposal got it wrong (this proposal's §12).

**Every route by which an invariant-bearing record's storage can come to hold a value is one of five things**: (i) a constructor call, (ii) a `with` expression, (iii) an external store through a settable auto-property — §9.4: *"an external store through a named binding's lvalue chain — `r.X := 5` on a settable auto-property — remains legal, the store landing in `r`'s storage… What runs there is the **synthesized** auto-property setter"*, (iv) a language-performed storage-level construction (`default`, `with`'s copy, §9.8's constructor-less path — the three §9.8 itself groups), and (v) a copy of an existing value. (i)–(iii) are instrumented; (iv) is either instrumented (§9.8) or statically removed (`default`); (v) needs nothing.

**Author-written member bodies are not a sixth route, but only because of (a′).** §9.4 rejects stores landing in the receiver's own storage *and in the same paragraph legalises the mediating store*: *"a chain that hops **through a class- or interface-typed member leaves the receiver**: a record's reference-typed fields are borrows ([§9.7]), so a store beyond such a hop lands in the borrowed heap object, not in the receiver, and is **legal** — the sound **mediating** shape, and the answer to what a record accessor body can do."* On a borrow-bearing record that is a live falsification route for any clause reading through such a field, and — worse — the borrow's *source* can mutate the same state with the record entirely uninvolved. Restriction (a′) removes the field kind that makes both possible, and only then does "no member body can falsify the invariant" hold. With (a′) in force the record instrumentation set is genuinely **smaller than the class one**: no public-method points, no entry points, no getter residual.

**A `ref`/`out` argument is not a route either.** §22.3 admits *"a mutable local variable, a writable field, an indexer slot, or a property with an accessible setter"*, and its general rule for accessor targets is *"An **indexer slot or property** target is instead **copy-in / copy-out** under [§17.6]'s evaluate-once discipline"* — so `ref r.X` over an auto-property runs the setter (instrumented), and `ref r` over the whole value assigns a whole record value (a copy of something already valid). The only uninstrumented spelling left is `ref` over a **public writable field**, which the candidate text handles under §4's companion rule.

### 2.4 The deserialize path — the answer to 0106's open verification

0106 asked whether the §9.8 path is already covered by constructor-exit instrumentation. **Partly.** §9.8 selects an explicit constructor when one exists and constructs at the storage level when none does. The former is covered with no new text; the latter is a language-performed construction that runs no user code and would otherwise reintroduce producer 1 through the wire.

Naming it costs one bullet and needs no new machinery: the synthesized `Deserialize` already constructs *"at a static program point, so a record's field types, nullability, and definite-assignment rules are checked normally"* — the check goes where those checks go. The band in which a violation is reported is this proposal's §6.

### 2.5 Positional records — the spelling, decided

0106 §6.2(d′) item 3 left an option open: *"Optionally, a clause slot on the record declaration so a positional record can constrain its generated primary constructor without being rewritten into body form."* Appendix A.3's `RecordDecl` admits no `ContractClause`.

**Declined, and the reason is §9.3.** A positional record does not have to be *"rewritten into body form"* to gain a body: §9.3's **combined form** already admits both — *"A record may have both positional fields and a body … The auto-generated members are present and the body adds further members."* The delta for the author is two lines:

```bezel
record Port(Value: int);                     // today

record Port(Value: int)                      // with an invariant — §9.3's combined form,
  invariant(Value >= 0 and Value <= 65535);  //   no new grammar, primary constructor intact
end Port;
```

The generated primary constructor is untouched, its positional properties are untouched, and the constructor-exit point catches it — the candidate text names the auto-generated primary constructor explicitly for this reason. Against that, a `ContractClause`-on-`RecordDecl` slot would buy two lines of brevity at the price of a **second spelling of one meaning** (a record could then carry an invariant in two places, with a conjunction order to define), a grammar production this proposal otherwise does not need, and an asymmetry with classes, which have no such slot either.

*Recorded as a choice, not a fact:* this is a spelling judgement the spec does not decide, so it is put to the owner (this proposal's §11, Q2).

### 2.6 Modes, and the one tier question left open

§22.16 puts `invariant` under the checked-contracts mode: *"in **unchecked (release) mode** — the default — unproven `ensures` and `invariant` clauses are not evaluated"*, while *"`requires` clauses are outside the mode entirely"*. This proposal **inherits that mode unchanged** for records, and the consequence must be stated at the front rather than buried:

- The **static** half — non-defaultability, and everything it closes — is a §3.6 derived property computed from structure at compile time. **It holds in every build mode.** It is not a check that can be turned off.
- The **dynamic** half evaluates in checked-contracts mode, exactly as a class invariant does. In release mode an author who wants an always-on check writes it where the language already puts always-on checks: a `requires` on the constructor, which is the status-quo hardened pattern (this proposal's §1.7) and remains available and unchanged.

Two readings were available and one is rejected: making record invariants always-on would buy release-mode enforcement of the `with` point at the price of splitting the `invariant` member's meaning across kinds. Uniformity wins; the divergence is put to the owner as Q3, because the argument is not one-sided.

**The recorded tier question.** §22.16 is categorical that *"An `ensures` or `invariant` clause is discharged at the **declaring body**"*, and only a `requires` discharges at *"every call site"*. Three of the four record instrumentation points have a declaring body; the `with` point has none — the copy is built at the client's expression. An earlier draft of this proposal resolved that by giving one clause kind two discharge sites by clause role; that is new tier machinery presented as derived, and it is withdrawn. It is also unexercisable today: the prover floor admits only a constant expression folding to `true` and the §22.4 `when` family, so no `static invariant(Value >= 0 and Value <= 65535)` exists to discharge anywhere. The candidate text therefore **records the question** and leaves the proven tier to reach records when the D.2 fragment lands. This is deliberately not an owner question: nothing can exercise it, so there is nothing to rule.

---

## 3. The design, part (b) — non-defaultability as a §3.6 consequence

### 3.1 The candidate clauses ▲

§3.6's *defaultable* row, amended (additions in **bold**):

> ▲ **defaultable** — *"types for which `default(T)` is defined: every nullable type `T?`; and every non-nullable value type whose components are recursively defaultable — the primitives, the unit type, an enum **declaring a zero-valued constant**, a record all of whose declared fields are defaultable **and which declares no `invariant` member ([§22.16](#sec-contract-clauses))**, an inline array whose element type is defaultable (the owning forms `[N owned]T?` included). Not defaultable: non-nullable reference types; **every choice type** ([§35.10](#sec-choice-default-with)); an enum with no zero-valued constant; **a record declaring an `invariant` member**; and any record or inline array with a non-defaultable component"*.

§17.11, correspondingly ▲ — one clause in the records bullet, one entry in the compile-error list, spelled on the model of the enum entry already there:

> ▲ *"Records all of whose declared fields are defaultable **and declaring no `invariant` member ([§22.16](#sec-contract-clauses))**: the record value with each field at its zero value. …"*
>
> ▲ (compile-error list) *"A record declaring an `invariant` member ([§22.16](#sec-contract-clauses)). Every value of such a record satisfies its invariant at one of §22.16's instrumentation points, and `default(T)` constructs at none of them; a fabricated zero value would be a value of the type that the type's own predicate never saw. For such a record, `default(R)`, `new T()` at such a substitution ([§14.2](#sec-constraints)), default initialization of `R`-typed storage ([§7.9](#sec-definite-assignment-for-non-nullable-fields), [§15.1](#sec-inline-arrays)), and any other site that supplies a `default(T)` ([§9.8](#sec-serialization-synthesis)'s `serialized skip` and `serialized optional`) are rejected. The remedy is a constructor or factory call — or, where a zero value is genuinely wanted, `R?` and `default(R?)`."*

That second paragraph is deliberately a near-transcription of §17.11's existing enum paragraph — *"for such an enum, `default(E)`, `new T()` at such a substitution ([§14.2]), default initialization of enum-typed storage ([§7.9], [§15.1]), and any other site that supplies a `default(T)` (e.g. [§9.8]'s `serialized skip`) are rejected. The remedy is declaring a zero-valued constant."* The enumeration of affected sites is not new work: **the spec has already written it once**, for the structurally identical case. The one change of substance is that this proposal's version names `serialized optional` beside `serialized skip`, which the enum paragraph does not (this proposal's §1.6).

### 3.2 The precedent is inside §3.6's own row

0106's verification is confirmed against the v0.262 text. The *defaultable* row admits *"an enum **declaring a zero-valued constant**"* and excludes *"an enum with no zero-valued constant"*. That is:

- **member-level** — the property turns on the presence of one declared member, not on the type's kind;
- **within one kind** — two enums, same kind, opposite verdicts;
- **author-flippable in both directions** — adding a zero-valued constant grants defaultability; deleting it withdraws it;
- **already normative**, in the very row this proposal amends, so the amendment introduces no new *shape* of clause.

§17.11's *"no choice type is defaultable"* is the secondary precedent — it keys on the type **kind**, so it shows a whole kind being non-defaultable rather than a member flipping the property within one — and it earns its citation elsewhere (this proposal's §5.4): the choice already occupies the exact lattice position this proposal puts records into.

### 3.3 The `invariant(true)` counter-reading, and its answer

The counter-reading is §3.6 rule 1: *"**Computed, never declared.** A derived type property cannot be asserted, opted into, or waived by annotation. It follows from structure; the only way to change it is to change the structure."* Under this proposal an author obtains non-defaultability by *writing a member* and waives it by deleting one — and in the degenerate `invariant(true)` the member has no other observable effect at all, because §22.16's prover floor makes a constant expression folding to `true` **proven**, and proven means *"discharged statically at the clause's discharge site, never evaluated at runtime, zero cost in every mode"*. So the objection is at its strongest exactly where a hand-waving answer is most tempting: for `invariant(true)` the §3.6 flip really is the member's entire observable content.

**The answer is the precedent, and it is decisive on its own.** A zero-valued enum constant is *also* a member an author may add for no reason other than to obtain defaultability — `enum Mode  Unset := 0, Fast := 1  end Mode;`, where `Unset` exists solely so `default(Mode)` compiles, and where deleting it withdraws the property again. That is opting into a derived property by adding a member; it is normative today; and it sits in the **same table cell** as the clause this proposal adds. Whatever reading of rule 1 forbids `invariant(true)` forbids `Unset := 0`, and the register has already ruled the other way. Nor is the shape confined to that row: §3.6 rule 3's own examples of property flips are all **member edits** — *"adding a destructor flips trivial-dispose; adding a computed property in the wrong form flips auto-hashable; adding or removing `equality skip` on a member ([§9.2]) flips comparison results and hash values for clients and can flip auto-hashable/hashable"* — so the "structure" rule 1 speaks of demonstrably includes which members a type declares.

Two weaker arguments are named and *not* relied on, because a rationale entry drafted from this section (this proposal's §10) would otherwise carry them. First, "an invariant member has independent checked effect, so it is not a mere annotation": true of every real predicate and **false of `invariant(true)`**, which is the case under attack, by the prover-floor sentence quoted above. Second, "§22.16 already tolerates `requires(true)`": true, but a `requires(true)` flips no derived type property, so it answers a different objection. The honest position is that the degenerate case is admitted **because the register already admits its twin**, not because it is harmless.

### 3.4 What the property does not depend on

The *defaultable* verdict reads **only** whether an `invariant` member is declared. It does not read the clause's tier (a `static invariant` and a checked one flip it identically), the build's contract mode (§3.6 properties are compile-time and mode-independent), whether the predicate is satisfiable, or whether the record also declares a validating constructor. This keeps the property a pure structural predicate, per rule 1, and keeps the diagnostic writable in one sentence, per rule 4.

---

## 4. Companion amendments the feature requires

Five are mandatory, two are optional. The first is the one that would bite if missed.

**(1) §9.2's auto-equality membership list ▲ — mandatory, and a trap if omitted.** §9.2 grants auto-equality when *"every **instance** member of the record **not marked `equality skip`** … is one of"* four things: an auto-property of admissible type, *"A method or expression-bodied (computed) property"*, *"An overload of a non-equality operator"*, *"A constructor"*. An `invariant` member is none of them. Under the letter of the rule, **adding an invariant member would silently strip a record of auto-equality** — and because §9.8's body-record eligibility is defined as *"exactly the auto-equality membership conditions of [§9.2] — the §9.2 member list and admissible-component rule are the single normative statement, applied here unchanged"*, it would strip serialization synthesis with it, at the same stroke, with the equality failure surfacing at the *use* site rather than the declaration. Both are §3.6 derived properties whose loss §2.8.6 classifies as breaking. The fix is one bullet:

> ▲ (§9.2 membership list) *"— An `invariant` member ([§22.16](#sec-contract-clauses)). It declares no storage and is not a callable member; like a method it neither qualifies nor disqualifies the record, and it does not participate in equality, hashing, `ToString`, or serialization."*

With that bullet the interaction is genuinely zero.

**(2) §2.8.2's contract-record carriage ▲ — mandatory, one phrase.** The carriage sentence reads *"for every class it describes, one record per `invariant` member, base classes' records first ([§22.16])"*. It must become *"for every class **or record** it describes, one record per `invariant` member, base classes' records first (a record does not inherit, [§9.6])"*. Everything else at §2.8.2 is already correct: clause 1 already names *"the contract clauses and `invariant` members ([§22.16]; carried as **contract records** per the paragraph below)"* and already carries *"the **derived type properties**"*, so an importing module sees both the clause and the flipped `defaultable` verdict with no further change.

**(3) §9.8's `serialized optional` guard ▲ — mandatory (this proposal's §1.6).** One sentence, mirroring the `skip` bullet's, in the `optional` bullet:

> ▲ *"A `serialized optional` component whose type is not defaultable ([§17.11](#sec-the-default-operator)) is a compile error at the conformance declaration (a missing field would have no value to supply). The same holds for a choice payload field marked `serialized optional`."*

The diagnostic site is the conformance declaration, identical to `skip`'s, for §9.8's own stated reason (*"the conformance declaration is the explicit opt-in and the obstruction should be reported where the promise is made"*). This is a **repair of a pre-existing gap**, not a new prohibition: §17.11's catch-all already rejects *"any other site that supplies a `default(T)`"*, so an enum with no zero-valued constant marked `serialized optional` is arguably illegal today with no diagnostic site fixed. The remedy the diagnostic should name is `R?`: a nullable component is defaultable, and `serialized optional` on it behaves exactly as authors expect.

**(4) Appendix A.4's `InvariantDecl` comment ▲ — mandatory, comment-only.** *"class and record bodies; rejected semantically in static classes (§22.16)"*. No production changes, and enums and interfaces are deliberately not named: `InvariantDecl` is unreachable in `EnumMember` and `InterfaceMember`, so it is a syntax error there (this proposal's §2.1).

**(5) The public-writable-field rule ▲ — mandatory, and it belongs in §22.16 beside the invariant.** A **public writable field** (§9.2's explicit field declaration) on a record declaring an invariant member is a compile error: an external store to it invokes no accessor, so no instrumentation point can attach, and §22.3 admits it as a `ref`/`out` target that aliases directly rather than bracketing through a setter. The diagnostic names the remedy, an auto-property.

**(6) §9.5's advice paragraph ▲ — optional but recommended.** Today: *"If a record enforces invariants that may be violated by independent field replacement, the record should provide a named method for constrained updates rather than relying on `with`."* Under this proposal that advice is superseded for the invariant-bearing case, and leaving it would read as a live recommendation to avoid a now-checked operation. Keep the sentence for records whose validity lives only in a constructor `requires`; add that a record declaring an `invariant` member has its `with` updates checked at §22.16's `with`-update point.

**(7) §2.8.6's `invariant`-added row ▲ — optional.** Adding an invariant member to a public record produces two deltas: the row *"`invariant` added"* → **preserving** (*"A strengthened observable guarantee."*, with a *"(subclasser note as for ensures-added)"* qualifier that is inert for records, which have no subclasses), and the row *"derived-property verdict lost; element category changed"* → **breaking**. §2.8.6 classifies a delta *"**identical**, **preserving**, **widening**, or **breaking**, in that severity order"*, so the artifact verdict is correctly **breaking** with no amendment; a parenthetical on the `invariant`-added row naming the record case would spare a reader of that row alone the wrong conclusion.

**Not required: the stdlib.** See this proposal's §6 — the decode-band ruling is spelled so that `SerializationException` needs no new declared surface.

---

## 5. Consequences, worked

| Site | Today | Under this proposal | Mechanism |
|---|---|---|---|
| `default(R)` | a fabricated value | **compile error** | §17.11's non-defaultable list (this proposal's §3.1) |
| default initialization of `R`-typed storage; `[N]R` slots | implicit zero value | **compile error unless explicitly initialized** | §7.9's *Default initialization*; §17.11's inline-array bullet — no new rule |
| `where T: new()` at `T = R` | admitted; `new T()` fabricates | **rejected at the instantiation site** | §14.2's definition + §3.6 rule 2 (this proposal's §5.1) |
| `where T: value` at `T = R` | admitted | **admitted, unchanged** | §14.2's `value` row (this proposal's §5.4) |
| Try shape (c) over `R` | `default(R)` on the false path | **unwritable; shape (a) forced** | stdlib §1's shapes (this proposal's §5.2) |
| `serialized skip` on an `R` component | already an error for non-defaultables | **error, by the existing rule** | §9.8's skip guard (this proposal's §5.3) |
| `serialized optional` on an `R` component | **unguarded — fabricates** | **error, by a new guard** | §4(3) — new text (this proposal's §5.3) |
| stdlib §4.14 `OwningBuffer<R>(length)` | admitted iff `R` defaultable | **inadmissible; two-argument `init` form required** | existing stdlib text, no change (this proposal's §5.3) |
| a **borrow-bearing** record | — | **may not declare an invariant member** | restriction (a′) (this proposal's §2.2, §5.9) |
| public writable field on such a record | — | **compile error** | §4(5) |
| synthesized `Deserialize` of `R` | validates iff an explicit ctor exists; wrong band | **validates always; band ruled** | this proposal's §2.4 and this proposal's §6 |
| auto-equality / hashing / `ToString` | — | **unchanged**, given §4(1) | §9.2 |
| patterns and exhaustiveness (§19) | — | **no interaction** | patterns destructure, never construct (this proposal's §5.5) |
| artifact interface, §2.8.6 verdicts | — | carried; flip is **breaking** | §2.8.2 + §2.8.6 severity order (this proposal's §5.6) |
| FFI (§32.13) | — | admissible; **residual recorded over the whole position list** | this proposal's §2.2 and this proposal's §5.7 |
| overflow inside a clause (§6.1) | — | propagates **as itself** | §22.16's faults-during-clause-evaluation rule (this proposal's §5.8) |

### 5.1 `where T: new()` — the rejection lands where §3.6 says it should

No new rule is written. §14.2 defines value-type satisfaction *as* defaultability; the record stops being defaultable; the constraint stops being satisfied. The diagnostic pattern is §3.6 rule 2's — *"an instantiation that violates a property-dependent requirement is rejected *at the instantiation site*, with a diagnostic naming the property, the offending type argument, and the structural reason"* — and §17.11 already closes with the same sentence for the general case: *"A substitution that is not **defaultable** — an enum with no zero-valued constant included — is rejected at the instantiation site ([§3.6] rule 2's per-instantiation evaluation; the diagnostic names the property per rule 4)."* The structural reason rule 2 demands is one phrase: *R declares an `invariant` member*.

### 5.2 Try shape (c) → shape (a)

Shape (c)'s definition *is* `default(T)` on the false path, so an invariant-bearing `R` cannot inhabit it: the false path has no value to assign, and §22.3 requires every `out` assigned on every normal return path. The author writes shape (a) — *"`TryX(..., out item: T?): bool`"* — whose false path assigns `null`, with the `when true` postcondition narrowing the recipient in the true region.

This requires no stdlib edit, because **no landed stdlib member is affected**: shape (c)'s enumerated inhabitants are the primitive `TryParse` family, `Convert.TryTo*`, the `DateTime`/`DateOnly`/`TimeOnly`/`Guid` parsers, and the §4.14/§4.15 `TryRead*` accessors — every one over a type with no predicate. The forcing applies to *future* surfaces, and in the direction stdlib §1 already reasons: shape (a) exists for payloads that have *"no `default` to assign on the false path"*. The `*OrNull` tier and the throwing tier are unchanged, and the no-dual rule is not engaged (one stance, one spelling).

### 5.3 The three serialization-adjacent sites, separated

They do not behave alike, and the first draft of this proposal wrongly said they did.

- **`serialized skip` — zero new text.** §9.8 already reads: *"A `serialized skip` component whose type is not defaultable ([§17.11]) is a compile error at the conformance declaration (the synthesized `Deserialize` would have no value to construct with)."* An invariant-bearing component inherits that error verbatim, and the message already names the right property.
- **`serialized optional` — new text required**, per §4(3). Without it the commonest tolerant-field spelling silently fabricates. With it, the remedy is `R?`.
- **stdlib §4.14's `OwningBuffer<T>(length)` — closed by existing stdlib text, and a supporting data point.** *"The single-argument constructor requires T to have a default value (every primitive does; records with default-able fields do; enums with a zero-equivalent member do; user-defined types without defaults do not). It initializes every slot to `default(T)`. … For types without a default, only the two-argument constructor with an explicit init function is admitted."* At `T = R` the one-argument form becomes inadmissible and the `init`-function form is what remains — which is the correct outcome, reached by a rule written years before this proposal, over a sentence that already contemplates *"user-defined types without defaults"*.

### 5.4 Generics beyond `new()` — the position is already inhabited

`where T: value` is unaffected: its row admits *"Non-nullable value types — records, enums, choices ([§35]), primitives, inline arrays (all forms), the unit type"* — membership by kind, with no defaultability conjunct. `notnull`, `hashable`, `sendable`, and interface-bound constraints are likewise untouched.

The one place worth tracing is §17.11's generic-body bullet *"Under `where T: value`, `default(T)` is always non-null"*. That is a statement about the *nullability* of the result, not a promise of admissibility — and it cannot be one, because **choices already satisfy `value` and are never defaultable** (§14.2: *"**Choices never satisfy the value-type arm** — a consequence of the property: no choice is defaultable"*). A body writing `default(T)` under `where T: value` therefore already carries per-instantiation rejection risk today, and every rule that had to cope with a *value-satisfying, non-defaultable* type has already coped. This proposal adds a second inhabitant to an occupied cell rather than opening an empty one.

### 5.5 Patterns and exhaustiveness (§19) — no interaction

§19.3's record destructuring *"matches the type and binds positional fields by position"*; the named form's reads are *"a record field copy/borrow"*. Patterns **destructure; they never construct**, so no pattern is a producer. Exhaustiveness (§19.6) counts declared enum constants, choice cases, and closed-hierarchy leaves — records are none of these. One benign nuance: §19.3 rejects *"a constant slot whose declared type does not bear `=` (e.g. a non-auto-hashable record, [§9.2])"*, which §4(1) keeps from being triggered by the invariant member.

### 5.6 The artifact interface and change classification

§2.8.2 clause 1 already carries invariant members and derived-property verdicts, so a consumer sees both facts after §4(2). §3.6 rule 3 makes the flip *"a breaking change for clients relying on it, exactly as a signature change is"* — correct and desirable: a client generic instantiated at `where T: new()`, or a client field relying on default initialization, stops compiling, which is the point. Contract-record identity is by canonical text, so a re-spelled invariant is a different clause and classifies as breaking — again correct.

The practical guidance for a library author is worth one sentence in the eventual guide pass: **add the invariant when the type is born**, not in v2.

### 5.7 FFI

§32.13's typed-handle admission (*"A C-compatible record with exactly one field, of *integer or pointer* scalar type … is FFI-compatible **directly** — parameter, return, `ref`/`out`, and callback positions"*) is exactly the wrapper shape this proposal serves, so invariant-bearing records will cross the boundary. The classification itself is unaffected — C-compatibility is computed from field order and field types. The residual is stated in the candidate text over that whole position list, plus by-address records (§32.5) and by-address inline-array components (§15.1.3), rather than legislated away: it falls on the programmer-responsibility side of §32.10's boundary contract, where the compiler's guarantees stop, and stdlib §10's `import System.Interop` audit marker still applies at the call site.

### 5.8 Overflow, and other faults inside a clause

§22.16 already rules this and this proposal changes nothing: *"The fault propagates from the clause's evaluation point **as itself** — never converted to `ContractViolationException`, because the clause did not evaluate to `false`; it failed to evaluate, which is a defect in the *contract*, and the original exception is its diagnosis."* An `OverflowException` from an invariant clause propagates as itself at every instrumentation point — **including inside the synthesized `Deserialize`**, where this proposal's §6 band rule deliberately does *not* reach it: §6 converts a clause that answered *false* (an input verdict), never a clause that broke (a program defect whose diagnosis is the original exception).

### 5.9 What is *not* closed, stated plainly

Five gaps survive, each recorded above, none silent: release-mode evaluation of the dynamic half (this proposal's §2.6); the FFI boundary (this proposal's §5.7); **borrow-bearing records, which may not carry an invariant at all** (restriction (a′)) — the honest statement of a guarantee the language cannot provide over borrowed state, not a hole; a record whose validity depends on state outside the value, for the same reason; and the `Range<T>`-shaped spelling of a range predicate, which is a stdlib fact ruling that 0106 slate item iii resolves (this proposal's §9).

**Pricing (a′) against the corpus and the library.** The records that (a′) excludes are the borrow-bearing ones, and they are almost entirely intrinsic: stdlib §4.14/§4.15's span family and `CancellationToken` (whose §9.7 static-field exception exists for `CancellationToken.None`). In `tests/conformance/cases` the user-authored borrow-bearing records are three event records — `ClickedEvent(Sender: Button, …)`, `ValueChangedEvent(Sender: Slider, …)`, `DataReceivedEvent(Sender: Connection, Payload: Array<byte>, …)` — none of which is a validation candidate; **none of the five wrapper records the corpus does contain is borrow-bearing** (this proposal's §7). The restriction is free for the population the feature exists to serve.

---

## 6. The deserialize band — resolved

**The question.** When the synthesized `Deserialize` constructs a record and a contract clause reports `false` — the selected constructor's `requires`, or the record invariant at its instrumentation point — which exception reaches the caller: `ContractViolationException` (what §22.16 raises) or `SerializationException` (what every other structural fault of that read raises)?

**The resolution: `SerializationException`, raised by the synthesized code, recording the `ContractViolationException` as its cause.** Candidate text ▲ for §9.8's *Synthesized `Deserialize`* subsection:

> ▲ *"A contract clause evaluating `false` during the construction step — a `requires` clause of the selected constructor, or the record invariant at its instrumentation point ([§22.16](#sec-contract-clauses)) — raises `SerializationException` from the synthesized code, by the same implementation-internal route as the missing-, duplicate-, and mismatched-component faults above, with the `ContractViolationException` recorded in the raised exception's `CauseChain` in [§31.4](#sec-standard-exception-types)'s snapshot form (type name, message, and stack-trace text — so the clause kind, the member, and the clause's source text survive into logs and `ToString()`). The input is data, not a caller's broken promise: a decoded value that fails the type's own validation is a structural fault in the input, classified with the faults above. A clause that **faults** rather than evaluating `false` is not covered by this rule and propagates as itself ([§22.16](#sec-contract-clauses)) — the failure is a defect in the contract, and the original exception is its diagnosis."*

**Why.** Four reasons, in decreasing weight.

1. **stdlib §1's band discipline puts it there.** Band 1 is *"something a correct caller already guarantees"* and its surface *"is never a recovery surface: its exception exists to report a bug, not to be branched on"*. Nobody's correct-caller obligation covers the contents of a byte stream. Band 4 is *"Boundary-consumed fault … the fault is logged, converted to a protocol answer, retried, or reported"* — and stdlib §2.15 already classifies this surface's read-side conditions there in as many words: *"in band 4 under the rule's second clause: they are band-3-shaped — expected at the decode boundary, information-bearing — but no value-like error-payload type yet exists to carry them (the R3/R2 sequencing recorded at v0.229), so the typed exception is the mechanism…"*
2. **§22.16's own boundary sentence says so.** *"input whose invalidity is a normal outcome at a trust boundary is handled with Try shapes, nullable returns, or `choice` returns, never with `requires`; a `requires` states what a correct caller already guarantees, and its violation is a bug report, not a result."* Leaving the raw `ContractViolationException` at the decode boundary makes the deserializer a caller that violated a precondition it could not have checked.
3. **It makes one read fault one way.** A caller decoding untrusted JSON writes one `catch SerializationException`: missing field, duplicate field, type mismatch, unknown field under strict policy, and *invalid value* all arrive through one arm, each locatable through the same `Offset`/`Line`/`Column` surface. Under the alternative the caller must additionally catch the language's bug-report exception — which §31.4 forbids using for control flow.
4. **It needs no new declared surface, because the raising is already compiler-internal.** This is where an earlier draft of this proposal was wrong, and the correction matters. §31.4's `(message, cause)` chaining constructor is declared on **`Exception`**, constructors are not inherited (§31.4: *"User subclasses chain by calling the base constructor: `public constructor(msg: string, cause: Exception) base(msg, cause) end constructor;`"*), and `SerializationException`'s declared surface in stdlib §2.15 is three readonly properties and **no constructor at all**. So the wrap cannot be spelled as a user-visible constructor call without amending the stdlib. It does not need to be: §9.8's synthesized `Deserialize` already raises `SerializationException` for missing and duplicate components with no constructor named anywhere, so the candidate text above says *"by the same implementation-internal route"* and normatively fixes what the raised instance carries. The read side needs nothing new — *"Cause chaining is by snapshot … the `(message, cause)` constructor **borrows** `cause` for the duration of the call and copies its type name, message, stack-trace text, and its own `CauseChain` into the new exception's `CauseChain` (outermost cause first, flattened)"*, and `CauseChain` is an inherited property.

**The cost, stated.** This changes behaviour that exists today: a record whose explicit constructor carries `requires` currently surfaces `ContractViolationException` through `Deserialize`, and would now surface `SerializationException`. That is the pre-existing misfit 0106 §6.2(d) residual (4) named, and fixing it is the point; stdlib §1's own scoping sentence — *"an existing spelling stands until its surface is re-ruled"* — makes re-ruling the sanctioned move. With zero external users and (this proposal's §7) zero corpus instances, the migration is empty.

**Confidence: medium-high.** The band classification is high-confidence (the discipline is written down and this surface is already classified there). The contestable half is the *conversion*: it would be the first place in the language where a `ContractViolationException` is not propagated, and a reviewer could reasonably prefer either that the check be spelled as a distinct decode-time validation step, or that `SerializationException` gain a chaining constructor in the stdlib and the wrap be user-visible (this proposal's §8(g)). Put to the owner as Q1.

---

## 7. Migration and the corpus

**Nothing breaks at the landing.** The feature lifts a rejection (a program that compiles today has no `invariant` member in a record body) and changes a derived property only for types that adopt the new member. **The newly-rejected surface is exactly:** `default(R)` / default initialization of `R`-typed storage / an `R` inline-array element default / `where T: new()` at `T = R` / Try shape (c) over `R` / `serialized skip` on an `R` component / `serialized optional` on an `R` component / stdlib §4.14's one-argument `OwningBuffer<R>(length)` — *where `R` is a record that adds an invariant member* — plus, for any record that adds one, a public writable field (§4(5)) and, for borrow-bearing records, the invariant member itself (restriction (a′)). Since no record has an invariant member before this proposal lands, the set of programs broken **by the landing itself is empty**; breakage is only ever opt-in.

Two changes are *not* opt-in and are named: the §6 band ruling (corpus incidence zero, below), and §4(3)'s `serialized optional` guard, which is a rejection for any *existing* type marking a non-defaultable component `optional` — an enum with no zero-valued constant is the only such component available today, and the corpus contains none (the one corpus use of the clause, `stdlib_0021.bzl`'s `DateTime? serialized "last_seen" optional`, is over a nullable component and stays legal, every nullable type being defaultable).

**Corpus survey** (`tests/conformance/cases/*.bzl`, 571 cases, plus the six guides, re-run at `8a03dd2` after this revision):

| Measure | Count | Detail |
|---|---|---|
| Single-component wrapper records over a scalar — §27.8's prescribed newtype shape | **5** | `Counter(Value: int)` (`spec_0167`), `BitMask(Bits: ulong)` (`spec_0168`), `TriBool(Value: int)` (`spec_0169`), `UserId(Value: int)` and `OrderId(Value: int)` (`spec_0195`) |
| Of those, carrying a natural domain predicate an invariant would state | **3** | `TriBool` (a three-valued domain over `int` — the sharpest case), `Counter` (non-negative), `BitMask` (none beyond `ulong`'s range; listed for completeness) |
| Of those, excluded by restriction (a′) | **0** | none is borrow-bearing |
| Records anywhere in the corpus or guides with a **validating constructor** (`requires` in a record body) | **0** | every `requires` in the corpus sits on a class or a free method (7 files contain `requires(`; `guide6_0008`'s `QuotRem` record carries its clause on a free method) |
| `invariant(` members in the corpus | **2** | both the §22.16 `Account` **class** example (`guide6_0000`, `spec_0160`) |
| `invariant(` occurrences in the guides | **6** | 5 in Guide 6's prose, 1 in Guide 2's classification table |
| User-authored **borrow-bearing** records (the population (a′) excludes) | **3** | `ClickedEvent`, `ValueChangedEvent`, `DataReceivedEvent` (`stdlib_0100`, `stdlib_0102`) — none a validation candidate |
| Corpus cases whose behaviour the §6 band ruling would change | **0** | follows from the zero above: no record constructor carries a clause |
| Corpus uses of `serialized optional` **on a component** | **1** | `stdlib_0021`'s `LastSeen: DateTime? serialized "last_seen" optional` — legal before and after §4(3), every nullable type being defaultable. (A grep also hits `spec_0315`, which transcribes Appendix A: the clause appears there only inside grammar comments, not as a use.) |

Two readings of the zero are available and the honest one is stated: it may mean the shape is not needed, or it may mean the shape is unwritable in the form authors want. 0106 §5.3's argument is the second; this survey cannot decide between them. What it does establish is the migration fact: **the landing costs the corpus nothing**, and the eventual guide/corpus pass would *add* invariants to three cases rather than repair any.

---

## 8. Alternatives declined

**(a) Do nothing.** Priced at 0106 §6.2(d) and re-verified at this proposal's §1.7. The wrapper stays writable and stays unsound in five spellings. **Declined.**

**(b) Keep `invariant` class-only; promote the wrapper to a class.** Today's only complete answer. Declined on cost and on kind: a class costs an allocation, a disposal obligation (§28), reference semantics where the author wants value semantics, and it loses `with`, auto-equality, defaultability of *containing* types, C-compatibility (§32.13's typed-handle admission is records-only), and inline storage. §27.8 prescribes the record precisely because these values should not be heap objects.

**(c) A separate `validated record` kind ▲.** Declined on three grounds: §3.6's closing rule (*"New rules added to this specification that need a structural classification of types must define it as a derived type property … rather than introducing a parallel mechanism"*); cost (a §3.1 row plus a case in every kind-dispatching rule in the document); and that the distinction is not about the *kind* — a `Port` is a record in every respect except that it has a predicate.

**(d) Contract-carrying type aliases ▲.** 0106 §6.2(a) declines this independently and this proposal concurs: aliases see through, are module-internal, and offer no conversion boundary to check at, so the check would land *"by a new rule invented for the feature — precisely the parallel mechanism §3.6's closing rule forbids"*.

**(e) Reject `with` on invariant-bearing records instead of instrumenting it.** Cheaper, and with a foothold in §9.5's current advice. Declined: it removes a useful operation from exactly the types that most want it, it makes the feature subtractive for records that adopt it, and §22.16's own deferral names the `with` point as part of the design rather than as an obstacle.

**(f) The two rejected answers to B1, the mediating-store hole (this proposal's §2.3), recorded with reasons.** Restriction (a′) was chosen over both:

- *Instrument public methods and block-form setters for borrow-bearing records only.* Declined because it **buys partial coverage of an unachievable guarantee**: the borrowed object's owner can mutate the same state without touching the record at all, so no instrumentation set inside the record can make an invariant over borrowed state true. It would also condition the instrumentation set on a §3.6 property — admissible in principle (the language dispatching on a language construct, §3.6's own admitted-exception reasoning) but paying real complexity for a promise that still would not hold, and reintroducing the mutating-getter residual for that class.
- *Record it as a residual and withdraw the "smaller instrumentation set" claim.* Declined because it leaves a soundness-motivated proposal with a silent soundness hole for a whole §3.6-defined class of records — the exact failure this proposal criticises in the status quo. A restriction the compiler enforces is worth more than a sentence a reader may miss.
- The *narrower* variant of (a′) — restrict the invariant **expression** to components that do not hop through a class- or interface-typed field, leaving borrow-bearing records free to constrain their own scalars — is attractive and is **recorded as the liftable refinement**. It is not taken now because it needs an answer to laundering through §22.15 rule 6: `invariant(Count() > 0)` where a `pure` record method reads the borrowed field would evade a syntactic rule, and closing that needs a per-body fact (0105's discipline permits it; this proposal does not want to mint one). The structural rule needs no clause analysis at all.

**(g) Amend the stdlib so the §6 wrap is user-visible.** Give `SerializationException` a `(message, cause)` chaining constructor and have the synthesized code call it. Declined here because the raising is already compiler-internal (this proposal's §6, reason 4), so the amendment would buy nothing this proposal needs while making a spec-only change into a two-document one — but it is the coherent alternative if the owner prefers the wrap to be spellable in user code, and Q1 names it.

---

## 9. Interactions

- **Proposal 0106 (the survey).** This is its slate item **i** and its §6.4 recommendation verbatim. 0106 was **accepted (owner, 2026-08-20)** — its §9 slate acted on, with the two rulings and the recording train landed at v0.263 and proposals 0107–0111 filed for slate items i/vi/v/(iv+viii+ix)/x. This proposal inherits 0106 §6's framing but no unverified claim: every quotation is re-verified here against `8a03dd2`.
- **v0.263 (landed while this was drafted).** The relevant change is `Range<T>.Contains` gaining `pure` — 0106 slate item iii — and it is **directly favourable**: §22.16's clause-typing rule admits a clause expression only under §22.15's pure-body rules, whose rule 6 permits a call only where the member is `pure`-declared, so `invariant(Value in 0..65535)` — the readable spelling of the commonest record invariant — becomes compilable exactly when that lands. This proposal does not depend on it (the arithmetic spelling compiles either way). The rest of v0.263 is the `CancellationTokenSource` fact ruling and its recordings, which touch §22.12 and stdlib §6.1/§6.8 — sections this proposal does not cite; the header's Currency row states the per-section byte-identity evidence.
- **Proposal 0043 (behavioral contracts; graduated into §22.16).** 0043 §9.6 is the recorded deferral this proposal lifts, in its own words (this proposal's §1.1). Nothing 0043 decided is reversed: contract inheritance, tiers, clause typing, the `requires`-not-for-input boundary, and the always-on/`requires` posture all stand, and this proposal explicitly withdraws the one place an earlier draft extended tier machinery (this proposal's §2.6).
- **Proposal 0047 (mechanized core semantics).** Its §5-list item 6 classes the contract static tier and deterministic replay as *"Consumers of trust, not participants"* in the ownership calculus. This proposal adds no ownership-relevant construct: records carry no owned state (§9.7), clause evaluation is pure and borrow-only (§22.16), and non-defaultability is a compile-time classification. Mechanization scope unchanged; no axiom disposition touched.
- **Proposal 0103 / 0105 (accepted 2026-08-19).** 0103's Q5 clarification of §3.6's user surface landed at v0.262 and is the text this proposal amends. 0105's body-locality invariant is respected — an invariant member is a clause, classified signature-side by 0105's reading of §22.16/§2.8.2, and the per-instantiation defaultability check is §3.6 rule 2's existing shape. It is also why the narrower (f) variant is declined rather than adopted: that variant would need a new per-body fact.
- **Proposal 0102 (small records) and §27.8's newtype guidance.** The eventual guide pass belongs with them — Guide 3 is where the invariant-bearing wrapper should be shown, and §27.8's *"use a single-field record"* sentence would gain a pointer. Out of scope here (this proposal changes no guide) and named so it is not lost.
- **Future: operator synthesis for single-field numeric records (0106 §6.2(b)).** 0106 §6.4 sequences it *after* this proposal, as *"ergonomics over a currently-unsound foundation"*. Landing this first is what makes that sequencing coherent; nothing here forecloses it.

---

## 10. Landing shape

A single spec revision, no stdlib and no rationale-normative content, in this order:

1. §22.16 — the *Record invariants* subsection (this proposal's §2.2), including restriction (a′) and the public-writable-field rule; the Scope sentence; the out-of-scope strike.
2. §3.6 *defaultable* row and §17.11 — the two clauses and the compile-error entry (this proposal's §3.1).
3. The mandatory companions — §9.2's membership bullet, §2.8.2's carriage phrase, §9.8's `serialized optional` guard, §9.8's decode-band rule, A.4's comment (this proposal's §4 and this proposal's §6).
4. The optional ones — §9.5's advice paragraph, §2.8.6's parenthetical.
5. Changelog entry with a Migration note whose content is this proposal's §7 (opt-in breakage only; two non-opt-in changes, both with zero corpus incidence).
6. A rationale entry recording *why* the property is derived rather than declared — this proposal's §3.3 is its draft, and the enum-precedent argument is the one to carry; the two weaker arguments named there should not be reproduced.

Implementation follows the repository's ordinary discipline (spec-derived tests first, each fix shipping a spec-cited regression test). Natural test seams: the parse-already-works claim; the four instrumentation points; the borrow-bearing rejection; the eight rejection sites of §7's list; the §9.2 membership non-regression (a record with an invariant is still auto-hashable and still serialization-eligible); and the §6 band.

---

## 11. Questions for the owner

Four genuine forks; each carries a recommendation, so a silent yes is a complete answer.

- **Q1 — The decode band (this proposal's §6).** Should a contract clause answering `false` inside the synthesized `Deserialize` surface as `SerializationException` with the violation in its `CauseChain`, raised by the same implementation-internal route as the other structural faults (recommended) — or stay `ContractViolationException` with the mismatch recorded? A third option, if the wrap should be spellable in user code, is §8(g)'s stdlib amendment. **Recommendation: wrap, compiler-internal.** Confidence medium-high.
- **Q2 — The positional-record spelling (this proposal's §2.5).** Confirm that §9.3's combined form is the spelling and that no `ContractClause` slot is added to `RecordDecl`. **Recommendation: no slot.**
- **Q3 — Mode (this proposal's §2.6).** Checked-contracts mode, uniform with class invariants (recommended), or always-on like `requires`? **Recommendation: uniform.**
- **Q4 — Restriction (a′) (this proposal's §2.2, this proposal's §8(f)).** Confirm the structural form — a borrow-bearing record may not declare an invariant member — over the narrower expression-level restriction recorded as liftable. **Recommendation: structural now, narrow form on demand.**

*Deliberately not asked:* what a `static` record invariant means at the `with` point. It is recorded in the candidate text as open because the current prover floor admits no clause that could reach it, so there is nothing to rule (this proposal's §2.6).

*Also not asked:* whether the feature is worth having at all — that is 0106 §11's Q1, answered by its acceptance and by the 2026-08-20 direction to write this.

---

## 12. Revision record

Drafted 2026-08-20; revised the same day after adversarial verification. The revision is recorded because two of the changes are corrections of substance, not polish:

- **The instrumentation-completeness argument was wrong.** The first draft claimed no record member body can falsify an invariant, citing §9.4's rejection of receiver-storage stores while overlooking the *mediating* store the same paragraph legalises. Restriction (a′) is the repair (this proposal's §2.2, §2.3, §8(f)).
- **A fifth fabrication route was missing.** `serialized optional` supplies `default(T)` with no defaultability guard (this proposal's §1.6, §4(3)).
- **The decode-band mechanism named a constructor `SerializationException` does not declare** (this proposal's §6, reason 4) — re-spelled as compiler-internal, which is also what keeps this proposal spec-only.
- Smaller corrections: the two-site `static invariant` discharge withdrawn as invented tier machinery (this proposal's §2.6); §3.3 rebuilt on the enum precedent alone; the enum/interface amendment corrected (syntax-unreachable, not semantically rejected — this proposal's §2.1); the FFI residual stated over §32.13's whole position list; `OwningBuffer<T>(length)` added; the Currency row's evidence restated as per-section byte-identity; 0106's status updated to accepted; five quotation truncations closed.
