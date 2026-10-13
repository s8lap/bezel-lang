# Proposal 0182 — Retire the `polymorphic` serialization registry

| | |
|---|---|
| **Status** | declined — declined (2026-10-13, at critique in the v0.338 diet train, under the owner's criterion for that train: "Make a judgement on what should be retired based on how useful the feature is and how easy it is to write code differently to work around the absence of the feature"). Neither half holds: type-discriminated payloads are common in service APIs, and there is no short local rewrite — a `choice` admits only record-admissible payloads (§9.8 *Choice serialization*), and the hand-written base pair of §8 below is about 30 lines per base, unchecked for completeness, and changes the flat `$type` wire form. The registry stays. Drafted by lane W3; the critique is recorded in the v0.338 changelog entry. The text below is kept as the drafted record. |
| **Tracking issue** | the 2026-10 subtraction review (candidate D-B); no compiler tracker (declined — the registry stays) |
| **Target module(s)** | **spec**: §9.8 (*Inherited components* clauses 3–4, *Synthesized `Serialize`* clause 3, the derived-instance paragraph, the *Polymorphic dispatch* subsection deleted with its anchor, *Choice serialization*), §13.3, Appendix A.3 (`PolymorphicMember`/`PolyCase` deleted), A.4 (`Member`), B.1/B.2 (`polymorphic` no longer contextual), C.2, C.8 (one row deleted, three respelled). **stdlib**: §2.14 (the registry example replaced by the hand-written idiom; the closing summary), §2.15 (`$type` leaves the reserved set). **rationale**: §5.22 (one dated paragraph). **code-generation reference**: §3.2, §3.3, §3.3's visibility line, §7.1 (one *Looks right, is rejected* pair), §9. **`DECLINED.md`**: one `retired` row, one `declined-at-proposal` row. **No guide change** (no guide names the registry). |
| **Written against** | spec/stdlib **v0.337** at `e1a20aa44` (main). Section numbers are v0.337's. |
| **Origin** | The 2026-10 subtraction review's survey lane (its report §2, *§9.8 Serialization synthesis — Simpler*: "D-B retire the `polymorphic` registry … reach 2 test files (2 lines), 1 stdlib mention") and the owner's 2026-10-13 decision. Drafted by lane W3 of the v0.338 diet train. The reach was re-measured at HEAD and is larger than the survey's figure (§4 below); the judgement's two grounds — a closed subtype set already has a hand-written and a `choice` spelling, and the workaround is local — hold at the re-measured reach. |

*Reference convention: unprefixed §-references are language-spec references at v0.337; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API. The former name of the language is not used anywhere in this document.*

---

## 0. Summary

§9.8 lets a base class declare `polymorphic ("circle": Circle, "square": Square);` — a closed map from a wire discriminator to a derived type — so that a component or receiver whose static type is the base writes the run-time type's form under a leading `$type` field and reads it back by dispatching through the map. The mechanism is sound and its closed-set principle is right (rationale §5.19's v0.122 paragraph: a discriminator that resolves to an arbitrary type is a remote-code-execution vector). What this proposal retires is the *member*, not the principle: the registry is one member kind carrying its own grammar production (`PolymorphicMember`/`PolyCase`), a contextual keyword, a visibility exception (§13.3's "four kinds"), a reserved wire name (`$type`) and five rules of §9.8 — registry eligibility, the `$type`-first ordering, the `$ref`-coexistence rule, dispatch-before-binding, type scoping — about 810 words of normative text plus 31 mentions across the spec, for a surface the standard library never declares and no guide teaches.

The replacement is ordinary code the checker already reads: the base hand-writes `Serialize` and `Deserialize` — a discriminator field, then the leaf's own synthesized form — and the leaves synthesize as any class does. The leaf set stays closed and explicit (it is the set the two members name), nothing is discovered reflectively, and a `Circle`-typed receiver or component is unaffected (it never went through the registry). The run-time type check §9.8 added at v0.317 — a synthesized `Serialize` raises `SerializationException` for a derived instance — is the registry's closed-set rule "with `C` as the set's one member" (§9.8's own words) and stays as the one rule.

Measured at HEAD: nineteen registry declarations in nine gtest files, two lit programs (both about the parser's recovery inside an extension body), one test helper, and the two documentation examples that introduced the form (spec §9.8, stdlib §2.14). No guide, no other stdlib type, no corpus program beyond those two. This is a §10 subtraction: a construct's keep is defended, not assumed, and the defence here is the tests of the construct itself.

---

## 1. The text at v0.337

### 1.1 What the registry does

§9.8 *Polymorphic dispatch*: "A base class whose serialized components may, at run time, hold instances of registered derived classes declares a `polymorphic` registry as a member of its body … The registry is a closed, explicitly written map from a **wire discriminator string** to a **permitted concrete derived type**." Its synthesized `Serialize` "writes the discriminator of the value's concrete run-time type as a distinguished leading field (the discriminator field name is `$type`, reserved within an object written for a polymorphic base; it precedes all component fields), followed by that concrete type's own synthesized form"; its `Deserialize` reads the discriminator "**first**, before any component of the object, and consumed by dispatch", selects "the concrete type's synthesized `Deserialize`, and delegates the remainder of the object to it".

### 1.2 What the rest of §9.8 already decides without it

Since v0.317 (rationale §5.22, *A derived instance behind a base type*): "The synthesized `Serialize` of a class `C` with no `polymorphic` registry of its own writes `C`'s form, and raises `SerializationException` when the receiver's run-time type is a class derived from `C` … This is the registry's closed-set rule with `C` as the set's one member." So the language already states, for every class, the behaviour the registry generalizes to a declared set: a base-typed write of a derived instance is caught at run time, never silently truncated. The registry's only addition is *which* set is admitted and *how* its members are spelled on the wire.

### 1.3 The rules the member carries

Beyond the subsection itself (807 words): the eligibility rule's class row and the abstract-class clause ("The remedy for either is a registry or a hand-written member"); two sentences in *Inherited components* (a base "such as the abstract `Shape` of the `polymorphic` registry example"; a derived class's synthesized members "are not hiding declarations under §12.2, as a registered derived type's are not"; the inherited member raising "by the base's registry, whose registered types each declare the interface themselves"); the closing sentence of *Synthesized `Serialize`*; *Choice serialization*'s "exactly the polymorphic-registry discipline above"; §13.3's four-kind visibility exception; grammar A.3's `PolymorphicMember` and `PolyCase` with a 20-line comment; A.4's `Member` alternative; B.1/B.2's contextual-keyword entries; Appendix C.2 and four C.8 rows; stdlib §2.14's example and paragraph and §2.15's `$type` reservation; the code-generation reference's keyword list, declaration-forms line, visibility line and §9 table.

---

## 2. Options

**A — status quo.** Keep the member.

**B — retire the member; hand-written base members are the spelling (this proposal).** Delete the registry, its grammar, keyword, visibility exception, wire name and rules. A hierarchy written behind its base type hand-writes the base's `Serialize`/`Deserialize`; the leaves synthesize. The run-time type check stays.

**C — retire the member; synthesize a discriminated form over a `closed` hierarchy.** Delete the registry but have the compiler synthesize, for a `closed` base declaring the interfaces, a `$type`-discriminated form over the hierarchy's leaves (which §11.7 fixes to the declaring module). No new member, but a second synthesis shape for a base whose own form is otherwise empty, with the `$type` ordering, `$ref`-coexistence and dispatch-before-binding rules kept to describe it.

**D — retire the member; a `choice` is the only closed sum.** As B, but state that a closed set of serialized shapes is modelled as a `choice` (which serializes with a `$case` discriminator, §9.8 *Choice serialization*) and offer no class-hierarchy spelling.

---

## 3. Evaluation

| | A | B | C | D |
|---|---|---|---|---|
| soundness | sound | sound: the hand-written pair is ordinary code under §9.8's existing rules (a base declaring no component hand-writes freely; leaves synthesize; the run-time type check guards every base-typed write that bypasses the pair) | sound, but re-states four of the five retired rules for the synthesized shape | sound; loses nothing a `choice` can express |
| simplicity | 810 w + grammar + keyword + visibility exception + wire name | −810 w, −1 production, −1 contextual keyword, −1 visibility exception, −1 wire name; +1 stdlib idiom (one example) | −1 member, +1 synthesis shape and its rules: about neutral | as B, minus the stdlib idiom |
| consistency | the one member kind that is a compiler-consumed map rather than a declaration | every serialization behaviour is now either synthesized from declared structure or hand-written — no third mode | a third synthesis trigger (the `closed` modifier) that §9.8's structure-driven model does not otherwise read | the `choice` already carries class payloads nowhere (value-shaped storage only), so owned class leaves have no spelling |
| migration | — | nineteen test declarations rewrite to the idiom or to a `choice`; wire shape of the base-typed form changes (user-chosen discriminator and nesting instead of `$type` + flat) | same test migration; wire shape kept | same migration; a hierarchy with owned fields cannot migrate |
| the principle (closed set, never reflective) | kept | kept — the set is the names the two members spell | kept | kept |

B is chosen. C keeps most of what B deletes for the one property B gives up — the flat `$type` wire form — and the wire form of a Bezel-only serialization is not a compatibility surface (§2.8.5's ABI non-goal applies to it a fortiori; no external producer exists). D over-narrows: a hierarchy with owned fields is exactly the case the class-graph tier exists for, and §9.8's record clause keeps those out of a `choice`.

---

## 4. Measurement (HEAD `e1a20aa44`; `grep -rnE 'polymorphic\s*\('`)

| where | count | sites |
|---|---|---|
| gtest files declaring a registry | **9** (19 declarations) | `tests/codegen/serialization_tier4_poly_test.cpp:158,518,538,558`; `serialization_tier2_classes_test.cpp:854,897,938`; `serialization_case_type_ws_2419_test.cpp:257`; `serialization_discriminator_double_throw_2432_test.cpp:106`; `class_primary_ctor_serialization_2325_test.cpp:616`; `tests/module/xmod_class_serialization_3179_test.cpp:229`; `tests/parser/serialized_class_clause_test.cpp:30,38,47`; `uniform_trailing_commas_2086_test.cpp:408`; `e2e_kitchen_sink_test.cpp:48` |
| lit programs | **2** | `tests/lit/extension_polymorphic_diagnostic_cascade_4222.bzl:43`, `extension_polymorphic_unclosed_paren_4257.bzl:74` (both pin the parser's recovery when the clause appears inside an `extension` body, #4222/#4257) |
| test helper | 1 | `tests/support/parse_support.hpp:245–248` (re-prints the clause) |
| documentation examples (doc corpus) | **2** | spec §9.8 (`tests/conformance/cases/spec_0027.bzl`), stdlib §2.14 (`stdlib_0024.bzl`) |
| rule corpus (`tests/conformance/rules`) | 0 | — |
| guides | 0 | — |
| stdlib declarations | 0 | the vocabulary interfaces know nothing of it; §2.15's JSON mapping reserves `$type` |
| compiler | parser (`Parser_Decls.cpp`: `parsePolymorphicMember`, `parseAndDiscardExtensionPolymorphicMember`, 41 mentions), sema (`Symbol.hpp` registry, `TypeResolution_Checks.cpp` 24 mentions, `SymbolCollection.cpp`, `TypeResolution_Names.cpp`), metadata (`ModuleMetadata.hpp` `MetaPolyCase`, `ModuleMetadata.cpp` 35 mentions, `ModuleMetadataBuilder.cpp`), codegen (`LlvmBackend_Serialization.cpp` `emitPolyClassSerialize`, 22 mentions), runtime (`bezel_runtime_serialization.cpp` `$type` dispatch, 15 mentions) | implemented end to end |

The survey (D2) counted "2 test files (2 lines)"; the re-measure counts program text, not file names containing the word, and finds nine gtest files. The judgement's grounds do not depend on the count being two: every one of the nineteen declarations is a test *of the registry*, none is a program that needs a registry to say something else, and each migrates by the same local rewrite (§8).

**Over-rejection.** Nothing is newly rejected except the member itself: a class body containing `polymorphic (…)` is a parse error at the member (the word is an identifier again, so `polymorphic` followed by `(` is an unknown-member error, the diagnostic naming the hand-written idiom). A base-typed write of a derived instance that previously dispatched through a registry now raises `SerializationException` at run time under the v0.317 rule — the same exception the registry raised for an *unregistered* type — until the base's members are hand-written. No other program changes verdict.

---

## 5. Soundness

Nothing in the ownership or type system reads the registry. The hand-written pair is admitted by §9.8's existing rules: a base that "declares no component of its own contributes nothing and needs no conformance" and may hand-write both members (synthesis suppressed on the base only, *Suppression and interaction*); the leaves list the interfaces themselves, so each is "a synthesized class … for a serialization interface its own inheritance clause lists" and `c.Serialize(s)` on a `Circle`-typed `c` runs `Circle`'s synthesized member (*Inherited components* clause 3: selection by the receiver's static type). The run-time type check still guards every base-typed write that bypasses the hand-written pair (a `Shape`-typed component in a *synthesized* class elsewhere runs `Shape`'s hand-written `Serialize`, which is the dispatch). Identity tags and `$ref`s are the leaf's own, written inside the leaf's object, so the registry's `$ref`-coexistence rule has nothing left to say. No matrix cell quotes the subsection (0 of 149 cells citing §9.8).

---

## 6. The normative edits

The drafted edits (lane W3, ids `W3/0182-*`, 27 edits) were anchored on the §9.8 text as it stands after lane W3's editorial edits (`W3/ed-9.8-*`), in that order. In outline:

- **§9.8.** *Inherited components* clauses 3 and 4 lose their registry clauses; *Synthesized `Serialize`* clause 3 (the run-time type check) is stated for every class, its last sentence now naming the hand-written idiom: ▲ "A hierarchy whose instances are written behind a base type hand-writes the base's `Serialize` and `Deserialize` — a discriminator, then the leaf's own synthesized form (stdlib §2.14 shows the idiom)." The derived-instance paragraph is respelled without the registry (▲ "The remedy for either is a hand-written member."). The *Polymorphic dispatch* subsection (anchor `sec-polymorphic-dispatch-synthesis`, the example, five paragraphs) is deleted; the two links to the anchor are deleted with the sentences that carried them. *Choice serialization* drops "exactly the polymorphic-registry discipline above" (the discriminator rule is stated in that sentence itself).
- **§13.3, Appendix C.2:** "four kinds" → three (static constructor, destructor, invariant member).
- **Appendix A.3/A.4:** `PolymorphicMember`, `PolyCase` and their comment deleted; `Member` loses the alternative.
- **Appendix B.1/B.2:** `polymorphic` leaves the contextual-keyword list and table.
- **Appendix C.8:** the registry row deleted; the run-time-check and abstract-class rows lose "with no `polymorphic` registry of its own"; the contextual-keyword row loses its clause.
- **stdlib §2.14:** the registry paragraph and example become the hand-written idiom (the `closed class Shape` program in §8 below); the closing summary says a base-typed edge is written by a hand-written base member. **§2.15:** `$type` leaves the reserved-name list (`$`-leading renames stay rejected as a class).
- **rationale §5.22:** one dated paragraph (*The `polymorphic` registry is retired*), recording the ground and the declined alternatives.
- **code-generation reference:** §3.2's contextual list, §3.3's declaration-forms line and visibility line, §9's table; one *Looks right, is rejected* pair at the end of §7.1 (the WRONG registry, the rule, the hand-written pair and the `choice`).
- **`DECLINED.md`:** a `retired` row; a `declined-at-proposal` row for options C and the `$ref`-coexistence keep.

---

## 7. Declined alternatives (rows for `DECLINED.md`)

- **Keeping the registry for the `$ref`-coexistence rule it spelled** — the hand-written pair writes the leaf's form inside the discriminated object, so a leaf's identity tags and `$ref`s are the leaf's own; no coexistence rule is needed. Reopening bar: a measured hierarchy for which the hand-written pair is a recurring burden.
- **A compiler-synthesized discriminated form over a `closed` hierarchy (option C)** — a second synthesis shape for a base whose own form is empty costs what the registry cost, and the `choice` kind is already the synthesized closed sum. Reopening bar: as above, with a design that keeps the set closed and explicit.
- **Option D** (no class-hierarchy spelling) is not recorded as a row: it is subsumed by the `choice` sentence of the *Looks right, is rejected* pair and over-narrows for owned leaves (§3).

---

## 8. Changelog, migration, trackers


**Migration.** *Newly rejected:* the class-body member `polymorphic (…);` — nineteen declarations in nine gtest files, two lit programs and one helper (§4), each migrating to the idiom below or, where the test's subject was the registry's own diagnostics (duplicate discriminator, unrelated type, ineligible registered type: `serialization_tier4_poly_test.cpp:518,538,558`, `serialization_tier2_classes_test.cpp:938`), deleted with its subject. *Newly admitted:* nothing. *Run-time:* a base-typed write of a derived instance that dispatched through a registry raises `SerializationException` (the v0.317 rule) until the base's members are hand-written. *Doc-only:* §13.3, Appendix B, C.2, C.8, the code-generation reference. *Doc corpus:* `spec_0027` and `stdlib_0024` regenerate from the edited blocks (the stdlib block becomes the idiom).

The idiom (stdlib §2.14 after this proposal; every member it names exists at stdlib §2.14 — `BeginObject`, `WriteFieldName`, `WriteString`, `EndObject`, `ReadFieldName`, `ReadString` — and `FormatException` is a standard exception type, spec §31.4, thrown with a message as the code-generation reference §7.7 does):

```bezel
public closed class Shape: ISerializable, IDeserializable
  public method Serialize(s: ISerializer)              // hand-written: suppresses synthesis on Shape only
    s.BeginObject(2);
    s.WriteFieldName("kind");
    case this of                                       // exhaustive: Shape is closed
      var c: Circle => do s.WriteString("circle"); s.WriteFieldName("shape"); c.Serialize(s); end,
      var q: Square => do s.WriteString("square"); s.WriteFieldName("shape"); q.Serialize(s); end
    end;
    s.EndObject();
  end Serialize;

  public static method Deserialize(d: IDeserializer): owned Shape
    _ := d.BeginObject();
    _ := d.ReadFieldName();                            // "kind"
    const kind := d.ReadString();
    _ := d.ReadFieldName();                            // "shape"
    if kind = "circle" then
      var c := Circle.Deserialize(d);                  // the leaf's synthesized form
      d.EndObject();
      return c;
    elif kind = "square" then
      var q := Square.Deserialize(d);
      d.EndObject();
      return q;
    end;
    throw new FormatException($"unknown shape kind: {kind}");
  end Deserialize;
end Shape;

public sealed class Circle(radius: double): Shape, ISerializable, IDeserializable
  public property Radius: double => radius;          // Circle's members are synthesized
end Circle;

public sealed class Square(side: double): Shape, ISerializable, IDeserializable
  public property Side: double => side;
end Square;
```

Traced: `this` is a borrow; `case this of var c: Circle` binds a `Circle` borrow (§19.3, type pattern with binding over a class scrutinee; exhaustive over a `closed` root, §19.6); `c.Serialize(s)` runs `Circle`'s synthesized member (static-type selection, §9.8 *Inherited components* clause 3). `Circle.Deserialize(d)` returns `owned Circle`; `return c;` is the marker-free owned return (§28.8); the derived instance is transferred as the base type. Every path returns or throws (§21.7).

**Trackers to file at the landing** (one `spec-tracking` issue): (i) parser: `parsePolymorphicMember` and the extension-body discard loop deleted (`polymorphic` becomes an ordinary identifier; the #4222/#4257 lit programs lose their subject — their recovery property is re-pinned on another member clause or deleted); (ii) sema: the `Symbol.hpp` registry, `serializedPolyCasesOf`, the registry checks in `TypeResolution_Checks.cpp`; (iii) metadata: `MetaPolyCase` and the `polyCases` carriage (a metadata format bump); (iv) codegen/runtime: `emitPolyClassSerialize` and the `$type` dispatch in `bezel_runtime_serialization.cpp` (the `$type` reservation in the JSON reader can go with it); (v) the nine gtest files migrated as §8 states; (vi) `scripts/extract_doc_examples.py` corpus regeneration for `spec_0027`/`stdlib_0024`.

---

## 9. Self-critique (adversarial pass)

(a) *The survey said two test files; this proposal says nine.* The difference is counting method (file names vs program text), and it is stated rather than hidden. The judgement's criterion is "negligible real-code reach" and "a short, local, obvious rewrite": nineteen declarations, every one a test of the registry, each rewriting to a twenty-line idiom, meets both halves; but the coordinator should see the larger number before landing, so it is in the Status row, §0, §4 and the rationale paragraph.

(b) *Does the idiom lose a property?* Two: the flat `$type` wire form (now a user-chosen discriminator and a nested leaf object), and the compile-time check that every registered type is serialization-eligible (now the ordinary check at each leaf's own conformance declaration, which is at least as strong). Neither is a verdict of a program.

(c) *Is `closed` required in the idiom?* No — it is what makes the `case` exhaustive without a `_` arm; an `abstract` base writes a `_ => throw …` arm instead. The idiom shows the tighter form.

(d) *Does the stdlib idiom invent an API?* Checked member by member against stdlib §2.14 (`ISerializer`: `BeginObject(fieldCount)`, `WriteFieldName`, `WriteString`, `EndObject`; `IDeserializer`: `BeginObject(): int`, `ReadFieldName(): string`, `ReadString()`, `EndObject()`) and spec §31.4 (`FormatException`, thrown with a message exactly as the code-generation reference §7.7 does). `_ := d.BeginObject();` is the stdlib's own example spelling.

(e) *Does retiring `$type` from the reserved set newly admit a rename?* No: §9.8 and stdlib §2.15 reject every `$`-leading rename as a class; the list is illustrative.

(f) *Former-name check:* none used.
