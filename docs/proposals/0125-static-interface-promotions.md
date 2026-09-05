# 0125 — The two §11.8 promotions: `FromCollectionLiteral` → `ICollectionLiteralTarget` (#1506) and `IDeserializable.Deserialize` → a static interface member (#1507)

| | |
|---|---|
| **Status** | graduated-v0.294 — superseded — **Part A (#1506) graduated v0.294** (owner-accepted 2026-09-05): `FromCollectionLiteral` promoted to the static-interface pair `ICollectionLiteralTarget<TSelf, T>` / `IOwningCollectionLiteralTarget<TSelf, T>` (stdlib §2.19), conformance declared explicitly by user classes and nominally by the ten built-in sequence collections with the factory listed; the §15.4 list derived from those declarations; D4 re-keyed; class-only, type-parameter and interface rulings stated; the declaration-site migration diagnostic; `DECLINED.md:30` amended (D-0009). **Part B (#1507) deferred**, not graduated — the design in §3 is the recorded shape and its three-item reopening bar is §3.11; the deferral is registered in `DECLINED.md`. *(prior: `under-review` — draft 2026-09-05, revised the same day after two adversarial critique passes.)* |
| **Tracking issue** | [#1506](https://github.com/s8lap/bezel-lang/issues/1506) (Part A), [#1507](https://github.com/s8lap/bezel-lang/issues/1507) (Part B); parent [#3247](https://github.com/s8lap/bezel-lang/issues/3247) (closed completed — its 2026-08-21 owner comment carries two binding constraints on Part A, §2.4/§2.7); refutations [#3338](https://github.com/s8lap/bezel-lang/issues/3338)/[#3339](https://github.com/s8lap/bezel-lang/issues/3339) (closed duplicates; their checklists are absorbed here, with corrections); compiler prerequisite [#3857](https://github.com/s8lap/bezel-lang/issues/3857) (Part A); the dictionary-form delta [#3831](https://github.com/s8lap/bezel-lang/issues/3831) (already filed, cited not re-filed). |
| **Target module(s)** | **spec** — [§11.8](../Bezel_Language_Spec.md#sec-static-interface-members) (the *What this unblocks* sentence), [§15.4](../Bezel_Language_Spec.md#sec-collection-literals) (the target list; the D4 borrowing-family clause and the nested-fresh-producer clause), [§15.4.1](../Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol) (whole section: opt-in, the two form blocks, conjuncts, lowering, recognition category, example), [§17.5](../Bezel_Language_Spec.md#sec-the-in-operator) (the taxonomy sentence); Part B additionally [§9.8](../Bezel_Language_Spec.md#sec-serialization-synthesis). **stdlib** — [§2.19](../Bezel_Standard_Library.md#sec-static-abstractions) (heading, opening, producer-markers sentence, two new interfaces), [§4.3](../Bezel_Standard_Library.md#sec-how-the-borrowing-and-owning-hierarchies-relate) (the roster sentence), **§4.4–§4.11** (ten class headers gain a conformance clause and a listed member; `OwningArray<T>`'s reference implementation gains the body), [§2.14](../Bezel_Standard_Library.md#sec-serialization-vocabulary) (`:747`, `:769`, both parts; the whole marker prose under Part B), [§2.15](../Bezel_Standard_Library.md#sec-concrete-serialization-formats) (`:985`, Part B). **rationale** — §5.16 (`:687`, `:689`, `:697`), §5.19 (`:752`), §18 (`:2268`, `:2275`). **changelog**, `DECLINED.md` (routed rows, §5), `Bezel_For_Code_Generation.md` (`:765` §5-index line **and its pin**, Part A; `:763` under Part B). |
| **Written against** | v0.291, `main` @ `dc0b60d` (2026-09-05). Every `file:line` was re-located with `sed -n`/`grep -n` against that tree after the critique (C5's three mis-cites corrected); every count was re-run with the recipe in §0. Compiler citations are from reading `src/` and `tests/`; nothing here required a build. **v0.292 (item 1c) and v0.293 (proposal 0124) have since landed** — HEAD is `7561e19`, docs at v0.293 — and the two lines they share with this edit-set (A1, R2) are re-anchored on the landed text (§4, §7); every other `old` is unchanged between `dc0b60d` and HEAD (42/44 unique at `dc0b60d`, A1/R2 being anchored on the v0.292 text; 44/44 at HEAD `7561e19` — §7). `0124-enum-name-parse.md` is `graduated-v0.293`, so 0125 is confirmed free (N5). |
| **Currency** | **Quoted:** spec §2.8.2 (`:263`), §9.8, §11.2, §11.8, §12.7 (`:2488–2496`), §14.5 (`:2629`), §15.4, §15.4.1, §17.5 (`:3402`), Appendix A.3/A.4 (`:10618`, `:10625–10628`, `:10768–10779`, `:11068`, `:11073`); stdlib §2.14 (`:717`, `:735–747`, `:769`), §2.15 (`:985`), §2.16 (`:1032`), §2.19 (`:1115–1143`), §4.3 (`:1532`), the ten class headers of §4.4–§4.11 (`:1557`, `:1644`, `:1723`, `:1838`, `:1879`, `:2235`, `:2251`, `:2302`, `:2313`, `:2325`, `:2336`); rationale §5.16 (`:687`, `:689`, `:697`), §5.19 (`:752`), §5.22 (`:854`), §18 (`:2268`, `:2275`); changelog v0.272; `DECLINED.md:75`, `:204`, `:256`; proposal 0115 §3.3, §6.2; `Bezel_For_Code_Generation.md:15`, `:765`, `:2041`; `tests/sema/zero_one_reject_1424_test.cpp:1–13`, `tests/sema/collection_literal_factory_conjuncts_3032_test.cpp:1–41`. **Cited without quotation:** spec §3.6, §9.2, §9.7, §14.2, §22.6, §26, §28.1, §28.3, §28.6, §28.11, §28.13; stdlib §4.14, §4.15. **The `owned Shared<C>` return shape is in the spec** at §9.8 `:1546` (`grep -c "owned Shared<C>"` = 1 at both `dc0b60d` and `7561e19`; pass 1's C1 claimed otherwise and was wrong — §7). **Re-verify before graduation:** the three §15.4 sentences A4/A5 edit (v0.271/v0.272 both touched them). **Line drift `dc0b60d` → `7561e19` (P2-C11):** the spec's §15.4/§15.4.1/§17.5 region is **+33** (A2 `:2948`→`:2981`, A6 `:3040`→`:3073`, A8 `:3056`→`:3089`, A12 `:3402`→`:3435`), rationale §18 **+2** (R3 `:2275`→`:2277`), the code-generation reference **+2** (G1 `:765`→`:767`), `DECLINED.md` **+1** (D1 `:75`→`:76`, D2 `:30`→`:30`); the stdlib is unmoved. Cites below are `dc0b60d` unless marked HEAD. |
| **Origin** | The 2026-09 owner-delegated spec batch (scoping brief `SCOPING-round2.md` §0/§1a/§1b), which found that (i) `Self` is a recorded *rejection* (`DECLINED.md:204`) while stdlib `:747`/`:769` still call it deferred, (ii) the compiler never received v0.272's carrier rename (#3857), (iii) #3338's "does not reproduce on HEAD" was measured before v0.272, and (iv) the A/B conformance-declaration question and the `owned Shared<C>` shape's place in an interface were unsettled by every prior record. This proposal settles (i), (iv) and the A/B question, and sequences behind (ii). The adversarial critiques added the nominal-declaration requirement for the built-ins (B1/B2, coordinator-directed), the true in-tree break (B3), and the interface/type-parameter/derived-class edge rulings (B4, B5, C11). |

*Reference convention: unprefixed `§` is the **spec**; **stdlib**, **rationale**, **changelog**, **process** references are marked. `:NNNN` alone is a line in the document named by the nearest preceding file name. Every quotation is verbatim from the v0.291 tree; emphasis inside a quotation is this proposal's unless stated. ▲ marks proposed normative text, landed nowhere. Item 1c (`Range<T>` / `ISteppable<T>`, #1508) is ruled separately at v0.292 and is not touched here except at the one sentence all three promotions share (§4).*

**Grep convention (every count below).** `docs/` = `grep -n -E '<pattern>' docs/Bezel_*.md` (the eleven documents; `docs/proposals/` excluded unless stated); `tests/` = `grep -rn -E --include=*.cpp --include=*.bzl '<pattern>' tests/`; `corpus` = the same over `tests/conformance/`; `src/` = `grep -rn -F '<literal>' src/`. Part A's declaration sites: `grep -rn "static method FromCollectionLiteral" tests/ --include=*.cpp | grep -v '//'` (B3's recipe). Part B's conformance-declaration sites: `-o -E '(:|,)\s*IDeserializable\b'` (occurrences, not lines — a signature listing carries several per line); constraint sites: `-o -E 'where \w+: IDeserializable\b'`. Counts are at `dc0b60d`, 2026-09-05.

---

## 0. What this proposes, in one page

| Part | Item | Verdict | Class (process §2) | One-sentence reason |
|---|---|---|---|---|
| **A** | #1506 — `FromCollectionLiteral` → two static interfaces, `ICollectionLiteralTarget<TSelf, T>` / `IOwningCollectionLiteralTarget<TSelf, T>` (stdlib §2.19); conformance **declared explicitly** (ruling B) by user classes **and by the ten built-in sequence collections, nominally, in their stdlib headers** with the factory listed as a member of each; dictionary form outside; the §15.4 list derived from those declarations | **Graduate** — after #3857 (v0.292/v0.293 have landed) | design change → ruling-class graduation | The mechanism exists and is implemented; every §15.4.1 conjunct re-keys to §12.7 clause-1 conformance (two need new sentences: class-only, and the declaration-site migration diagnostic); D4 survives as three predicate substitutions; the in-tree break is **17 declaration sites in 4 test files plus 1 corpus case, zero external** — one of those files (`_3032`, 10 sites) must be re-authored because it pins diagnostic sentences the revision deletes; and the promotion buys constraint dispatch, type-parameter targets, and ten listed factories. |
| **B** | #1507 — `IDeserializable` (marker) → `IDeserializable<TOut>` with `static method Deserialize(d: IDeserializer): owned TOut;`, **no recurrence constraint** (so the third shape is `class Node: IDeserializable<Shared<Node>>`), with an admissibility clause on `TOut`; synthesis supplies the witness (the `IEquatable<T>` precedent) | **Defer** — design recorded, three prerequisites named | design change → proposal; not ready to graduate | The promotion buys something real — user-writable generic deserialization, which stdlib `:985` says is impossible today — and all three recognized return shapes fit one member (the third, `owned Shared<C>`, is stated in the spec at §9.8 `:1546` and is `TOut = Shared<C>`); it is deferred on **cost and two unverified obligations**: a **621-occurrence** breaking header change (§3.5) with no ramp unless arity-overloaded type names are ruled admissible, the §9.8 synthesis-generator obligation (the synthesizer must emit an interface-member witness), and the unverified admissibility of `class Node: IDeserializable<Shared<Node>>` as a conformance header. |

Both parts correct §11.8:2079's *"both signatures were chosen for zero-source-change promotion"*: it is true of the **member signature** and false of the **conformance declaration** in both cases (Part A: one header clause per conformer; Part B: `: IDeserializable` → `: IDeserializable<Point>` everywhere). Part A's graduation rewrites the sentence (§4); Part B's deferral leaves its half stated honestly.

Both parts fix the `DECLINED.md:204` / stdlib `:747`+`:769` inconsistency (`Self` *rejected* vs *deferred*): Part A's graduation carries both one-line editorials (Appendix A, edits S17 and S18) regardless of Part B's fate.

---

## 1. Shared ground — the facts both parts rest on

**1.1 The mechanism is admitted, and implemented.** §11.8 (`:2049–2051`): *"An interface member may be declared `static`: a contract the conforming **type** — not an instance — must satisfy. Static interface members are **abstract-only** … and may be **methods** or **operators**."* The grammar admits it directly: Appendix A.4 `:10768–10779` — `InterfaceMember ::= IfaceMethod | IfaceProperty | IfaceIndexer | IfaceStaticMethod | IfaceStaticOperator`, `IfaceStaticMethod ::= "static" "pure"? "method" Identifier GenericParams? "(" ParamList? ")" (":" "owned"? Type)? ContractClause* ";"`. An interface declaration carries constraints: A.3 `:10625–10628`, `InterfaceDecl ::= … GenericParams? Inheritance? GenericConstraints* InterfaceMember* …`, and `GenericConstraints ::= "where" Identifier ":" Constraint ("," Constraint)* ","?` (`:11068`, starred) with `Constraint ::= "class" | "value" | … | Type` (`:11073`) — so a two-parameter interface with a recurrence on one parameter and `class` on the other is spellable as two `where` clauses. The compiler implements user-declared static interfaces and recurrent conformance: `tests/lit/sema/static_iface_conformance_807__recurrence_conformance_accepted.bzl`, `…__owned_return_mismatch_rejected.bzl`, `tests/codegen/static_interface_dispatch_807_test.cpp`, `tests/module/xmod_static_interface_2294_test.cpp`; 124 `§11.8` citations in `src/`. **Neither part is blocked on a missing mechanism.**

**1.2 `Self` is rejected, not deferred, and the recurrence is the only spelling.** §11.8 `:2061`: *"no new syntax, no `Self` keyword (deliberately declined for v1: the recurrence is explicit and already expressible)"*; `DECLINED.md:204`, verdict `rejected`, bar `—(none stated)`, correctly placed under `### rejected`. Stdlib says *"separately deferred"* at **two** sites — `:769` (*"Were a `Self`-return type ever added (the separately-deferred feature …)"*) and `:747` (*"Bezel has no `Self` type (that feature is separately deferred)"*); Appendix A edits S17 and S18 fix both. Both interfaces below use a type parameter for the conforming type, named **`TSelf`**, not `Self`: `Self` is an ordinary identifier today, but a stdlib parameter spelled `Self` would read as the keyword the register rejects, and the stdlib's parameter names are `T`-prefixed throughout (`TElement` at §4.16).

**1.3 The recurrence is not a self type — and that decides how Part A's predicate must be keyed.** `interface I<TSelf> where TSelf: I<TSelf>` admits `class Weird: INumber<Meters>` (Meters conforms, so the constraint holds) — the conformer need not be the argument. §11.8 lives with this because dispatch is always *through a type parameter*: `T.Zero()` at `T = Meters` is Meters' member. A literal site is different: the compiler starts from a **target type** `C` and must decide whether `C` is a target. The predicate is therefore *"`C` is a class that conforms to `ICollectionLiteralTarget<C, U>` — TSelf substituted with `C` itself"* (§2.2). A `Weird: ICollectionLiteralTarget<RingBuffer<int>, int>` is a legal conformance (a class that happens to build ring buffers) and is **not** a literal target; nothing needs to reject it — and every clause that names "conformers" must say "targets" instead, so `Weird` is never read into D4 (B5(b), §2.4).

**1.4 Conformance is nominal — §12.7's definition.** §12.7 *Interface conformance* (`:2491–2496`): *"For an interface I and a candidate type T, T **conforms to** I exactly when one of the following holds"* — clause 1 (`:2492`) *"**T is a class** that lists I (or any interface extending I) in its inheritance clause, that inherits from a class which conforms to I, or that conforms **implicitly** — every class conforms to `IDisposable` without listing it"* (the one named implicit case); clause 2 records, clause 3 interfaces (*"has I in its extension chain"*), clause 4 enums — each *"lists I … in its inheritance clause"*; clause 5 (`:2496`) primitives, which *additionally* carry §11.8's compiler-provided conformances — **scoped to primitives**. The grammar matches: `Inheritance ::= ":" InheritItem ("," InheritItem)* ","?` (`:10618`); `ExtensionDecl ::= Visibility? "extension" Type GenericConstraints* ExtensionMember* …` carries no `Inheritance`, so an extension block cannot declare conformance (which is what preserves §15.4.1's *"not … supplied by an extension block"* conjunct for free, §2.2). §11.8's `:2075` — *"a runtime type conforms only by declaring the full member set, statics included"* — is the same fact stated from the `is`/`as` side. §11.2's `:1900` (*"A class implements an interface implicitly: by declaring matching public members"*) is about the **absence of C#-style explicit-implementation syntax** and is, on its own, too weak to carry ruling B; §12.7 is the authority. **Consequence for the built-ins (B1):** the ten sequence collections can conform only by clause 1 — by *listing* the interface in their stdlib class declarations — since clause 5's compiler-provided device is not available to classes and this proposal adds no implicit case to §12.7 (§2.3).

**1.5 The erasure rule unifies the value/class return shapes.** §11.8 `:2061`: *"at a value or runtime-managed conforming type a declared `owned` return … is erased by the substitution (§28.1), so the witness is spelled without the marker … a class or interface conformer spells the marker exactly."* So one member `…: owned TSelf` is witnessed by `: Point` at a record and `: owned C` at a class — Part B's first two shapes, and Part A's class-only shape, with no bridge.

**1.6 A declared conformance with a synthesized witness already exists.** §12.7 clause 2 (`:2493`): *"auto-generated members count: the synthesized `Equals(T)` satisfies `IEquatable<T>` (stdlib §2.9's recorded convention — 'Records implement this implicitly')"*, and stdlib `:747` describes `: IDeserializable` as working *"in the same way `: IEquatable<T>` opts a record into synthesized equality"*. Part B's "the synthesizer supplies the static witness" is therefore not unprecedented (the brief's obstruction 3 is answered) — the precedent is an *instance* member, and §11.8's primitive bridge (`:2077`) is the *static* one; Part B composes the two.

**1.7 What `:2079` promised, exactly.** *"both signatures were chosen for zero-source-change promotion"*. Under 1.4 a promotion to an interface member necessarily adds a conformance clause the by-name protocols never required. The **signature** promise holds for both parts (§2.1, §3.1 keep the member text byte-for-byte); the **declaration** promise cannot. Proposal 0115 §6.2 (`:380`) read the sentence the same way — *"The promoted `ICollectionLiteralTarget<T>` member then declares `ReadOnlyArraySpan<T>`, and §11.8's zero-source-change claim survives"* — and, like #3338 and #3339, took the claim to cover the header too. It does not, and the graduation says so (§4).

**1.8 Consequence (2) binds both parts.** `:2075`: *"An interface that declares static members may not mark its type parameters variant"*. Neither interface below is variant; a future `out` on the element parameter is rejected at the declaration.

**1.9 Direct static calls exist only where the stdlib declares them (#1424).** `tests/sema/zero_one_reject_1424_test.cpp:1–3`: *"`Zero` and `One` as direct primitive statics are declared ONLY on decimal (stdlib §9.3); the prior accept on every numeric primitive keyword was an invented API (CLAUDE.md rule 2)"*; `:11–13`: *"The §11.8 constraint path (`T.Zero()` inside a `where T: INumber<T>` body, #807) is SEPARATE."* So a compiler-provided conformance makes a member reachable **through a constraint only**; a member callable on the type's own name must be **listed** in `Bezel_Standard_Library.md` (`Bezel_For_Code_Generation.md:15`/`:2041`: *"if it is not there, it does not exist"*). This is the second reason (with 1.4) the built-ins declare nominally and list the factory (§2.3): the direct call `Array<int>.FromCollectionLiteral(span)` is then a declared API, not an invented one, and #1424's rule is honored rather than contradicted.

---

## 2. Part A — #1506: `FromCollectionLiteral` → `ICollectionLiteralTarget`

### 2.1 The interface shape ▲

Two interfaces, because §15.4.1 has two forms with different carriers and *exactly one per type* (`:3056`; `DECLINED.md:75`), and a static interface member is abstract-only — a single interface declaring both members would force every conformer to supply both. The pair mirrors the stdlib's borrowing/owning hierarchy split (stdlib §4.1/§4.2), which is the split the two forms already encode.

```bezel
// stdlib §2.19 ▲ — the collection-literal target pair (spec §15.4.1)
public interface ICollectionLiteralTarget<TSelf, T>
    where TSelf: ICollectionLiteralTarget<TSelf, T>
  static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned TSelf;   // borrowing form
end ICollectionLiteralTarget;

public interface IOwningCollectionLiteralTarget<TSelf, T>
    where TSelf: IOwningCollectionLiteralTarget<TSelf, T>
    where T: class
  static method FromCollectionLiteral(owned elements: OwningArray<T>): owned TSelf;   // owning form
end IOwningCollectionLiteralTarget;
```

Every name in the members exists: `ReadOnlyArraySpan<T>` (stdlib §4.15, the v0.272 carrier — `:3045`), `OwningArray<T>` (stdlib §4.5, `where T: class` at `:1644`), and the member text is §15.4.1's `:3045`/`:3053` with `C<T>` replaced by `TSelf`. `where T: class` on the owning interface restates §15.4.1's *"For a type `C<T>` that owns its elements (`where T: class`)"* (`:3050`) and matches every owning built-in's own constraint (`:1644`, `:1879`, `:2251`, `:2313`, `:2336`). The conformer writes, per the §11.8 pattern:

```bezel
public class RingBuffer<T>: ICollectionLiteralTarget<RingBuffer<T>, T>
  …
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned RingBuffer<T>
    …
  end FromCollectionLiteral;
end RingBuffer;
```

— the member is unchanged from the spec's `:3083` example; the header gains one clause. A generic class conforming to a self-instantiated interface has precedent in-tree (`class Box<T>: IComparable<Box<T>>`, `tests/codegen/array_sort_generic_class_elem_3530_test.cpp:78`).

### 2.2 The admission predicate, re-keyed — and every §15.4.1 conjunct accounted for

▲ **A class `C` is a sequence-literal target when `C` conforms (§12.7) to `ICollectionLiteralTarget<C, U>` — a *borrowing-form target* — or to `IOwningCollectionLiteralTarget<C, U>` — an *owning-form target* — with `TSelf` instantiated with `C` itself, at `C`'s own instantiation; `U` is the element type against which every element expression is checked. A constrained type parameter `P` whose constraints guarantee that conformance at `TSelf = P` (§12.7 clause 6) is likewise a target inside the body that declares it (C11 — newly defined; the lowering is §11.8's constraint dispatch). An interface type is never a target (B4).** Inline arrays and the dictionary form are unchanged (§2.3). "Sequence-literal target" is thereby a *declaration-derived* property, not a §3.6 structure-derived one; the revision says so and adds no §3.6 row (N1).

| §15.4.1 conjunct today (`:3040`, `:3056`) | Under conformance | Loss? |
|---|---|---|
| "a static method named exactly `FromCollectionLiteral` with one of two recognized signatures" | §11.2 exact-signature matching against the interface member (the `owned`-in-signatures clause included) — the general rule replaces the special one | none; the special-case checker `checkCollectionLiteralFactoryConjuncts` (`src/sema/TypeResolution_Checks.cpp:1576–1751`) retires into the ordinary conformance check, **plus** the migration diagnostic below |
| "must be `public`" | interface members are implicitly public and a witness must be `public` (§11.2 `:1902`) | none |
| "must be `static`" | the member is declared `static` on the interface; an instance witness does not match (§11.8) | none |
| "Exactly one of the two forms … Declaring both is a compile error" | ▲ a class that declares, on itself, conformance to both interfaces at `TSelf` = itself is a compile error at the declaration (same reason: element-expression ownership would not select a target) | none — `DECLINED.md:75`'s row re-cites the new authority (and drops its stale `ReadOnlySpan<T>` carrier, C9) |
| "must return `owned C<T>` where `C<T>` is the enclosing type at its own arity" | `TSelf = C` in the use-site predicate (1.3). A conformance with `TSelf ≠ C` is legal and is not a target. | none |
| "declared directly on the collection type (not inherited from a base class …)" | a derived class inherits `Base: ICollectionLiteralTarget<Base, T>` (§12.7 clause 1) with `TSelf = Base ≠ Derived`, so `Derived` is not a target unless it declares its own conformance at `TSelf = Derived` — which the uniqueness rule (next-but-one row) **admits**, because uniqueness is scoped to conformances the class declares on itself (B5(a)) | none — falls out of 1.3 |
| "(… or supplied by an extension block)" | extension blocks cannot declare conformance (1.4) | none |
| "The element type is read directly from the parameter's element type" | `U` is read from the unique own-declared conformance's second argument | none — but "unique" must now be **stated** (next row) |
| *(implicit today: one factory per type)* | ▲ a class declares, on itself, at most one instantiation of either interface at `TSelf` = itself; a second — the other interface, or the same one at another element type — is a compile error at the declaration. Inherited conformances (at `TSelf` = the base) do not count. | a new sentence, not a new rule |
| *(implicit today: classes only — `owned` on a record/choice/enum return is a compile error, §28.1)* | **§11.8's erasure (1.5) would silently admit a value-kind conformer** with an unmarked return. ▲ **Conformance to either interface by a value kind is rejected at the declaration** — the literal's result is an owned heap value under §15.4's literal-tree rule and §15.4.1's *Result ownership*, and admitting value-kind targets is a widening this proposal does not take. Recorded liftable (§5). An interface may extend either interface (§12.7 clause 3 — ordinary interface extension), but an interface type is never a target: `IBuf.FromCollectionLiteral(…)` is the *"static member on the interface name"* compile error of §11.8 `:2075`, and the compiler already declines interface targets (`isCollectionLiteralInterfaceTarget`, `src/sema/ExprTypecheck_Calls.cpp:316`, consulted at `:502`/`:572`). | none — but these are the **two conjuncts the interface alone cannot carry**, and both must be written |
| *(new — the migration signal, B7)* | ▲ **Declaration-site diagnostic:** a class that declares a `public static` method named `FromCollectionLiteral` whose signature matches either interface member at `TSelf` = the class, but that conforms to neither interface, is a compile error at the declaration naming the missing conformance and the remedy (*add `: ICollectionLiteralTarget<C, T>` / `: IOwningCollectionLiteralTarget<C, T>` to the class header*). Without it a migrated-but-unclaused type is a silent non-opt-in reported at the wrong file — exactly the #3857 failure the promotion is sequenced behind. | a new sentence; the only usable migration signal |

**Body-locality (process §9, proposal 0105 §1): local** — a declaration-site check on the conformer plus a use-site predicate over the target type's declared conformances (or, for a type parameter, the constraints in scope in the same body); no cross-body facts. **Kernel:** no §28/§30.5 contact beyond D4's re-key, which v0.272 already classified as a *derivation* and which §2.4 preserves verbatim in substance.

### 2.3 The built-in roster — declared nominally, in the stdlib, per type

**Roster (sequence form only).** Each built-in conforms at itself. Borrowing: `Array<T>`, `LinkedList<T>`, `Set<T>`, `Stack<T>`, `Queue<T>` — `Array<T>` to `ICollectionLiteralTarget<Array<T>, T>`, and so on. Owning: `OwningArray<T>`, `OwningLinkedList<T>`, `OwningSet<T>`, `OwningStack<T>`, `OwningQueue<T>` — `OwningArray<T>` to `IOwningCollectionLiteralTarget<OwningArray<T>, T>`, and so on. **Outside, unchanged:** inline arrays `[N]T` (not nominal types; §15.1), and `Dictionary<K, V>`/`OwningDictionary<K, V>` under the dictionary form — the owner's #3247 caveat, *"the dictionary form (`[k: v]`) stays outside the interface, and the §15.4 built-in list becomes conformance-derived rather than deleted"*. `SortedSet`, `FrozenArray`, `OwningBuffer` and both span families are not §15.4 targets today and stay out (N4). `SortedDictionary<K, V>` is a *dictionary-form* compiler admission the spec never made, already tracked as **#3831** (impl-ahead removal, ruled on #3517) — cited, not re-filed (C2).

**Mechanism: nominal declaration in each type's stdlib section (B1/B2, coordinator-directed).** The §11.8 `:2077` compiler-provided device is **not** available: §12.7 scopes it to primitives (clause 5) and this proposal adds no implicit case to §12.7 (1.4); and a member a user may call on the type's own name must appear in a stdlib signature (1.9). So each of the ten class headers gains the interface in its inheritance clause, and the factory is listed as a member of the type in its own section — **10 conformance declarations on 10 types, spread over 11 header lines** (`OwningArray<T>` is declared twice, `:1644` the signature listing and `:1723` the *Reference implementation*, which is real Bezel and therefore also gains the **body**) **and 11 member additions** (10 listed signatures + 1 body). Every one is verifiable by `grep -n` on the header line: `:1557`, `:1644`, `:1723`, `:1838`, `:1879`, `:2235`, `:2251`, `:2302`, `:2313`, `:2325`, `:2336` (Appendix A, S6–S16; the first-line anchors are unique except `OwningArray<T>`'s, which is disambiguated by its second line). The built-ins then conform by **§12.7 clause 1 as written** — the stdlib declaration *is* the class declaration that lists the interface — and §15.4's list *derives* from those declarations and says so (A2, S5). The factory's body is the literal's own lowering (§15.4's per-target element-order rules); `OwningArray<T>`'s reference implementation shows the owning form is one line (`return elements;` — the transferred temporary is the result). **Observable consequences, stated in the Migration note:** (i) `TSelf.FromCollectionLiteral(…)` through a constraint at a built-in substitution; (ii) the direct static call `Array<int>.FromCollectionLiteral(span)` — a **declared** member, so #1424's rule is honored; (iii) `arr is ICollectionLiteralTarget<Array<int>, int>` is `true` for the ten types (§11.8 `:2075` keeps static-member interfaces usable as `is`/`as` operands) — a new runtime-observable fact.

**A recorded decline this reopens, named (P2-B1): `DECLINED.md:30`.** That row — *"A `ToArray()` on `ReadOnlyArraySpan<T>`, riding the v0.272 collection-literal carrier fix"*, verdict `rejected`, bar *"A stdlib pass that takes it on its own merits"* — declines exactly the capability `Array<T>.FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Array<T>` supplies, on three grounds: *"in-tree uses are zero and external users are zero, and a stdlib addition riding a signature fix is the *while-we're-at-it* expansion the project's operating rules forbid. Surface is debt."* Process §2's third rule applies: reopening takes **new evidence against the stated reasons**. The evidence is structural, not a re-ask: the owner accepted the promotion (#3247, the *"built-in collection types declare it"* item), and a nominal conformance to a static-interface member **requires a declared witness** — §11.8 `:2061`: *"A conforming type declares `class Meters: INumber<Meters>` and supplies the static members with exact signatures (the §11.2 matching rule, applied to statics)"*; `:2075`: *"a runtime type conforms only by declaring the full member set, statics included, so there is no partial-conformance state to probe."* There is **no "witness-only" or "conformance without a callable member" form** in §11.8 or §12.7 — a static witness is a `public static` member of the type, reachable by name like any other. So the ten factories are not a convenience addition riding a fix (the *while-we're-at-it* ground) but the unavoidable member cost of the promotion the owner directed, and the "surface is debt" ground is paid knowingly (§2.8 cost 2). The alternative — the built-ins conforming without a callable member — was priced (§2.10 item 10) and declined because the conformance rule has no such form. The `:30` row is **amended** (D2), not deleted: its `ToArray()` spelling stays declined (a second spelling for the same value would be the dual the row forbids), its record notes that v0.294 supplies the value at the witness spelling as a structural consequence, and the register carries the reasoning as D-0009 (§5).

### 2.4 D4 survives: three predicate substitutions, no loss

The owner's first #3247 constraint: *"0115's D4 extended §15.4's borrowing-family fresh-producer rejection to name 'a user-defined type whose `FromCollectionLiteral` declares the borrowing form' … Promoting the signature … must not lose that rule."* §15.4 keys the rule in three places; each keys on a *form*, and each form is now an *interface* — and each is keyed on **targets**, not conformers, so 1.3's `Weird` never enters (B5(b)):

| §15.4 clause (verbatim fragment) | Re-keyed ▲ |
|---|---|
| `:2962` *"For **borrowing-family** targets (`Array<T>`, `Set<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>`, the key and value positions of `Dictionary<K, V>`, and a user-defined type whose `FromCollectionLiteral` declares the borrowing form ([§15.4.1](…)))"* | *"For **borrowing-family** targets (every borrowing-form target under [§15.4.1](…) — `Array<T>`, `Set<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>`, and any user class that is one — and the key and value positions of `Dictionary<K, V>`)"* |
| `:2962` *"a nested collection literal targeting a non-trivial-dispose collection type (built-in, or a user-declared target of the [§15.4.1](…) protocol, whose `FromCollectionLiteral` result is `owned`)"* | *"a nested collection literal targeting a non-trivial-dispose collection type (built-in, or any sequence-literal target under [§15.4.1](…) — the factory result is `owned TSelf`)"* |
| `:2960` *"For owning targets, the literal's element expressions must produce owned values … This applies uniformly to `OwningArray<T>`, … and `OwningDictionary<K, V>` (value position …)"* | the general first sentence already covers a user owning-form target; the enumerated list is left as is (it serves the value-position parenthetical for the dictionary). **No edit**; A6 defines the terms *borrowing-form target* / *owning-form target* so "owning targets" has a name to resolve to (N3). |

The predicate is unchanged in extension: a borrowing-form target was "a user type declaring the borrowing form"; it is now "a class that is a borrowing-form target" — the same set, by 2.2's table. The **kernel classification stays what v0.272 recorded** (derivation via K1/K2.3/K4.3 through §28.11's borrow column), because the rule's content did not move, only its name for the target class. Compiler: the two admission sites (`ExprTypecheck_Calls.cpp:503–530`, `:573–592`) and codegen's carrier discrimination (`LlvmBackend_Setup.cpp:31450`) switch from a name walk to a conformance query — after #3857 has re-keyed them onto the right carrier (§2.7).

### 2.5 The §17.5 / rationale §5.16 taxonomy

§17.5 `:3402` today: *"This is the only structural-dispatch rule in Bezel's expression surface (the iteration protocol of §29.1 is interface-based by contrast; the user-defined collection-literal protocol of §15.4.1 is name-based — it requires a method of one *exact* name, not any method of a matching shape, so it is recognition-by-declared-name rather than structural duck-typing)."* ▲ *"This is the only structural-dispatch rule in Bezel's expression surface (the iteration protocol of §29.1 and the collection-literal protocol of §15.4.1 are interface-based by contrast)."* — the three-way taxonomy becomes two-way. Rationale §5.16's *"A new recognition category, named honestly"* paragraph (`:697`) is rewritten to state the current two categories and to move the name-based era to the changelog (process §7's no-narration rule); its *"promoting … without breaking any user code"* clause is corrected per 1.7; and the two framing sentences above it that still assert the retired mechanism — `:687` *"via a named static factory"* and `:689`'s heading *"Mechanism: named static factory, not an attribute"* — are re-framed so the section does not contradict itself two paragraphs apart (N6; a *why* document may date its reasoning, but not carry a live heading for a mechanism the same section says is gone). §15.4.1's *Recognition category* paragraph (`:3067`) becomes *"This is interface-based recognition …"*. Full text in Appendix A.

### 2.6 The A/B conformance-declaration ruling: **B — explicit**

**(A) Implicit — the interface as an internal predicate, conformance inferred from the factory.** Priced: Bezel has no structural conformance (1.4 — every §12.7 clause but the `IDisposable` carve-out lists the interface in the inheritance clause); inventing it for one interface makes `ICollectionLiteralTarget` a type a class satisfies *without naming it* — a second structural rule, which rationale §5.16 (*One opt-in path, not two*: *"The specification is careful to keep exactly one structural-dispatch rule"*) and §17.5 (*"the only structural-dispatch rule"*) both guard against by name; `DECLINED.md:86` already declines duck-typed enrollment on that ground. And (A) buys prose only: the compiler still looks up a name, the "special case is deleted" claim is false, and `TSelf.FromCollectionLiteral` through a constraint does not follow because nothing declares the constraint's satisfaction. **Declined** (§5 row 1).

**(B) Explicit — the conformer writes the header clause.** Priced: the promise at `:2079` is corrected, not honored (1.7). The break is one header clause per conformer. **In-tree cost, measured (B3):** **17 declaration sites in 4 test files** — `tests/sema/collection_literal_factory_conjuncts_3032_test.cpp` (10), `tests/parser/param_post_colon_owned_3027_test.cpp` (3), `tests/codegen/collection_literal_user_type_fcl_2957_test.cpp` (2), `tests/sema/collection_literal_dict_form_target_3031_test.cpp` (2) — **plus 1 corpus case** (`spec_0075.bzl:12`, the `RingBuffer` example; `spec_0073`/`spec_0074` are the two form signature listings, which A7 re-spells with the conformance rather than replacing, B6) — and the spec example (`:3072`), rationale §5.16's quoted signature (`:689`), and #3338's repro block. **Of those 17, the 10 in `_3032` are not a header-clause edit but a re-authoring**: that file pins, verbatim, the three §15.4.1 diagnostic sentences this revision deletes (`:17–21`: *"non-`public` visibility → 'must be `public`'; return type not `owned C<T>` → 'must return `owned C<T>`'; both recognized carriers declared → 'exactly one may be declared'"*), and its predicates are *"selective on the §15.4.1 wording"* so that *"the test fails if a generic 'cannot recognize factory' diagnostic replaces it"* (`:36–41`). After A8 those conjuncts are §11.2 conformance diagnostics plus the B7 migration diagnostic, and the test must be rewritten against them. **External users: none.** (B) is what the mechanism *is*; the zero-source-change wording was a wish about the header that the mechanism never supported. **Ruled B.**

### 2.7 Prerequisite: #3857, and the order of landing

`src/sema/TypeResolution_Checks.cpp:1625` — `const bool isBorrowForm = (carrierName == "ReadOnlySpan");` and `src/codegen/LlvmBackend_Setup.cpp:31450` — `if (paramName == "ReadOnlySpan") {`: on HEAD a factory written to the current spec (`ReadOnlyArraySpan<T>`) is not recognized at all. #3338's *"does NOT reproduce as a bug on HEAD"* was measured at `77ed0fc`, before v0.272, and its repro spells `ReadOnlySpan<T>`; **this proposal does not repeat that claim.** #3857 is filed and open. **Order:** v0.292 and v0.293 have landed (§4); #3857 lands before the graduation (it is two string compares, two diagnostics, and un-skipping/re-transcribing the three corpus rows); the spec graduation next; the conformance re-key after. Landing the spec before #3857 is *permitted* (process §5 item 3 — the spec leads), but it would stack a second delta on the same three sites while the first is still open, and the owner's #3247 reasoning (*"promote first and the wrong carrier is frozen into the interface"*) applies in spirit: do not promote what the compiler cannot yet recognize.

### 2.8 What the promotion buys, against what it costs (rewritten with the corrected numbers)

**Gains.**
1. **Constraint dispatch:** a generic body `Build<C, T>(…) where C: ICollectionLiteralTarget<C, T>` can construct any target — through `C.FromCollectionLiteral(span)`, or, since a constrained parameter is itself a target (§2.2, C11), through the literal `var c: C := [a, b];` directly. Today no constraint names the factory, so neither is expressible.
2. **Ten declared factories:** `Array<T>.FromCollectionLiteral(span)` and its nine siblings become listed stdlib members (§2.3) — the value `DECLINED.md:30` declined at the `ToArray()` spelling, now supplied at the witness spelling because §11.8's conformance rule has no witness-less form. This is a gain **and** a cost (cost 2), and it is disclosed as a reopening, not sold as free.
3. **One recognition category fewer** (§2.5); the §15.4.1 conjunct checker retires into §11.2 and the §15.4 target list is derived from declarations (owner's caveat honored: derived, not deleted).
4. **Two records repaired on the way:** the `Self` deferred/rejected inconsistency at stdlib `:747`/`:769` (S17, S18), and the stale `ReadOnlySpan<T>` leftover at spec `:3056` and `DECLINED.md:75` (A8, D1), each named in the changelog as an editorial repair rather than dropped silently (C9).

**Costs.**
1. **Source:** 17 declaration sites in 4 test files + 1 corpus case gain a header clause, zero external (B3) — and **`_3032` is a re-authoring, not a clause**: its 10 sites pin diagnostic sentences the revision deletes (§2.6).
2. **Stdlib surface:** 2 new interfaces; 10 conformance declarations on 10 types over 11 header lines; 11 listed/implemented members (§2.3) — 22 stdlib edit sites in §4 alone, every one anchored in Appendix A. **Nine of the ten span/array→collection materializations were never taken "on their own merits"** — which is what `DECLINED.md:30`'s bar demands — and the tenth is that row's declined value at another spelling; they arrive as required witnesses (§2.3), and the row is amended to say so (D2, D-0009). That is surface accepted as the price of the promotion, recorded as such.
3. **Newly observable:** `is`/`as` against the pair is `true` for ten stdlib types; direct calls on ten factories; type-parameter targets (Migration note, §2.11).
4. **Compiler: ~4–6 days** (§2.12) — up from the first draft's 3–5, because the migration diagnostic, the `_3032` rewrite, and the built-in symbol surface (conformance + member on symbols that today have *"null `decl` and empty members"*, `ExprTypecheck_Calls.cpp:19830`) are added, while the callable body for the built-ins stays (it is now a declared member, so it must exist).
5. **Docs:** 3–4 h for the revision plus the code-generation reference's §5-index line and **its pin move** — required by process §9's stdlib-surface clause, not by gate arithmetic (the pin is v0.293 at HEAD, so v0.294 would be 1 behind and inside `CODEGEN_MAX_STALE = 3`; P2-C8).

**Net.** The gains are capabilities, not prose — items 1 and 2 are new expressible programs, item 3 is the special-case retirement the promotion was designed for. The costs are bounded, measured, in-tree, and — for the ten factories — a disclosed reopening of `DECLINED.md:30` with structural evidence. Pass 2 confirmed this case against the tree; the graduation lands on it.

### 2.9 Corpus accounting (process §6, deletion-shaped clause)

| Row | Section | Today | After graduation |
|---|---|---|---|
| `spec_0073` | §15.4.1 borrowing signature | `skip:signature-listing` | **unchanged** — A7 edits only the prose heading above the block; the code block and its comment do not move, so no re-transcription is owed |
| `spec_0074` | §15.4.1 owning signature | `skip:signature-listing` | **unchanged**, same reason |
| `spec_0075` | §15.4.1 `RingBuffer` example | `skip:elided` | header gains the conformance clause (A11); stays `skip:elided` (the example elides `Push`) — **candidate to un-elide** once #3857 makes it compilable, which would make it the promotion's first live conformance row |
| *(new)* stdlib §2.19 block | the two interfaces | — | new `stdlib_NNNN` row, `skip:signature-listing` (the dry-run measured exactly +1 extracted fragment, N1) |
| the §4.4–§4.11 listing rows for the ten types | their existing `stdlib_NNNN` rows | `skip:signature-listing`-class | text changes (header clause + listed member); re-transcribed by hand (#1380); `OwningArray<T>`'s reference-implementation row gains a real body |

No row is deleted. Non-corpus: 17 sites in 4 files (§2.6) change after the compiler accepts the clause — the tests drive the real `bezelc`, so the test edits ride the compiler PR, not the docs revision; `_3032` is re-authored there.

### 2.10 Alternatives declined, with reasons

1. **Implicit/structural conformance (ruling A)** — §2.6. Contradicts the one-structural-rule posture; buys prose only.
2. **One interface carrying both forms** (two static members, or one member with an overload pair) — abstract-only statics make every conformer supply both, which contradicts *exactly one form per type*; and it is precisely the shape proposal 0115 §3.3 (`:229`) set as its reopening bar for the carrier-discrimination question — *"#3247's interface promotion producing a shape in which one declared `ICollectionLiteralTarget<T>` member must carry both forms"*. Two interfaces keep that question closed.
3. **A single interface with the form as a third type parameter** (`ICollectionLiteralTarget<TSelf, T, TCarrier>`) — a carrier-typed parameter would admit `ReadOnlySpan<T>` (the owning view v0.272 removed) and `Array<T>` (0115's declined option D) as carriers by construction; the two-interface pair fixes the carriers.
4. **Compiler-provided conformance for the ten built-ins** (the first draft's choice) — unavailable to classes under §12.7 clause 5, and it would make ten callable members exist nowhere in the stdlib (1.9, #1424). Declined; nominal declaration per type instead (§2.3).
5. **Admitting value-kind conformers** (records/choices as literal targets, via erasure) — a widening the by-name protocol never had; needs the literal-tree rule (§15.4) and §9.7's no-temporary-source rule traced at a borrow-bearing record target. Deferred/liftable (§5).
6. **Rejecting the type-parameter target** (keeping "a type parameter is never a target") — the lowering is exactly §11.8's constraint dispatch over §15.4.1's existing *Lowering* paragraph, and it is the capability item 1 of §2.8 names; declining it would keep a special case the promotion exists to remove. Admitted instead, as newly-defined.
7. **A ramp (warn-first) for the missing header clause** — not owed: process §2's Ratchet class is for a tightening *"whose design a prior revision already ruled"*, and nothing scheduled this one; what is owed is the declaration-site diagnostic (§2.2, B7/N7).
8. **A `Self` keyword** — rejected at `DECLINED.md:204`; not reopened (no new evidence).
9. **Dictionary-form interface** — stays outside (owner's caveat; `DECLINED.md:256` row unchanged).
10. **Built-in conformance without a callable member** (declare the pair on the ten types but list no `FromCollectionLiteral`, keeping `DECLINED.md:30` untouched) — priced and declined: §11.8 `:2061` requires a conformer to *"supply the static members with exact signatures (the §11.2 matching rule, applied to statics)"* and `:2075` says a type *"conforms only by declaring the full member set, statics included"*; a witness is a `public static` member, and there is no witness-only form in §11.8 or §12.7. Inventing one would be a new conformance kind for ten types, which is the implicit-case route §2.10 item 4 already declines. Recorded as D-0009 (§5).

### 2.11 Change class, gates, and the Migration note

**Design change → this proposal (§6) → ruling-class graduation.** It adds two stdlib types, changes an admission predicate from name to conformance, adds ten stdlib conformances and eleven members, and (ruling B) changes what a conforming declaration must say. Gate: mechanical + adversarial critique (a second pass over §2.6/§2.8) + composition gate over §11.8:2079 and rationale `:2268` with v0.292/v0.293 (§4). **Migration note (end state; no ramp):**
- **newly-required:** a header clause on every user conformer (17 in-tree sites, 4 files, + 1 corpus; zero external); the `_3032` test re-authored against §11.2 conformance diagnostics and the new declaration-site diagnostic (diagnostic-text coupling, B3);
- **newly-defined:** `TSelf.FromCollectionLiteral(…)` through a constraint; a constrained type parameter as a literal target; the ten listed built-in factories callable directly; `is`/`as` against the pair `true` for the ten built-ins; the declaration-site diagnostic for a matching-shaped `FromCollectionLiteral` with no conformance;
- **newly-rejected:** value-kind conformers; own-declared dual conformance / a second instantiation; (unchanged in effect: an interface type as a target — already declined by the compiler);
- **newly-accepted (the retired conjunct diagnostics, P2-C2):** a `FromCollectionLiteral` that is non-`public`, non-`static`, inherited, extension-supplied, or of a non-matching return type, on a class that declares **no** conformance, is no longer a declaration-site error — it is an ordinary member of a type that is simply not a target; the three §15.4.1 conjunct diagnostics that used to reject it are retired into §11.2 matching (which reaches it only once a conformance is declared) plus the declaration-site diagnostic (which keys on a *matching* signature);
- **doc-only:** §17.5, rationale §5.16/§18, the `Self` editorials at stdlib `:747`/`:769`, the `ReadOnlySpan<T>` leftovers at spec `:3056` and `DECLINED.md:75` (named as editorial repairs).

### 2.12 Size

- **Graduation revision (docs): 3–4 h**, plus 30 min for the code-generation reference. Spec §11.8/§15.4/§15.4.1/§17.5; stdlib §2.19 (heading, opening, producer-markers sentence, the pair) + §4.3 + eleven §4 header/member edits + `:747`/`:769`; rationale §5.16 (three edits) + §18 + §5.19; changelog; `DECLINED.md` (rows routed per §5); **`Bezel_For_Code_Generation.md`: required** — the §5-index line (`:765` at `dc0b60d`, `:767` at HEAD) gains the pair and the ten conformers, and **the pin moves** because process §9 requires it on any stdlib-surface change (not because of gate arithmetic: the pin is v0.293 at HEAD, so v0.294 is 1 behind, inside `check_docs.sh:115`'s `CODEGEN_MAX_STALE = 3` — P2-C8). Guides: none mention the protocol (grep: 0) — no revalidation.
- **Compiler (separate, larger — not this lane's): ~4–6 days.** The tracking issue filed at graduation must carry: (i) #3857 as prerequisite (½ day); (ii) the built-in symbols' conformance and member surface — the ten `TypeSymbol`s have *"null `decl` and empty members"* (`ExprTypecheck_Calls.cpp:19830`), so the conformance query and the listed member must be synthesized on them (N8); (iii) `isCollectionLiteralAdmissibleTarget` / the empty-literal predicate → conformance query at `TSelf` = target, class-only, plus the type-parameter case (`ExprTypecheck_Calls.cpp:503–530`, `:573–592`); (iv) `checkCollectionLiteralFactoryConjuncts` (`TypeResolution_Checks.cpp:1576–1751`) → retire into §11.2 matching, keep the value-kind and own-declared-dual rejections, **add the declaration-site migration diagnostic** (B7); (v) a callable static body for the intrinsic lowering so `TSelf.FromCollectionLiteral(span)` at a built-in substitution and `Array<int>.FromCollectionLiteral(span)` work (`LlvmBackend_Setup.cpp:31214–31500`, carrier discrimination at `:31450`) — **while the literal itself, at a built-in target, keeps its direct intrinsic construction under §15.4's element-order rules** (A9's carve, P2-B3): the materialize-then-call lowering governs user conformers and type-parameter targets only, so nothing is circular for `Array<T>`/`OwningArray<T>` and the eight others do not acquire a span-materialization step; (vi) diagnostics; (vii) **test migration: `tests/sema/collection_literal_factory_conjuncts_3032_test.cpp` needs re-authoring** — its 10 declaration sites pin the three §15.4.1 diagnostic sentences A8 deletes (`:17–21`, `:36–41`) and must be rewritten against §11.2 conformance diagnostics plus the new declaration-site diagnostic — and its cells have successors **only once each cell declares the conformance**: cell 1 (a non-`public` factory with no conformance) has no successor at all, because that program is now newly-accepted (§2.11); it is already on `runAot`, so no `runJit` ratchet obligation rides the rewrite (P2-N5); the other three files — `tests/parser/param_post_colon_owned_3027_test.cpp` (3 sites), `tests/codegen/collection_literal_user_type_fcl_2957_test.cpp` (2), `tests/sema/collection_literal_dict_form_target_3031_test.cpp` (2) — take a header clause each; plus new spec-cited tests for the `Weird` non-target (1.3), the derived class with its own conformance (B5), the interface non-target (B4), the type-parameter target (C11), and the migration diagnostic (B7).

### 2.13 Verdict: graduate

After #3857 (v0.292 and v0.293 have landed; A1/R2 are anchored on their text). Everything the promotion needs exists and is implemented; every conjunct re-keys (two need new sentences); D4 survives as three substitutions; the break is 17 sites in 4 files plus one corpus case, zero external; the gain is real and measured against a cost that is now fully enumerated.

---

## 3. Part B — #1507: `IDeserializable.Deserialize` → a static interface member

### 3.1 The shape ▲ — a type parameter for the result, **without** the recurrence, **with** an admissibility clause

```bezel
// stdlib §2.14 ▲ — replaces the marker `public interface IDeserializable end IDeserializable;` (:717)
public interface IDeserializable<TOut>
  static method Deserialize(d: IDeserializer): owned TOut;
end IDeserializable;
```

Conformers: `record Point(X: int, Y: int): IDeserializable<Point>` (witness `Deserialize(d): Point` — `owned` erased, 1.5); `class Order(…): IDeserializable<Order>` (witness `: owned Order`); and the third shape, `class Node(…): IDeserializable<Shared<Node>>` (witness `: owned Shared<Node>` — `Shared<T>` is a class, stdlib `:1032`, so the marker applies exactly). All three member texts of stdlib `:735–741` are unchanged — the **signature** promise holds.

▲ **Admissibility of `TOut` (C10).** Dropping the recurrence leaves `TOut` unconstrained by the interface, so `class Order: IDeserializable<int>` would be a legal conformance whose synthesized witness (§3.4) produced an `int` from `Order`'s serialized form. The rule that closes it: *a class `C` may declare `: IDeserializable<TOut>` only with `TOut = C` or `TOut = Shared<C>`; a value kind `V` only with `TOut = V`; any other instantiation is a compile error at the conformance declaration.* This is the admissibility the by-name protocol carried implicitly (stdlib `:747`: *"the return type is the declaring type itself, written concretely"*), made explicit because the interface no longer carries it. Its `Shared<C>` arm is the header whose admissibility under §11.2/§11.8/§12.7 as written has never been traced or probed — §3.11's item 3.

**Why no `where TOut: IDeserializable<TOut>`.** The recurrence earns its place on `INumber<T>` because `T.Zero() + x` must type as `T`; here the member's result is consumed by the caller, and the constraint would do exactly one thing — forbid *"type A reconstructs type B"* — which the third shape shows is **wanted** (`Node` reconstructs `Shared<Node>`). With the recurrence, `Node: IDeserializable<Shared<Node>>` needs `Shared<Node>: IDeserializable<Shared<Node>>`, i.e. a compiler-provided conformance of a *wrapper* type witnessed by *another type's* static member — a bridge kind with no precedent (`:2077` bridges intrinsics, not foreign members). #3339 wrote the recurrent form and did not see the third shape; the brief saw the shape and called it *"the item's hardest sub-question"*. Dropping the constraint dissolves it: `IDeserializable<TOut>` is the first §2.19-family interface without a recurrence, and §11.8 permits that (`:2061` calls the recurrence *"the ordinary generic-constraint machinery"*, not a requirement). Generic code still dispatches through the constraint: `Load<T>(path): T where T: IDeserializable<T>` calls `T.Deserialize(d)`; and the shared-node entry, which stdlib `:985` says *"has no facade form"*, becomes `LoadShared<C>(path): owned Shared<C> where C: IDeserializable<Shared<C>>` — ordinary Bezel. At most one instantiation per type follows from §22.6 (overloads never differ by return type alone), so no "unique" sentence is needed here — unlike Part A; *which* instantiations are admissible is the clause above.

### 3.2 The three shapes, one member

| stdlib `:735–741` shape | Witness under 3.1 | Mechanism |
|---|---|---|
| `Deserialize(d): T` (record/enum/choice) | `: Point` | `owned` erased at value kinds (1.5) |
| `Deserialize(d): owned T` (class) | `: owned Order` | exact |
| `Deserialize(d): owned Shared<C>` (shared node) | `: owned Shared<Node>` with `TOut = Shared<Node>` | exact — **no `Self` substitution needed**, because `TOut` is not required to be the conformer |

The brief's finding — *"the `owned Shared<C>` shape is not [a `Self` substitution]"* — is correct and is exactly why 3.1 drops the recurrence rather than adding a second parameter or a second interface.

### 3.3 The third shape is stated in the spec and unimplemented — a tracking item, not a deferral ground

- **Stated, at §9.8 `:1546`** (identical at `dc0b60d` and HEAD `7561e19`; `grep -c "owned Shared<C>"` = 1 in both): *"The class return shape is [§2.14]'s: `owned C` for a class that *holds* shared/weak components, and `owned Shared<C>` when the class *is* a shared node (the co-owned entry); the choice is locally decidable from the entry type."* Stdlib `:741`/`:747` and rationale `:854` (*"Reconstructing a shared node returns `owned Shared<T>`"*) carry the same rule. The spec states the shape, states the trigger (*"when the class is a shared node"* — a class that its own serialized graph reaches by identity, the only reading under which `Downgrade()`/`Clone()` can bind to the root), and asserts the trigger is locally decidable from the entry type. Under 3.1 it is `TOut = Shared<C>`, an exact witness, needing no bridge. **Pass 1's C1 asserted the spec never states this and was wrong; the first draft's citation was right; every absence assertion has been removed (P2-B2).**
- **Unimplemented.** Sema types every `Deserialize` call as returning the declaring type (`src/sema/ExprTypecheck_Calls.cpp:~19822`: `retTy->declaration = targetType`); codegen's `emitClassDeserialize` (`LlvmBackend_Serialization.cpp:3075…`) handles `Shared<C>` **components** (identity participants, #703) and never mints a `Shared` handle as the **entry**; `grep -rn "owned Shared" src/` finds no serialization site. Stdlib `:985` already records the consequence: `ParseOwned<C>` over such a class *"is a compile-time error"* and the class *"is read through the agents"*.
- **Unused.** `tests/` and the corpus contain the shape only in the §2.14 signature listing (`stdlib_0018.bzl:64`); zero conformers, zero calls.
- **Untracked.** A GitHub search (*"Deserialize returns owned Shared<C> shared-node co-owned entry shape"*) returns nothing on point; #703 covers components. This is a **spec-ahead-of-impl delta** (process §5 item 3) and gets its issue at Part B's graduation — or now, since it is true regardless of #1507. It is **not** a ground for deferring the promotion: the rule is normative and the interface carries it unchanged.

### 3.4 §9.8: the synthesizer supplies the witness

▲ Restated rule: *A record, enum, choice or class that declares `: IDeserializable<X>` (with `X` admissible per 3.1) and is serialization-eligible has the static witness `Deserialize(d: IDeserializer): owned X` (marker erased at value kinds) **synthesized**, exactly as a record's synthesized `Equals(T)` satisfies `IEquatable<T>` (§12.7 `:2493`); a hand-written member matching the interface signature is the witness and suppresses synthesis of that direction; a hand-written member of any other shape is an ordinary §11.2 mismatch error.* This replaces the *"Hand-writing either member suppresses synthesis"* prose at stdlib `:771` and the name-keyed gate in the compiler (`typeHasHandWrittenMethod(t, "Deserialize")`, `TypeResolution_Checks.cpp:8787, 8904, 9068, 9405`) with "is a witness present". The synthesized eligibility error (`:1531`) keeps its site — the conformance declaration. One consequence must be stated: a synthesized witness at `X = Shared<C>` (a class declaring `: IDeserializable<Shared<Node>>` with no hand-written member) is the shared-node synthesis §9.8 `:1546` already specifies. **This section is §3.11's bar item 2:** the generator today emits a by-name static intercepted in `tryLowerSerializationCall`; it must learn to emit a witness that §11.2 matching recognizes and that a constrained generic can reach — a §9.8 design obligation the brief (obstruction 1) and #3339 (item 3) both name, and which no one has designed. Everything else in §9.8 is unchanged in substance; the section's opening sentence changes its spelling.

### 3.5 Migration count (the source break)

`: IDeserializable` → `: IDeserializable<T>` at every conformer, and `where T: IDeserializable` → `where T: IDeserializable<T>` at every constraint:

| Where | Occurrences | Of which constraints | Files |
|---|---|---|---|
| spec | 7 | 0 | 1 |
| stdlib | 28 | 8 | 1 |
| rationale | 2 | 0 | 1 |
| code-generation reference (`:763`, plus its prose describing the marker) | 1 | 0 | 1 |
| changelog | 5 | — | **pinned history, not edited** |
| `tests/` (`.cpp` harness strings + lit `.bzl`), corpus excluded | 537 | 18 | 76 |
| corpus `tests/conformance/` | 46 | 0 | 10 (`stdlib_0027`/`0028` carry 18 each — the §2.15 facade listings; `stdlib_0019–0025`, `spec_0027` the rest) |
| **Total to edit** | **621** (38 docs + 537 tests + 46 corpus) | 26 | 90 |

(The first draft said 667, double-counting the corpus into the tests figure — C3.) External users: none. All 583 test/corpus occurrences are mechanical (`sed`-shaped), but they can only flip when the compiler accepts the generic spelling — and there is **no ramp** unless a bare `IDeserializable` and a generic `IDeserializable<T>` may coexist: §2.8.2 `:263` makes generic arity part of program-wide type identity, but I found no rule admitting two declarations of one simple name at different arities in one scope, and the stdlib declares no such pair today. A ramp (marker deprecated → warn → removed) therefore needs its own ruling first; without it the flip is one hard revision across 90 files.

### 3.6 What the promotion buys (the honest question, Part B)

**Not uniformity only.** Stdlib `:985` states the gap in the language's own words: *"`IDeserializable` is a marker interface with no members, so no generic body can reach the recognized `T.Deserialize(d)` factory through a constraint, and `Parse<T>`/`ParseOwned<T>` (both formats) are **compiler-known generics**"*, and promises that after promotion *"all four become expressible as ordinary constrained generics with no signature change."* So Part B buys: (1) **user-writable generic deserialization** — impossible today; (2) the read facades become ordinary Bezel (their *constraint* spelling changes, which `:985`'s "no signature change" under-states); (3) a facade shape for the shared-node entry falls out of 3.1 for free (no new stdlib member proposed — that would be a "while we're at it" addition; recorded as newly-expressible only); (4) one name-recognized protocol fewer, and the code-generation reference stops teaching a by-name rule (`:763`).

**Against:** a 621-occurrence break with no ramp; a §9.8 rewrite whose generator must learn to emit an interface-member witness (bar item 2); an admissibility clause (3.1) whose `IDeserializable<Shared<Node>>` header has never been traced or probed (bar item 3); **1–2 weeks** of compiler work (`declaresBuiltinInterface(t, "IDeserializable")` at `ExprTypecheck_Calls.cpp:19809, 20230, 20313, 20339` becomes a generic-instantiation query; the synthesized factory must be materialized as a real static function reachable through a constraint instead of intercepted by name in `tryLowerSerializationCall`; `LlvmBackend_Serialization.cpp:4306–4318, 4793–4799`, `LlvmBackend_Setup.cpp:26219–26260`, `OwnershipFacts.cpp:599`; the facades optionally de-special-cased).

### 3.7 Alternatives declined, with reasons

1. **The recurrent form** `IDeserializable<TSelf> where TSelf: IDeserializable<TSelf>` (#3339's shape) — cannot express the third shape without a foreign-member bridge (3.1). Declined.
2. **A second interface for the shared-node entry** (`ISharedDeserializable<C>`) — a second type for one undefined case; and a type conforming to both would need two same-named statics differing only in return type (§22.6 forbids). Declined.
3. **Retiring the third shape** (every class returns `owned C`; the caller wraps) — a root that its own graph references by identity (`Weak<C>` back-edge to the root) must be constructed *inside* a shared cell for `Downgrade()` to bind (rationale `:854`, stdlib `:747`); wrapping after the fact cannot produce that. Declined: the shape is normative (§9.8 `:1546`) and its reason is structural. Liftable only on evidence that a self-referencing shared-node graph is unreachable in ownership-valid Bezel (e.g. that every such graph is a §28.13 `Shared` cycle) — then retirement is the right answer.
4. **Keeping the marker and adding `IDeserializable<T>` beside it permanently** — two mechanisms for one fact; the no-dual posture (stdlib §1 `:63`) and rationale §5.19's *"avoids inventing a second recognition mechanism"*. Declined as an end state; **admissible as a ramp** if arity-overloaded names are ruled in (3.5).
5. **Leaving `TOut` unconstrained** (no admissibility clause) — admits `Order: IDeserializable<int>` with a synthesized nonsense witness (C10). Declined; the clause in 3.1 instead.
6. **A `Self` keyword** — rejected (`DECLINED.md:204`); not reopened.

### 3.8 Change class

**Design change → proposal (§6).** It changes a shipped conformance spelling, a stdlib interface's shape, and §9.8's synthesis statement, and it contradicts a recorded claim (`:2079`). Body-locality: **local** (declaration-site witness and admissibility checks). Kernel: none.

### 3.9 Size

Proposal drafting is done here. **Graduation, if taken now: 5–6 h docs** (spec §9.8/§11.8; stdlib §2.14 rewrite + §2.15 `:985` + the 28 sites; rationale §5.19/§5.22/§18; changelog with a Migration note; `DECLINED.md`; code-generation reference `:763` + PIN move) **plus the prerequisite work of §3.11** (the arity ruling for a ramp: ~1–2 h; the `IDeserializable<Shared<Node>>` header trace and compiler probe plus the `TOut` admissibility ruling: ~2–3 h; the §9.8 generator design: ~half a day) **plus 1–2 weeks compiler** before any test can flip.

### 3.10 Corpus accounting

Ten corpus files, 46 occurrences, all text edits; no row deleted. `stdlib_0018` (`skip:signature-listing`) becomes the ▲ interface text; `stdlib_0019`/`0020` (the hand-written `Point`) gain `<Point>`; `stdlib_0027`/`0028` (the §2.15 facades) change constraint spelling. Rows remain at their current `skip` status until the compiler flips.

### 3.11 Verdict: defer — design recorded, three prerequisites named

The shape in 3.1 is the recommended accepted design. It is deferred on **cost and two unverified obligations** — not on any gap in the spec's statement of the three return shapes, all of which 3.2 fits to the one member. **Reopening bar** (all three before graduation):
1. **An owner decision on the break, and the ramp ruling it may need** (3.5): one hard revision across 90 files (621 occurrences), or a ramp — which requires a ruling that a bare `IDeserializable` and a generic `IDeserializable<T>` may coexist by arity (§2.8.2 `:263` makes arity part of type identity; no rule admits two same-named declarations in one scope today).
2. **The §9.8 synthesis-generator obligation designed** (3.4): the synthesizer must emit `Deserialize` as an interface-member witness that §11.2 matching recognizes and a constrained generic reaches, with the *hand-writing suppresses synthesis* rule restated in conformance terms — including the `X = Shared<C>` case §9.8 `:1546` specifies.
3. **`class Node: IDeserializable<Shared<Node>>` verified admissible** as a conformance header under §11.2/§11.8/§12.7 as written (a class conforming to a generic interface instantiated at a *different* class type, with a static witness `: owned Shared<Node>`) — by trace and by compiler probe (§11.8's examples and tests cover only self-instantiation) — and the `TOut` admissibility clause of 3.1 ruled with it.

**Sequencing, not a bar item:** after Part A (same machinery — conformance-keyed recognition, compiler-supplied witnesses; #1506's compiler work builds the query Part B reuses). **Filed regardless:** the spec-ahead-of-impl issue for the `owned Shared<C>` entry (3.3).

Until then, the record keeps §11.8:2079's Part B half honest (§4) and stdlib `:747`/`:769` name the recurrence rather than `Self` (edits S17, S18).

---

## 4. Composition with v0.292 (item 1c) and v0.293 (proposal 0124)

All three promotions edit one sentence (`:2079`): *"**What this unblocks:** promoting the name-recognized `FromCollectionLiteral` (§15.4.1) to a real `ICollectionLiteralTarget<T>` and `IDeserializable`'s `Deserialize` (stdlib §2.14) to a true static interface method — both signatures were chosen for zero-source-change promotion; and replacing §16's compiler-intrinsic `Range<T>` element restriction with an `ISteppable<T>` constraint."* The brief's hard rule: *whichever lands last deletes it; if two land in one revision, the composition gate is mandatory.* **v0.292 and v0.293 have landed** (HEAD `7561e19`, docs v0.293) and rewrote the sentence: the `Range<T>` clause is no longer a follow-up — *"§16's `Range<T>` element restriction is **not** among them: its admitted set is a strict subset of the `ISteppable<T>` conformers, so the constraint cannot replace it without widening ([§16.3](#sec-the-ranget-type), v0.292)"* (`:2087`) — and rationale `:2270` carries the matching *"ruled out as a follow-up in v0.292 (#1508)"* tail. Part A's edits A1 and R2 are therefore anchored on the **landed** text, not the v0.291 text: A1 keeps v0.292's `Range<T>` sentence verbatim and replaces only the promotion clause with ▲ *"**Recorded follow-up:** promoting `IDeserializable`'s `Deserialize` (stdlib §2.14) to a static interface member — the member signature is promotable unchanged; the conformance declaration becomes `: IDeserializable<T>` (#1507). The collection-literal protocol is already interface-based (§15.4.1, stdlib §2.19)."*; R2 replaces the fragment *"The follow-ups it unblocks (FromCollectionLiteral/IDeserializable promotion) are tracked issues;"* and leaves the `Range<T>` tail. Every other `old` in Appendix A is identical at `dc0b60d` and at HEAD (the ten stdlib headers included), so no further composition edit is owed to v0.292/v0.293; the composition gate at graduation is over the union with whatever lands between v0.293 and the graduation revision. The proposal number is settled: `0124-enum-name-parse.md` is `graduated-v0.293`.

---

## 5. Records a landing (or this deferral) must leave — `DECLINED.md` and rationale

**Decline-index rows (process §9 line 3), routed by section (C9) — on Part A's graduation:**

Into `### rejected` (`DECLINED.md:26`), alphabetized by Topic:

| Topic | Verdict class | Authoritative record | Reopening bar |
|---|---|---|---|
| *(update, `:75`)* Declaring both collection-literal factory shapes on one type | `rejected` | rationale §5.16 (*Ownership: two factory shapes*); spec §15.4.1 (own-declared dual conformance) | —(none stated). Exactly one shape per type — borrowing (`ReadOnlyArraySpan<T>`) or owning (`owned OwningArray<T>`) … *(carrier corrected from the stale `ReadOnlySpan<T>`, D1)* |
| One interface carrying both collection-literal forms (two statics, or a carrier type parameter) | `rejected` | [proposal 0125 §2.10](0125-static-interface-promotions.md); [proposal 0115 §3.3](0115-collection-literal-borrowing-span-family.md) (its bar, now met and kept closed) | —(none stated). Abstract-only statics force every conformer to supply both; contradicts *exactly one form per type*. |
| Structural/implicit conformance to `ICollectionLiteralTarget` (the interface as an internal predicate; the factory alone as the opt-in) | `rejected` | spec §15.4.1 (*Recognition category*); rationale §5.16; [proposal 0125 §2.6](0125-static-interface-promotions.md) | —(none stated). A second structural rule beside §17.5's `Contains`; buys prose only. |
| Compiler-provided conformance to the collection-literal target pair for the ten built-in sequence collections (the §11.8 primitive-tower device, or a witness-less conformance) | `rejected` | spec §12.7 clause 5 (scoped to primitives); §11.8 (*"supplies the static members … the §11.2 matching rule, applied to statics"*); [proposal 0125 §2.10 items 4 and 10](0125-static-interface-promotions.md); D-0009 | §12.7 gaining a class-scoped implicit-conformance case, or §11.8 gaining a witness-less conformance form — each a design change with its own proposal. |

*(Amended, in place at `:30` — D2:)*

| Topic | Verdict class | Authoritative record | Reopening bar |
|---|---|---|---|
| A `ToArray()` on `ReadOnlyArraySpan<T>`, riding the v0.272 collection-literal carrier fix | `rejected` (the `ToArray()` spelling); **bar met at v0.294 for the value, at the witness spelling** | [proposal 0115 §7 R3]; changelog v0.272; **amended v0.294: [proposal 0125 §2.3](0125-static-interface-promotions.md), D-0009** | unchanged for `ToArray()`. The value — `ReadOnlyArraySpan<T>` → `owned Array<T>` — arrived at v0.294 as `Array<T>.FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Array<T>`, the conformance witness §11.8 requires of a declared `ICollectionLiteralTarget<Array<T>, T>` conformer: structural, not *while-we're-at-it*. A second spelling of the same value is the dual this row still forbids. |

**Register entry (Part 2, `## D-0009`), on Part A's graduation:**

> ## D-0009 — Built-in collection-literal conformance without a callable witness; and the `DECLINED.md:30` reopening
>
> **Declined 2026-09-05 (v0.294, recorded from proposal 0125 §2.3/§2.10).** The ten built-in sequence collections conform to the collection-literal target pair (stdlib §2.19) by nominal declaration, and the conformance rule — spec §11.8: *"A conforming type declares `class Meters: INumber<Meters>` and supplies the static members with exact signatures (the §11.2 matching rule, applied to statics)"*; *"a runtime type conforms only by declaring the full member set, statics included, so there is no partial-conformance state to probe"* — has no form in which a type conforms without a callable static witness. Two alternatives were priced and declined: a **compiler-provided** conformance (§12.7 clause 5 scopes that device to primitives; extending it to ten classes is a new implicit-conformance case), and a **witness-less** conformance (a new conformance kind for ten types, in the same family). The consequence is that `Array<T>.FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Array<T>` — the value the `ToArray()` row at the top of the index declined on *"surface is debt"* and *while-we're-at-it* grounds — is supplied at the witness spelling. That row is amended rather than deleted: the new evidence against its stated reasons is that the member is a **structural consequence of the promotion the owner directed** (#3247, #1506), not a convenience riding a fix; its `ToArray()` spelling stays declined as a dual. Revisiting the witness requirement takes a §11.8 design change with its own proposal.

Into `### deferred` (`DECLINED.md:228`), alphabetized by Topic:

| Topic | Verdict class | Authoritative record | Reopening bar |
|---|---|---|---|
| Value-kind (record/choice) conformers as sequence-literal targets | `deferred` | spec §15.4.1 (the ▲ class-only sentence); [proposal 0125 §2.2, §2.10](0125-static-interface-promotions.md) | A traced ownership story for a record target under §15.4's literal-tree rule and §9.7's no-temporary-source rule, plus a use case. |

**On Part B's deferral (this proposal, even without graduation — the v0.262-era rule binds declines recorded outside a train):** into `### rejected`:

| Topic | Verdict class | Authoritative record | Reopening bar |
|---|---|---|---|
| A recurrence-constrained `IDeserializable<TSelf>` (#3339's shape) | `rejected` | [proposal 0125 §3.1, §3.7](0125-static-interface-promotions.md) | —(none stated). Cannot express the `owned Shared<C>` entry without a foreign-member bridge. |

and into `### deferred`:

| Topic | Verdict class | Authoritative record | Reopening bar |
|---|---|---|---|
| Promoting `IDeserializable.Deserialize` to a static interface member (#1507) | `deferred` | [proposal 0125 §3](0125-static-interface-promotions.md); spec §11.8 (*Recorded follow-up*); stdlib §2.14 | §3.11's three items: an owner decision on the 621-occurrence break (hard revision vs a ramp, the ramp needing an arity-coexistence ruling); the §9.8 synthesis-generator obligation designed (an interface-member witness); `IDeserializable<Shared<Node>>` verified admissible as a conformance header and the `TOut` admissibility clause ruled. Sequenced after Part A. |
| Retiring the `owned Shared<C>` deserialization entry shape (spec §9.8) | `deferred` | [proposal 0125 §3.7(3)](0125-static-interface-promotions.md) | Evidence that a self-referencing shared-node graph is unreachable in ownership-valid Bezel (e.g. every such graph is a §28.13 `Shared` cycle). |

**Rationale §18** (`:2268`): the static-interface-members entry's *"The follow-ups it unblocks … are tracked issues"* gains the shipped marker for Part A at graduation (v0.201 rule) and a pointer to this proposal for Part B (R2). `:2275` (*Custom collection-literal protocols*) restates the opt-in as conformance (R3). **Rationale §5.19** (`:752`): under Part B's deferral, the sentence *"The name and signature are chosen so that the promotion, when it lands, turns `Deserialize` into a real interface method with no source change"* is corrected to "with no change to the member" at Part A's graduation (it is in the same document Part A edits) — R4.

**README (N5):** Appendix B supplies the pre-graduation Live-queue row. At graduation the row **moves** from `## Live queue` to `## Graduated`, rewritten in the past tense with a bare number, with the status token `graduated-v0.NNN — superseded — Part A graduated v0.NNN …; Part B deferred (DECLINED.md)` on the 0077 precedent; `check_proposals.sh`'s SECTION and BIJECTION checks re-run.

---

## 6. Open questions for the owner

1. **Part A, sequencing:** confirm #3857 lands before the graduation (§2.7), or authorize a spec-first landing that stacks on the open delta.
2. **Part A, roster mechanism:** nominal declaration per type with the factory listed (coordinator-directed, §2.3) is what this draft carries; confirm.
3. **Part B:** accept the 3.1 shape (with its admissibility clause) as the recorded design and the three-item bar in 3.11; and whether the `owned Shared<C>` entry-shape impl-lag issue (§3.3) should be filed now rather than at Part B's graduation (it is spec-ahead-of-impl regardless of #1507).
4. **Part B, the break:** hard revision or ramp — the ramp needs the arity ruling (3.5).

---

## 7. Critique resolution (`CRITIQUE-0125.md`, 2026-09-05)

Every BLOCKING and CORRECTION is resolved in the text above and in Appendix A; each NOTE is applied or declined here.

| ID | Resolution |
|---|---|
| **B1** | **Applied (coordinator-directed).** The built-ins conform by §12.7 clause 1 as written: nominal declaration in each stdlib class header (S6–S16), factory listed as a member; no implicit case added to §12.7; §15.4's list derives from those declarations and says so (A2, S5). The `is`/`as` consequence is in §2.3 and the Migration note. |
| **B2** | **Applied (coordinator-directed, the critic's alternative fix).** The direct call stays, as a *declared* member: ten stdlib listings (S6–S16) and the code-generation §5-index line (G1) make it exist where a reader looks. §1.9 records that this honors #1424 rather than contradicting it. §2.12 item (v) stays priced. |
| **B3** | **Applied.** 17 sites / 4 files + 1 corpus everywhere (§0, §2.6, §2.8, §2.9, §2.11, §2.12, §2.13, Appendix B); `_3032` named as a re-authoring with the three other files listed (§2.6, §2.12 item vii); the diagnostic-text coupling in the Migration note. |
| **B4** | **Applied.** Predicate restricted to classes (A6, §2.2); an interface may extend the pair but an interface type is never a target, citing §11.8 `:2075` and `isCollectionLiteralInterfaceTarget` (`ExprTypecheck_Calls.cpp:316`). |
| **B5** | **Applied.** (a) uniqueness scoped to conformances the class declares on itself at `TSelf` = itself (A8, §2.2); (b) A4/A5 say "sequence-literal target under §15.4.1" / "borrowing-form target", not "conformer". |
| **B6** | **Applied.** New edits A7 (`:3042–:3053`, both form blocks re-spelled as the interface members' witnesses, code blocks kept as `C<T>`-spelled listings) and A9 (`:3058`'s "that declares `FromCollectionLiteral`"); §2.6/§2.9 corrected to say the blocks stay and are re-transcribed. |
| **B7** | **Applied.** Declaration-site migration diagnostic made normative (A8's last sentence; §2.2 last row), named as a compiler obligation (§2.12 item iv) and in the Migration note. |
| **B8** | **Applied.** Pin move stated as required (§2.12); §5-index edit G1 at `:765` carries the pair and the ten conformers. |
| **C1** | **Reverted (P2-B2) — the pass-1 finding was false.** `owned Shared<C>` is at spec §9.8 `:1546` at both `dc0b60d` and `7561e19` (`grep -c` = 1 each); the first draft's citation was right. Every absence assertion removed (§0, Currency, §3.3, §3.11, the `deferred` row); Part B re-grounded per the coordinator on cost, the §9.8 generator obligation, and the unverified `IDeserializable<Shared<Node>>` header. |
| **C2** | **Applied.** #3831 cited in §2.3 and the header; the filing obligation and former Q5 removed. |
| **C3** | **Applied.** 621 everywhere (§0, §3.5, §3.6, Appendix B). |
| **C4** | **Applied.** §1.4 cites §12.7 `:2491–2496` as the authority; `:2075` re-attributed to §11.8; §11.2 `:1900` marked as too weak on its own. |
| **C5** | **Applied.** `:3056` (A8, §2.2), `:3402` (A12, §2.5), `LlvmBackend_Setup.cpp:31450` (§2.4, §2.7, §2.12), `:1576–1751` (§2.2, §2.12). |
| **C6** | **Applied.** S18 fixes stdlib `:747`. |
| **C7** | **Applied.** S1 (heading names the pair), S2 (opening sentence names the ten collection conformances), S3 (producer-markers list gains `FromCollectionLiteral`). |
| **C8** | **Applied.** A2 says "the built-in **sequence** collections above". |
| **C9** | **Applied.** §5 routes every row to `### rejected` / `### deferred`, alphabetized; D1 fixes `:75`'s carrier; the `:3056` `ReadOnlySpan<T>` leftover is named as an editorial repair (§2.8, §2.11). |
| **C10** | **Applied, both ways.** Admissibility clause added to 3.1 (`TOut ∈ {C, Shared<C>}` / `TOut = V`) **and** a fourth reopening-bar item (3.11), since the `Shared<C>` arm is the undefined predicate. |
| **C11** | **Applied.** Type-parameter targets ruled *admitted* (A6, §2.2), listed as newly-defined (§2.11), reason recorded (§2.10 item 6), test obligation (§2.12). |
| **N1** | **Applied.** "Sequence-literal target" stated as declaration-derived, no §3.6 row (A6, §2.2). Grammar-productions finding recorded (none needed). |
| **N2** | **Accepted, no change** (mechanism, shape, split verified). |
| **N3** | **Applied.** A6 defines *borrowing-form target* / *owning-form target*; `:2960` still takes no edit (§2.4). |
| **N4** | **Accepted, no change** (roster exhaustive). |
| **N5** | **Applied.** README consequences in §5 (row moves to Graduated; second row authored at landing); 0124/0125 numbering contingency in the header and §4. |
| **N6** | **Applied.** R5 (`:687`) and R6 (`:689`) re-frame the two sentences; §2.5 says why. |
| **N7** | **Accepted, no change** — no ramp owed; recorded in §2.10 item 7. |
| **N8** | **Applied.** §2.12's tracking-issue content adds the built-in symbol surface (ii), the migration diagnostic (iv) and the `_3032` rewrite (vii); item (v) stays because B2's alternative was taken. |

**Pass 2 (`CRITIQUE-0125-pass2.md`, 2026-09-05; measured at HEAD `7561e19`).**

| ID | Resolution |
|---|---|
| **P2-B1** | **Applied.** `DECLINED.md:30` named in §2.3 with the structural evidence (§11.8 `:2061`/`:2075` quoted — no witness-less conformance form); the alternative priced and declined (§2.10 item 10); the ten factories in §2.8's cost column as well as its gain column; the row amended in place (D2) and the reasoning registered as D-0009 (§5); disclosed in the changelog (Appendix C). |
| **P2-B2** | **Applied (coordinator-directed).** Pass 1's C1 reversed: §0, Currency, §3.3, §3.11 and §5's `deferred` row no longer assert any absence; §3.3 re-grounded on *stated at `:1546`, unimplemented, unused, untracked* and demoted to a tracking item; the bar is now cost + the §9.8 generator obligation + the unverified `IDeserializable<Shared<Node>>` header (three items; Part A is sequencing). |
| **P2-B3** | **Applied.** A9 carves the ten built-ins out of §15.4.1's *Lowering* (direct intrinsic construction under §15.4; the listed factory produces the same value; materialize-then-call governs the other targets); priced in §2.12 item (v); P2-N3's identity body is the sharpest instance and is covered by the same sentence. |
| **P2-B4** | **Applied.** S19 rewrites stdlib `:732`'s *"recognizes … by name, exactly as … recognizes `FromCollectionLiteral`"* to the dated past tense pointing at the §2.19 pair. |
| **P2-B5** | **Applied.** Appendix C adopts the critic's Appendix F entry as the v0.294 changelog entry, edited for P2-B1 (the `:30` disclosure), P2-B2 (Part B's corrected grounds), P2-B3 (the carve), P2-B4 (S19 in the counts), P2-C1/C3 (rationale counts), P2-C6/D-0009 (the register), P2-C8 (the pin reason), and a gate-round line that says what bit in **both** passes. |
| **P2-C1** | **Applied.** R4b re-frames rationale `:752`'s cross-reference to *"before that protocol's promotion"*. |
| **P2-C2** | **Applied.** §2.11 gains the *newly-accepted* bullet; §2.12 item (vii) says `_3032`'s cells have successors only once each declares the conformance (cell 1 has none). |
| **P2-C3** | **Applied.** R7 rewrites the two body clauses of rationale `:689` (*"declaring a specifically-named static method"*, *"the recognized factory signature does exactly that"*) into the past tense. |
| **P2-C4** | **Applied.** A2: *"the bullets above restate those declarations rather than defining the set"*. |
| **P2-C5** | **Applied.** A6's closing sentence: *"A sequence literal in a context targeting a type that is not a sequence-literal target is the usual … compile error."* |
| **P2-C6** | **Applied.** Eighth index row (compiler-provided / witness-less conformance for the built-ins, `rejected`, bar: a class-scoped implicit case or a witness-less form, each its own proposal) in §5, plus D-0009. |
| **P2-C7** | **Applied.** §2.9: `spec_0073`/`spec_0074` unchanged, no re-transcription. |
| **P2-C8** | **Applied.** Pin reason corrected in §2.8 cost 5 and §2.12 (v0.293 at HEAD; process §9's clause compels the move). |
| **P2-C9** | **Applied.** `X<T>` removed from S5 and §2.3 (*"each conforms at itself — `Array<T>` to `ICollectionLiteralTarget<Array<T>, T>`, and so on"*). |
| **P2-C10** | **Applied.** Appendix B now carries the graduation-form `**Status**` cell and the `## Graduated` README row beside the Live-queue row. |
| **P2-C11** | **Applied.** The Currency block states the `dc0b60d` → `7561e19` offsets (spec +33, rationale +2, code-gen +2, `DECLINED.md` +1, stdlib 0). |
| **P2-N1, N2, N5** | **Accepted, no change** (no FACTS/TABLE/SETS/REGISTER contact; EXTRACT +1; `_3032` already on `runAot` — noted in §2.12). |
| **P2-N3** | **Applied** via A9's carve sentence. |
| **P2-N4** | **Applied.** A10: *"produces exactly what the literal produces"*. |

**Mechanical re-check.** Every `old` block in Appendix A (44 blocks after pass 2, multi-line anchors included — `OwningArray<T>`'s duplicated header is disambiguated by its second line) was extracted programmatically and counted as an exact substring against `git show 7561e19:<doc>` (HEAD, docs v0.293): all **= 1** (44/44). Against `dc0b60d` the same pass finds 42/44, the two misses being exactly A1 and R2, which are anchored on the v0.292 text that replaced their v0.291 lines (§4). `scripts/check_docs.sh` and `scripts/check_proposals.sh` were run green by the pass-2 critic on the pass-1 set (513 fragments, +1 as predicted); the pass-2 corrections (S19, R4b, R7, D2, the A9/A2/A6/A10 wording) must be dry-run again at graduation. `scripts/check_docs.sh` was **not** re-run after the revision (the critic's scratch tree ran the first draft's set; the new S-series edits touch signature listings inside fenced blocks and the §2.19 heading — the critic's C7 note that ANCHOR/FENCE/EXTRACT stay green on such edits applies, but the graduation must re-run the full gate on the new set, with the pin moved).

---

## Appendix A — Part A edit-set (graduation; `old → new`, verbatim, each `old` unique at `dc0b60d`)

*Dry-run before landing (process §9). Every `old` is unique at `dc0b60d` (40/40, exact-substring count over the whole document, multi-line blocks included); A1 and R2 are anchored on the v0.292 text that replaced their v0.291 lines (HEAD `7dd9a63`, §4) and are unique there. Line numbers are the `dc0b60d` reader's except where an edit says otherwise.*

### Spec

**A1 — §11.8, the *What this unblocks* sentence — `old` re-anchored on the landed v0.292 text (`:2087` at HEAD `7dd9a63`; the v0.291 line at `:2079` no longer exists).**
old:
```
**What this unblocks:** promoting the name-recognized `FromCollectionLiteral` (§15.4.1) to a real `ICollectionLiteralTarget<T>` and `IDeserializable`'s `Deserialize` (stdlib §2.14) to a true static interface method — both signatures were chosen for zero-source-change promotion. §16's `Range<T>` element restriction is **not** among them: its admitted set is a strict subset of the `ISteppable<T>` conformers, so the constraint cannot replace it without widening ([§16.3](#sec-the-ranget-type), v0.292).
```
new:
```
**Recorded follow-up:** promoting `IDeserializable`'s `Deserialize` (stdlib §2.14) to a static interface member — the member signature is promotable unchanged; the conformance declaration becomes `: IDeserializable<T>` (#1507). The collection-literal protocol is already interface-based ([§15.4.1](#sec-the-user-defined-sequence-literal-protocol), stdlib §2.19). §16's `Range<T>` element restriction is **not** a follow-up: its admitted set is a strict subset of the `ISteppable<T>` conformers, so the constraint cannot replace it without widening ([§16.3](#sec-the-ranget-type), v0.292).
```

**A2 — §15.4, `:2948`, the target-list bullet.**
old:
```
- Any user-defined type declaring the `FromCollectionLiteral` static factory ([§15.4.1](#sec-the-user-defined-sequence-literal-protocol)); sequence-literal form only.
```
new:
```
- Any class that is a sequence-literal target under [§15.4.1](#sec-the-user-defined-sequence-literal-protocol) — one conforming to `ICollectionLiteralTarget<TSelf, T>` or `IOwningCollectionLiteralTarget<TSelf, T>` (stdlib [§2.19](Bezel_Standard_Library.md#sec-static-abstractions)) at `TSelf` = itself. The built-in **sequence** collections above are exactly such classes: each declares the conformance in its standard-library declaration (stdlib §4.3), so the bullets above restate those declarations rather than defining the set; sequence-literal form only.
```

**A3 — §15.4, `:2950`, the sequence-form sentence (fragment).**
old:
```
`Queue<T>`/`OwningQueue<T>`, and user-defined types per [§15.4.1](#sec-the-user-defined-sequence-literal-protocol). The **dictionary literal** form
```
new:
```
`Queue<T>`/`OwningQueue<T>`, and every other sequence-literal target per [§15.4.1](#sec-the-user-defined-sequence-literal-protocol). The **dictionary literal** form
```

**A4 — §15.4, `:2962`, D4's borrowing-family target list.**
old:
```
For **borrowing-family** targets (`Array<T>`, `Set<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>`, the key and value positions of `Dictionary<K, V>`, and a user-defined type whose `FromCollectionLiteral` declares the borrowing form ([§15.4.1](#sec-the-user-defined-sequence-literal-protocol))), each element expression
```
new:
```
For **borrowing-family** targets (every borrowing-form target under [§15.4.1](#sec-the-user-defined-sequence-literal-protocol) — `Array<T>`, `Set<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>`, and any user class that is one — and the key and value positions of `Dictionary<K, V>`), each element expression
```

**A5 — §15.4, `:2962`, D4's nested-fresh-producer clause.**
old:
```
a nested collection literal targeting a non-trivial-dispose collection type (built-in, or a user-declared target of the [§15.4.1](#sec-the-user-defined-sequence-literal-protocol) protocol, whose `FromCollectionLiteral` result is `owned`), or an `owned`-returning call
```
new:
```
a nested collection literal targeting a non-trivial-dispose collection type (built-in, or any sequence-literal target under [§15.4.1](#sec-the-user-defined-sequence-literal-protocol) — the factory result is `owned TSelf`), or an `owned`-returning call
```

**A6 — §15.4.1, `:3040`, the opt-in paragraph.**
old:
```
A user-defined type opts into sequence-literal construction by declaring a static method named exactly `FromCollectionLiteral` with one of two recognized signatures. The presence of such a method is the opt-in; a type without it is not a sequence-literal target, and a sequence literal in a context targeting such a type is the usual "cannot infer target type" / "no applicable target" compile error.
```
new:
```
A **class** opts into sequence-literal construction by conforming ([§12.7](#sec-interface-conformance)) to one of two static interfaces (stdlib [§2.19](Bezel_Standard_Library.md#sec-static-abstractions), [§11.8](#sec-static-interface-members)) — `ICollectionLiteralTarget<TSelf, T>`, which makes it a **borrowing-form target**, or `IOwningCollectionLiteralTarget<TSelf, T>`, which makes it an **owning-form target** — with `TSelf` the conforming class itself at its own instantiation, and supplying the one static member each declares, `FromCollectionLiteral`, under [§11.2](#sec-interface-declaration)'s exact-signature matching. A class `C` is a **sequence-literal target** exactly when it conforms to one of the two interfaces at `TSelf = C`; a conformance at any other `TSelf` is an ordinary conformance and not a target. A generic type parameter whose constraints guarantee that conformance at `TSelf` = the parameter ([§12.7](#sec-interface-conformance)) is likewise a sequence-literal target within the body that declares it; the lowering below then dispatches through the constraint ([§11.8](#sec-static-interface-members)). An interface type is never a sequence-literal target, whatever it extends ([§11.8](#sec-static-interface-members): the interface name declares no body to run). Being a sequence-literal target is derived from a type's declared conformances, not from its structure — it is not a [§3.6](#sec-derived-type-properties) derived type property. The built-in sequence collections declare the conformance in their standard-library declarations (stdlib §4.3). A sequence literal in a context targeting a type that is not a sequence-literal target is the usual "cannot infer target type" / "no applicable target" compile error.
```

**A7 — §15.4.1, `:3042` and `:3050`, the two form headings (their code blocks stay as `C<T>`-spelled witness listings; corpus `spec_0073`/`spec_0074` re-transcribed by hand).**
old:
```
**Borrowing form.** For a type `C<T>` whose elements are borrows (the type does not own its elements):
```
new:
```
**Borrowing form.** For a class `C<T>` whose elements are borrows (the type does not own its elements), the conformance is `ICollectionLiteralTarget<C<T>, T>` and the witness — stdlib §2.19's member at `TSelf = C<T>` — is:
```
old:
```
**Owning form.** For a type `C<T>` that owns its elements (`where T: class`):
```
new:
```
**Owning form.** For a class `C<T>` that owns its elements (`where T: class`), the conformance is `IOwningCollectionLiteralTarget<C<T>, T>` and the witness is:
```

**A8 — §15.4.1, `:3056`, the conjunct paragraph (also retires the `ReadOnlySpan<T>` leftover the paragraph still carries — an editorial repair the changelog names).**
old:
```
Exactly one of the two forms may be declared on a given type. Declaring both is a compile error (the literal's element-expression ownership would not unambiguously select a target). The method must be `public`, must be `static`, must be declared directly on the collection type (not inherited from a base class or supplied by an extension block), and must return `owned C<T>` where `C<T>` is the enclosing type at its own arity. The element type is read directly from the parameter's element type — `T` in `ReadOnlySpan<T>` or `OwningArray<T>` — and is the type against which every literal element expression is checked.
```
new:
```
A class declares, on itself, at most one of the two conformances at `TSelf` = itself, at one element type: declaring both interfaces, or one of them at two element types, is a compile error at the declaration (the literal's element-expression ownership would not unambiguously select a target). A conformance inherited from a base class is at `TSelf` = the base and neither counts against this rule nor makes the derived class a target; a derived class that wants to be one declares its own conformance. An extension block cannot declare conformance ([§26](#sec-extension-blocks)). A value kind — record, choice, or enum — may not conform to either interface: the literal's result is a fresh owned heap value ([§15.4](#sec-collection-literals)'s literal-tree rule; *Result ownership* below), and the [§11.8](#sec-static-interface-members) erasure of the member's `owned` return at a value kind does not make one a target (a compile error at the declaration; recorded liftable, proposals `DECLINED.md`). The element type is the conformance's `T` — read from the declaration, not inferred — and is the type against which every literal element expression is checked. A class that declares a `public static` method named `FromCollectionLiteral` whose signature matches either interface's member at `TSelf` = the class, but that conforms to neither interface, is a compile error at that declaration naming the missing conformance and its remedy (add `: ICollectionLiteralTarget<C, T>` or `: IOwningCollectionLiteralTarget<C, T>` to the class header).
```

**A9 — §15.4.1, `:3058`, the *Lowering* sentence.**
old:
```
**Lowering.** For a sequence literal `[e1, e2, ..., en]` with determined target type `C<U>` that declares `FromCollectionLiteral`:
```
new:
```
**Lowering.** For the built-in sequence collections (stdlib §4.3) the literal is constructed directly under [§15.4](#sec-collection-literals)'s element-order rules; their listed `FromCollectionLiteral` is the intrinsic factory that produces the same value, and the lowering below governs every other sequence-literal target. For a sequence literal `[e1, e2, ..., en]` with determined target type `C<U>` that is such a target (a conforming user class, or a type parameter constrained to conform):
```

**A10 — §15.4.1, `:3067`, the *Recognition category* paragraph.**
old:
```
**Recognition category.** This is name-based recognition: the compiler looks up a specifically-named `public static` method with a recognized signature. It is distinct from the interface-based iteration protocol ([§29.1](#sec-the-protocol)) and from the single structural-dispatch rule for `Contains` / `in` ([§17.5](#sec-the-in-operator)). The dictionary-literal form is *not* covered by this protocol; only the bare sequence-literal form `[e1, ..., en]` participates. Rationale [§5.16](Bezel_Design_Rationale.md#sec-collection-literals-for-complex-collections) explains the choice of mechanism, the contrast with C#'s `[CollectionBuilder]` attribute, and the forward-compatibility path to an interface-based form: now that static interface members exist ([§11.8](#sec-static-interface-members)), promoting `FromCollectionLiteral` to a real static interface method is a recorded follow-up.
```
new:
```
**Recognition category.** This is interface-based recognition, like the iteration protocol ([§29.1](#sec-the-protocol)) and unlike the single structural-dispatch rule for `Contains` / `in` ([§17.5](#sec-the-in-operator)). The dictionary-literal form is *not* covered by this protocol; only the bare sequence-literal form `[e1, ..., en]` participates. Because the built-in sequence collections declare the conformance and list the factory (stdlib [§4.3](Bezel_Standard_Library.md#sec-how-the-borrowing-and-owning-hierarchies-relate)), a generic body constrained by either interface may construct any target through `TSelf.FromCollectionLiteral(…)` or through a literal, and a direct static call on a built-in's listed factory produces exactly what the literal produces. Rationale [§5.16](Bezel_Design_Rationale.md#sec-collection-literals-for-complex-collections) explains the choice of mechanism and the contrast with C#'s `[CollectionBuilder]` attribute.
```

**A11 — §15.4.1, `:3072`, the example header (two-line anchor).**
old:
```
public class RingBuffer<T>
  private owned _slots: Array<T>;
```
new:
```
public class RingBuffer<T>: ICollectionLiteralTarget<RingBuffer<T>, T>
  private owned _slots: Array<T>;
```

**A12 — §17.5, `:3402`, the taxonomy sentence (fragment).**
old:
```
This is the only structural-dispatch rule in Bezel's expression surface (the iteration protocol of [§29.1](#sec-the-protocol) is interface-based by contrast; the user-defined collection-literal protocol of [§15.4.1](#sec-the-user-defined-sequence-literal-protocol) is name-based — it requires a method of one *exact* name, not any method of a matching shape, so it is recognition-by-declared-name rather than structural duck-typing).
```
new:
```
This is the only structural-dispatch rule in Bezel's expression surface (the iteration protocol of [§29.1](#sec-the-protocol) and the collection-literal protocol of [§15.4.1](#sec-the-user-defined-sequence-literal-protocol) are interface-based by contrast).
```

### Standard library

**S1 — §2.19 heading, `:1115`.**
old:
```
### 2.19 `INumber<T>`, `ISteppable<T>`, `IParsable<T>` (v0.176, #807)
```
new:
```
### 2.19 `INumber<T>`, `ISteppable<T>`, `IParsable<T>`, and the collection-literal target pair (v0.176, #807)
```

**S2 — §2.19 opening, `:1117` (fragment).**
old:
```
The primitive numeric tower and `char` conform via compiler-provided conformances; user types conform by declaring and supplying the statics.
```
new:
```
The primitive numeric tower and `char` conform to the first three via compiler-provided conformances; user types conform by declaring and supplying the statics. The collection-literal target pair at the end of this section is conformed to by declaration only — by the ten built-in sequence collections in their own declarations ([§4.3](#sec-how-the-borrowing-and-owning-hierarchies-relate)) and by user classes.
```

**S3 — §2.19 *Producer markers*, `:1143` (fragment).**
old:
```
**Producer markers (v0.196).** Every §2.19 member that produces a value spells the production: `owned` returns on `Zero`, `One`, the four arithmetic operators, `Step`, and `Parse`, and the `out owned` slot on `TryParse`
```
new:
```
**Producer markers (v0.196).** Every §2.19 member that produces a value spells the production: `owned` returns on `Zero`, `One`, the four arithmetic operators, `Step`, `Parse`, and both `FromCollectionLiteral` members, and the `out owned` slot on `TryParse`
```

**S4 — §2.19, insert the pair before `<a id="sec-result"></a>` (`:1145`).**
old:
```
<a id="sec-result"></a>
### 2.20 `Result<T, E>` (v0.200)
```
new:
```
**The collection-literal target pair.** The two static interfaces behind the sequence-literal protocol (language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol)): a target conforms at `TSelf` = itself and supplies the one factory; exactly one of the two per class, declared on the class itself. The ten built-in sequence collections declare the conformance and list the factory in their own sections ([§4.3](#sec-how-the-borrowing-and-owning-hierarchies-relate)). Neither interface is variant (language spec [§11.8](Bezel_Language_Spec.md#sec-static-interface-members), consequence 2).

```bezel
public interface ICollectionLiteralTarget<TSelf, T>
    where TSelf: ICollectionLiteralTarget<TSelf, T>
  static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned TSelf;   // the borrowing form: elements
end ICollectionLiteralTarget;                                                           //   are borrows (§4.15 view)

public interface IOwningCollectionLiteralTarget<TSelf, T>
    where TSelf: IOwningCollectionLiteralTarget<TSelf, T>
    where T: class
  static method FromCollectionLiteral(owned elements: OwningArray<T>): owned TSelf;   // the owning form: the factory
end IOwningCollectionLiteralTarget;                                                    //   drains the array (§4.5)
```

<a id="sec-result"></a>
### 2.20 `Result<T, E>` (v0.200)
```

**S5 — §4.3, `:1532`, the roster sentence (append to the paragraph).**
old:
```
The two hierarchies share `IEnumerable<T>`, `IReadOnlyCollection<T>`, and `IReadOnlyList<T>` as common ancestors. A function that wants to read elements without taking ownership accepts one of these interfaces; both borrowing and owning collections of T conform.
```
new:
```
The two hierarchies share `IEnumerable<T>`, `IReadOnlyCollection<T>`, and `IReadOnlyList<T>` as common ancestors. A function that wants to read elements without taking ownership accepts one of these interfaces; both borrowing and owning collections of T conform. The sequence collections are also collection-literal targets, and each **declares** it (language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol); the pair is at [§2.19](#sec-static-abstractions)): each conforms at itself — `Array<T>`, `LinkedList<T>`, `Set<T>`, `Stack<T>`, and `Queue<T>` to `ICollectionLiteralTarget` (`Array<T>` to `ICollectionLiteralTarget<Array<T>, T>`, and so on), and `OwningArray<T>`, `OwningLinkedList<T>`, `OwningSet<T>`, `OwningStack<T>`, and `OwningQueue<T>` to `IOwningCollectionLiteralTarget` (`OwningArray<T>` to `IOwningCollectionLiteralTarget<OwningArray<T>, T>`, and so on) — the conformance is in each class's declaration below and the factory is a listed member of each, whose body is the literal's own lowering (language spec [§15.4](Bezel_Language_Spec.md#sec-collection-literals)). Language spec §15.4's target list is derived from these declarations. The dictionaries take the dictionary-literal form only and conform to neither.
```

**S6 — §4.4 `Array<T>`, `:1557` (two-line anchor).**
old:
```
public class Array<T>: IList<T>
  public constructor();
```
new:
```
public class Array<T>: IList<T>, ICollectionLiteralTarget<Array<T>, T>
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Array<T>;   // §2.19 witness; the literal's
                                                                                                //   own lowering (spec §15.4)
  public constructor();
```

**S7 — §4.5 `OwningArray<T>`, `:1644` (two-line anchor — the signature listing).**
old:
```
public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T> where T: class
                                     // IOwningEnumerable: stated v0.195 — §4.13/spec §29.2's both-implemented rule presupposed it
```
new:
```
public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T>, IOwningCollectionLiteralTarget<OwningArray<T>, T> where T: class
                                     // IOwningEnumerable: stated v0.195 — §4.13/spec §29.2's both-implemented rule presupposed it
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningArray<T>;   // §2.19 witness: the
                                                                                                        //   transferred array is the result
```

**S8 — §4.5 `OwningArray<T>` *Reference implementation*, `:1723` (two-line anchor — the header).**
old:
```
public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T> where T: class
  private owned _storage: OwningBuffer<T>;
```
new:
```
public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T>, IOwningCollectionLiteralTarget<OwningArray<T>, T> where T: class
  private owned _storage: OwningBuffer<T>;
```

**S9 — §4.5 `OwningArray<T>` *Reference implementation*, the body (anchor: the parameterless constructor, `:1727–1729`).**
old:
```
  public constructor()
    this(8);
  end constructor;
```
new:
```
  public constructor()
    this(8);
  end constructor;

  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningArray<T>
    return elements;                                   // the literal's temporary is the result (spec §15.4.1, owning form)
  end FromCollectionLiteral;
```

**S10 — §4.6 `LinkedList<T>`, `:1838`.**
old:
```
public class LinkedList<T>: ICollection<T>
  public constructor();
```
new:
```
public class LinkedList<T>: ICollection<T>, ICollectionLiteralTarget<LinkedList<T>, T>
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned LinkedList<T>;   // §2.19 witness (spec §15.4)
  public constructor();
```

**S11 — §4.7 `OwningLinkedList<T>`, `:1879`.**
old:
```
public class OwningLinkedList<T>: IOwningCollection<T> where T: class
  public constructor();
```
new:
```
public class OwningLinkedList<T>: IOwningCollection<T>, IOwningCollectionLiteralTarget<OwningLinkedList<T>, T> where T: class
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningLinkedList<T>;   // §2.19 witness
  public constructor();
```

**S12 — §4.10 `Set<T>`, `:2235`.**
old:
```
public class Set<T>: ICollection<T>
  public constructor();
```
new:
```
public class Set<T>: ICollection<T>, ICollectionLiteralTarget<Set<T>, T>
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Set<T>;   // §2.19 witness; duplicates
                                                                                              //   collapse (spec §15.4)
  public constructor();
```

**S13 — §4.10 `OwningSet<T>`, `:2251`.**
old:
```
public class OwningSet<T>: IOwningCollection<T> where T: class
  public constructor();
```
new:
```
public class OwningSet<T>: IOwningCollection<T>, IOwningCollectionLiteralTarget<OwningSet<T>, T> where T: class
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningSet<T>;   // §2.19 witness
  public constructor();
```

**S14 — §4.11 `Stack<T>`, `:2302`.**
old:
```
public class Stack<T>: IReadOnlyCollection<T>   // v0.165 (#1354)
  public constructor();
```
new:
```
public class Stack<T>: IReadOnlyCollection<T>, ICollectionLiteralTarget<Stack<T>, T>   // v0.165 (#1354)
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Stack<T>;   // §2.19 witness; first element
                                                                                                //   pushed first (spec §15.4)
  public constructor();
```

**S15 — §4.11 `OwningStack<T>`, `:2313`.**
old:
```
public class OwningStack<T>: IReadOnlyCollection<T> where T: class   // v0.222 (#1811): demoted, completing v0.165 #1354
  public constructor();
```
new:
```
public class OwningStack<T>: IReadOnlyCollection<T>, IOwningCollectionLiteralTarget<OwningStack<T>, T> where T: class   // v0.222 (#1811): demoted, completing v0.165 #1354
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningStack<T>;   // §2.19 witness
  public constructor();
```

**S16 — §4.11 `Queue<T>` (`:2325`) and `OwningQueue<T>` (`:2336`).**
old:
```
public class Queue<T>: IReadOnlyCollection<T>   // v0.165 (#1354)
  public constructor();
```
new:
```
public class Queue<T>: IReadOnlyCollection<T>, ICollectionLiteralTarget<Queue<T>, T>   // v0.165 (#1354)
  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Queue<T>;   // §2.19 witness; first element
                                                                                                //   at the front (spec §15.4)
  public constructor();
```
old:
```
public class OwningQueue<T>: IReadOnlyCollection<T> where T: class   // v0.222 (#1811)
  public constructor();
```
new:
```
public class OwningQueue<T>: IReadOnlyCollection<T>, IOwningCollectionLiteralTarget<OwningQueue<T>, T> where T: class   // v0.222 (#1811)
  public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned OwningQueue<T>;   // §2.19 witness
  public constructor();
```

*(Declaration count, verifiable by `grep -n` on each header line at `dc0b60d`: **10 conformance declarations on 10 types over 11 header lines** — `:1557`, `:1644`, `:1723`, `:1838`, `:1879`, `:2235`, `:2251`, `:2302`, `:2313`, `:2325`, `:2336` — and **11 member additions**: 10 listed signatures (S6, S7, S10–S16) and 1 body (S9).)*

**S17 — §2.14, `:769`, the `Self` sentence (both parts; fixes the deferred/rejected inconsistency).**
old:
```
Were a `Self`-return type ever added (the separately-deferred feature this section names above — static interface members themselves exist since v0.176, spec §11.8), `Deserialize` could be promoted to an actual `IDeserializable` interface method with the identical name and signature, without breaking any code — the name and shape were chosen now to be promotable, the same forward-compatibility discipline applied to `FromCollectionLiteral`.
```
new:
```
Promoting `Deserialize` to a static interface member is a recorded follow-up (spec §11.8; #1507): it takes the type-parameter form of spec §11.8 — a self-type keyword is declined (proposals `DECLINED.md`) — with the member's name and signature unchanged and the conformance declaration becoming `: IDeserializable<T>`; the same discipline promoted `FromCollectionLiteral` ([§2.19](#sec-static-abstractions)).
```

**S18 — §2.14, `:747`, the second `Self` clause (fragment).**
old:
```
Bezel has no `Self` type (that feature is separately deferred), so the return type is spelled with the concrete type name.
```
new:
```
Bezel has no `Self` type (a self-type keyword is declined — spec §11.8; proposals `DECLINED.md`), so the return type is spelled with the concrete type name.
```

**S19 — §2.14, `:732`, the by-name teaching (fragment; P2-B4 — dated, not narrated: the sentence is already v0.114-dated).**
old:
```
exactly as the user-defined collection-literal protocol recognizes `FromCollectionLiteral` ([§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol) of the language spec)
```
new:
```
as the user-defined collection-literal protocol recognized `FromCollectionLiteral` before its promotion to the [§2.19](#sec-static-abstractions) target pair (language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol))
```

### Rationale

**R1 — §5.16, `:697`, the *A new recognition category* paragraph.**
old:
```
*A new recognition category, named honestly.* Bezel now has three ways the compiler recognizes a type's participation in a language construct: interface-based (the iteration protocol, [§29.1](Bezel_Language_Spec.md#sec-the-protocol)); the single structural-by-shape rule (`Contains`/`in`); and now name-based (`FromCollectionLiteral`). Name-based recognition — "declare this exact `public static` method with a recognized signature" — is the closest available analogue to C#'s explicit attribute opt-in given that Bezel has neither attributes nor (yet) static interface members. Static interface members have since shipped (v0.176, spec §11.8, with `ISteppable<T>` now real — stdlib §2.19), and promoting `FromCollectionLiteral` to an `ICollectionLiteralTarget<T>` interface method with the same name and signature is the recorded follow-up (spec §11.8) — recognition shifts from name-based to interface-based without breaking any user code. The name and signature were chosen to be interface-promotable, and that is now an open path rather than a bet on a future feature.
```
new:
```
*Two recognition categories, not three.* Bezel has two ways the compiler recognizes a type's participation in a language construct: interface-based (the iteration protocol, [§29.1](Bezel_Language_Spec.md#sec-the-protocol), and the collection-literal target pair, stdlib §2.19); and the single structural-by-shape rule (`Contains`/`in`). The literal protocol's opt-in is a declared conformance to `ICollectionLiteralTarget<TSelf, T>` or `IOwningCollectionLiteralTarget<TSelf, T>` — two interfaces because the two forms carry different element ownership and exactly one is admitted per class, and abstract-only statics would otherwise force every conformer to supply both. The factory's name and signature are the ones the by-name protocol recognized, so promoting it cost each conformer one header clause and nothing else; the built-in collections declare the same conformance in their own standard-library declarations and list the factory, which is what lets a constrained generic construct any target and lets the factory be called by name where a reader can find it. Two alternatives were declined: an implicit conformance inferred from the factory alone, which would be a second structural rule beside `Contains` and would leave `TSelf.FromCollectionLiteral` unreachable through a constraint; and a compiler-provided conformance for the built-ins on the primitive-tower model, which spec §12.7 scopes to primitives and which would make ten callable members exist in no standard-library listing.
```

**R2 — §18, the shipped marker (v0.201 rule) — `old` re-anchored on the landed v0.292 text (`:2270` at HEAD `7dd9a63`, fragment; the line's `Range<T>` tail is untouched).**
old:
```
The follow-ups it unblocks (FromCollectionLiteral/IDeserializable promotion) are tracked issues;
```
new:
```
Of the follow-ups it unblocks, the `FromCollectionLiteral` promotion *shipped* (stdlib §2.19's target pair; proposal 0125 Part A) and the `IDeserializable` promotion is recorded and deferred (proposal 0125 Part B, #1507);
```

**R3 — §18, `:2275`, the *Custom collection-literal protocols* entry (fragment).**
old:
```
As of v0.111, user-defined types opt into *sequence*-literal construction by declaring the `FromCollectionLiteral` static factory (language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol)); both borrowing (`ReadOnlyArraySpan<T>` parameter) and owning (`owned OwningArray<T>` parameter) shapes are supported.
```
new:
```
User-defined classes opt into *sequence*-literal construction by conforming to `ICollectionLiteralTarget<TSelf, T>` (borrowing, `ReadOnlyArraySpan<T>` parameter) or `IOwningCollectionLiteralTarget<TSelf, T>` (owning, `owned OwningArray<T>` parameter) — stdlib §2.19, language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol).
```

**R4 — §5.19, `:752`, the zero-source-change clause (fragment).**
old:
```
The name and signature are chosen so that the promotion, when it lands, turns `Deserialize` into a real interface method with no source change.
```
new:
```
The name and signature are chosen so that the promotion, when it lands, turns `Deserialize` into a real interface method with no change to the member; the conformance declaration itself becomes `: IDeserializable<T>` (proposal 0125 Part B).
```

**R5 — §5.16, `:687` (fragment; N6).**
old:
```
v0.111 opens sequence-literal construction to user-defined types via a named static factory, `FromCollectionLiteral`
```
new:
```
v0.111 opens sequence-literal construction to user-defined types via a static factory on the type, `FromCollectionLiteral` — recognized by name at first, and since the promotion the member of the stdlib §2.19 target pair
```

**R6 — §5.16, `:689`, the paragraph heading (fragment; N6).**
old:
```
*Mechanism: named static factory, not an attribute.*
```
new:
```
*Mechanism: a static factory on the type, not an attribute.*
```

**R7 — §5.16, `:689`, the two body clauses that still assert the retired mechanism (fragment; P2-C3).**
old:
```
Bezel has no general attribute system, so the natural "explicit opt-in" equivalent is *declaring a specifically-named static method*. And a Bezel static method on a generic type may freely reference the type's own parameters — the recognized factory signature does exactly that (`public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned C<T>`, spec §15.4.1) — so the factory goes *directly on the collection type* with no builder-type indirection.
```
new:
```
Bezel has no general attribute system, so the natural "explicit opt-in" equivalent was, before the promotion, *declaring a specifically-named static method*, and is now *declaring a conformance whose one static witness is that method*. And a Bezel static method on a generic type may freely reference the type's own parameters — the witness signature does exactly that (`public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned C<T>`, spec §15.4.1) — so the factory goes *directly on the collection type* with no builder-type indirection.
```

**R4b — §5.19, `:752`, the dangling cross-reference (fragment; P2-C1).**
old:
```
established for `FromCollectionLiteral` in [§5.16](#sec-collection-literals-for-complex-collections) and language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol)
```
new:
```
established for `FromCollectionLiteral` in [§5.16](#sec-collection-literals-for-complex-collections) and language spec [§15.4.1](Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol) before that protocol's promotion to the stdlib §2.19 target pair
```

### Code-generation reference

**G1 — `Bezel_For_Code_Generation.md:765`, the §5-index line (fragment; B8) — and the pin at `:3` moves to the landing revision in the same commit.**
old:
```
**Static-interface abstractions:** `INumber<T>` (`static Zero()`, `static One()`, operators `+ - * /`, `<`), `ISteppable<T>`
```
new:
```
**Static-interface abstractions:** `ICollectionLiteralTarget<TSelf, T>` (`static FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned TSelf`) and `IOwningCollectionLiteralTarget<TSelf, T>` (`static FromCollectionLiteral(owned elements: OwningArray<T>): owned TSelf`) — the sequence-literal target pair (spec §15.4.1): a user **class** declares exactly one, at `TSelf` = itself (`class Ring<T>: ICollectionLiteralTarget<Ring<T>, T>`), and the built-ins declare it too — `Array/LinkedList/Set/Stack/Queue<T>` the borrowing one, the five `Owning*<T>` the owning one — so each lists a callable `FromCollectionLiteral`; `INumber<T>` (`static Zero()`, `static One()`, operators `+ - * /`, `<`), `ISteppable<T>`
```

### `DECLINED.md`

**D1 — `:75`, the dual-shape row (carrier correction + re-cite; C9).**
old:
```
| Declaring both collection-literal factory shapes on one type | `rejected` | rationale §5.16 (*Ownership: two factory shapes*); spec §15.4.1 | —(none stated). Exactly one shape per type — borrowing (`ReadOnlySpan<T>`) or owning (`owned OwningArray<T>`): "declaring both is rejected because the literal's element-expression ownership would not unambiguously pick a target." |
```
new:
```
| Declaring both collection-literal factory shapes on one type | `rejected` | rationale §5.16 (*Ownership: two factory shapes*); spec §15.4.1 (own-declared conformance to both target interfaces) | —(none stated). Exactly one shape per type — borrowing (`ReadOnlyArraySpan<T>`, `ICollectionLiteralTarget`) or owning (`owned OwningArray<T>`, `IOwningCollectionLiteralTarget`): "declaring both is rejected because the literal's element-expression ownership would not unambiguously pick a target." |
```

**D2 — `:30`, the `ToArray()` row, amended in place (P2-B1; the row is kept, its bar recorded as met for the value at the witness spelling).**
old:
```
| A `ToArray()` on `ReadOnlyArraySpan<T>`, riding the v0.272 collection-literal carrier fix | `rejected` | [proposal 0115 §7 R3](0115-collection-literal-borrowing-span-family.md); changelog v0.272 | A stdlib pass that takes it on its own merits.
```
new:
```
| A `ToArray()` on `ReadOnlyArraySpan<T>`, riding the v0.272 collection-literal carrier fix | `rejected` (the `ToArray()` spelling; **the value arrived at v0.294 at the witness spelling** — `Array<T>.FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Array<T>`, the static witness spec §11.8 requires of a declared `ICollectionLiteralTarget<Array<T>, T>` conformer, structural rather than *while-we're-at-it*; D-0009, [proposal 0125 §2.3](0125-static-interface-promotions.md)) | [proposal 0115 §7 R3](0115-collection-literal-borrowing-span-family.md); changelog v0.272; changelog v0.294 | A stdlib pass that takes it on its own merits — for the `ToArray()` spelling, which stays declined as a dual of the witness.
```

**Also at graduation (not `old→new` edits):** the changelog entry (Appendix C) with the Migration note (§2.11) naming the two editorial repairs (spec `:3056`, `DECLINED.md:75`) and the `:30` reopening; the new `DECLINED.md` rows routed per §5 and the D-0009 register entry; the code-generation reference pin at `:3`; the compiler tracking issue with §2.12's content (including the `_3032` re-authoring and the three other files); comments closing #1506 and pointing #1507 at Part B; corpus rows per §2.9 re-transcribed by hand (#1380); the README row moved to `## Graduated` (N5).

---

## Appendix B — README Live-queue row (pre-graduation)

```
| [0125](0125-static-interface-promotions.md) | [The two §11.8 promotions](0125-static-interface-promotions.md) — Part A (#1506): `FromCollectionLiteral` → the static-interface pair `ICollectionLiteralTarget<TSelf, T>` / `IOwningCollectionLiteralTarget<TSelf, T>` (stdlib §2.19), conformance declared explicitly by user classes (ruling B — the `:2079` zero-source-change claim covers the member, not the header) and nominally by the ten built-in sequence collections in their own stdlib headers with the factory listed (10 declarations, 11 header lines, 11 members); dictionary form outside; D4 re-keyed as three predicate substitutions; class-only, type-parameter targets, and a declaration-site migration diagnostic stated; in-tree break 17 sites / 4 test files + 1 corpus case, zero external (`_3032` re-authored); Part B (#1507): `IDeserializable<TOut>` with `static Deserialize(d): owned TOut`, no recurrence (the `owned Shared<C>` entry is `TOut = Shared<C>`), a `TOut` admissibility clause, synthesis supplies the witness — deferred on a 621-occurrence breaking header change with no ramp rule, the §9.8 synthesis-generator obligation, and the unverified admissibility of `IDeserializable<Shared<Node>>` as a header | under-review — draft (2026-09-05), revised after two adversarial critique passes (8B/11C/8N and 5B/11C/5N resolved; pass 1's C1 reversed); Part A recommended to graduate after #3857 (v0.292/v0.293 landed; A1/R2 re-anchored), Part B recommended deferred with a three-item bar | owner: §6 Q1–Q4 (sequencing behind #3857; confirm the nominal-declaration mechanism; accept Part B's shape and bar; hard break vs arity ramp); then one more critique pass over §2.6/§2.8 | [#1506](https://github.com/s8lap/bezel-lang/issues/1506), [#1507](https://github.com/s8lap/bezel-lang/issues/1507), [#3857](https://github.com/s8lap/bezel-lang/issues/3857) |
```

**Graduation-form `**Status**` cell (P2-C10; unadorned token, space + EM DASH + space, the 0077 precedent):**

```
| **Status** | graduated-v0.294 — superseded — **Part A (#1506) graduated v0.294** (owner-accepted 2026-09-05): `FromCollectionLiteral` promoted to the static-interface pair `ICollectionLiteralTarget<TSelf, T>` / `IOwningCollectionLiteralTarget<TSelf, T>` (stdlib §2.19), conformance declared explicitly by user classes and nominally by the ten built-in sequence collections with the factory listed; the §15.4 list derived from those declarations; D4 re-keyed; class-only, type-parameter and interface rulings stated; the declaration-site migration diagnostic; `DECLINED.md:30` amended (D-0009). **Part B (#1507) deferred**, not graduated — the design in §3 is the recorded shape and its three-item reopening bar is §3.11; the deferral is registered in `DECLINED.md`. *(prior: `under-review` — draft 2026-09-05, revised the same day after two adversarial critique passes.)* |
```

**`## Graduated` README row (P2-C10; replaces the Live-queue row at landing; SECTION/BIJECTION re-run):**

```
| 0125 | [The two §11.8 promotions](0125-static-interface-promotions.md) — Part A (#1506) graduated v0.294: `FromCollectionLiteral` became the static-interface pair `ICollectionLiteralTarget<TSelf, T>` / `IOwningCollectionLiteralTarget<TSelf, T>` (stdlib §2.19), conformance declared explicitly (user classes) and nominally (the ten built-in sequence collections, factory listed — `DECLINED.md:30` amended, D-0009); §15.4's list derived from declarations; D4 re-keyed; class-only, type-parameter and interface rulings; the declaration-site migration diagnostic; in-tree break 17 sites / 4 test files + 1 corpus case, `_3032` re-authored. Part B (#1507) deferred on a 621-occurrence break with no ramp rule, the §9.8 generator obligation, and the unverified `IDeserializable<Shared<Node>>` header (`DECLINED.md`, three-item bar) | graduated-v0.294 — superseded (Part A); Part B deferred | compiler catch-up #3866 after #3857 | [#1506](https://github.com/s8lap/bezel-lang/issues/1506), [#1507](https://github.com/s8lap/bezel-lang/issues/1507), [#3857](https://github.com/s8lap/bezel-lang/issues/3857) |
```

---

## Appendix C — the v0.294 changelog entry (P2-B5; adopted from the pass-2 critique's Appendix F and edited for the pass-2 corrections)

Insert at EOF of `docs/Bezel_Changelog.md`, preceded by a blank line, `---`, blank line. The two `#3866`/`#3867` placeholders are the issues filed at landing (the Part A catch-up; the `owned Shared<C>` entry-shape impl-lag). Reviewed against the v0.286/v0.293 shape.

---

### v0.294 changelog (v0.293 → v0.294)

**Proposal [0125](proposals/0125-static-interface-promotions.md) Part A graduates (design change; owner-accepted 2026-09-05): the user-defined sequence-literal protocol becomes interface-based, [#1506](https://github.com/s8lap/bezel-lang/issues/1506).** §15.4.1's by-name opt-in — *"declaring a static method named exactly `FromCollectionLiteral` with one of two recognized signatures"* — is replaced by conformance (§12.7) to one of two static-member interfaces (§11.8), `ICollectionLiteralTarget<TSelf, T>` (borrowing form, `ReadOnlyArraySpan<T>` carrier) and `IOwningCollectionLiteralTarget<TSelf, T>` (owning form, `owned OwningArray<T>` carrier), defined in stdlib §2.19. A class is a **sequence-literal target** exactly when it conforms at `TSelf` = itself; a type parameter constrained to conform is one inside its declaring body; an interface never is. The member's name and signature are unchanged — the promotion costs each conformer one clause in its class header. The ten built-in sequence collections declare the same conformance in their own standard-library declarations (stdlib §4.4–§4.11) and each lists `FromCollectionLiteral` as a member, so §15.4's target list is now *derived* from declarations (its bullets restate them) rather than curated; at a built-in target the literal is still constructed directly under §15.4's element-order rules — §15.4.1's materialize-then-call *Lowering* governs the other targets, and a built-in's listed factory produces the same value the literal produces. Bezel therefore has **two** recognition categories, not three: interface-based (iteration, §29.1; collection literals, §15.4.1) and the single structural-dispatch rule for `Contains`/`in` (§17.5).

**Why explicit conformance, and not an implicit one inferred from the factory:** an implicit rule would be a second structural-dispatch rule beside `Contains` — the thing §17.5 and rationale §5.16 (*One opt-in path, not two*) both guard by name, and which `DECLINED.md`'s duck-typed-enrollment row already declines — and it would buy nothing: the compiler would still look up a name, and `TSelf.FromCollectionLiteral(…)` would stay unreachable through a constraint because nothing would declare the constraint satisfied. §12.7 states conformance nominally in every clause but the `IDisposable` carve-out, so an inferred conformance would be a new kind of conformance, not a use of the existing one. §11.8's *"both signatures were chosen for zero-source-change promotion"* is corrected on the same ground: the promise holds of the **member**, never of the **header**.

**Why the built-ins declare nominally, and not by a compiler-provided conformance:** §12.7 clause 5's compiler-provided device is scoped to primitives, so a class cannot use it without a new implicit case this revision declines to add; and a member callable on a type's own name that appears in no standard-library listing is exactly the invented-API class #1424 removed once (`int.Zero()`). Declaring the conformance and listing the factory keeps both rules: `Array<int>.FromCollectionLiteral(span)` is a **declared** member.

**A recorded decline reopened, with its evidence stated:** `DECLINED.md`'s row *"A `ToArray()` on `ReadOnlyArraySpan<T>`, riding the v0.272 collection-literal carrier fix"* (`rejected`, v0.272; bar *"a stdlib pass that takes it on its own merits"*) declined the value `ReadOnlyArraySpan<T>` → `owned Array<T>` on *"surface is debt"* and *while-we're-at-it* grounds. This revision supplies that value as `Array<T>.FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Array<T>`, and nine siblings. The new evidence against the stated reasons is structural: a nominal conformance to a static-interface member **requires a declared witness** — §11.8: *"A conforming type declares `class Meters: INumber<Meters>` and supplies the static members with exact signatures (the §11.2 matching rule, applied to statics)"*; *"a runtime type conforms only by declaring the full member set, statics included"* — and neither §11.8 nor §12.7 has a witness-less form. The members are the unavoidable cost of the promotion the owner directed (#3247, #1506), not a convenience riding a fix; the alternative (built-ins conforming with no callable member) was priced and declined as a new conformance kind. The row is amended in place — its `ToArray()` spelling stays declined as a dual — and the reasoning is registered as `DECLINED.md` D-0009. The ten members are recorded on the revision's **cost** side as well as its gain side.

**Adjacency kept, not amended:** the dictionary-literal form (`[k: v]`) stays outside the protocol and both dictionaries conform to neither interface; inline arrays are unchanged (§15.1, not nominal types); `SortedDictionary<K, V>`'s dictionary-form admission remains the impl-ahead delta tracked at [#3831](https://github.com/s8lap/bezel-lang/issues/3831); §15.4's `:2960` owning-target sentence takes no edit (its enumerated list serves the dictionary value-position parenthetical); §14.2's conditionally-admitted-member register, §3.6's derived-type-property table and the statics-and-sharing fact sets have no contact — *sequence-literal target* is declaration-derived, not structure-derived, and adds no §3.6 row.

**Migration.**

- **Newly required: a conformance clause on every user conformer.** A class that keeps a bare `FromCollectionLiteral` and declares neither interface is no longer a sequence-literal target. In-tree: **17 declaration sites in 4 test files** — `tests/sema/collection_literal_factory_conjuncts_3032_test.cpp` (10), `tests/parser/param_post_colon_owned_3027_test.cpp` (3), `tests/codegen/collection_literal_user_type_fcl_2957_test.cpp` (2), `tests/sema/collection_literal_dict_form_target_3031_test.cpp` (2) — plus one corpus case (`tests/conformance/cases/spec_0075.bzl`). **External users: none.** No ramp is owed: this is a design change, not a Ratchet (process §2 — nothing scheduled the tightening), so there are no two ends to state; what is owed is the diagnostic below.
- **Newly defined:** the declaration-site diagnostic — a class declaring a `public static FromCollectionLiteral` matching either interface member at `TSelf` = the class while conforming to neither is a compile error naming the missing conformance and its remedy (*add `: ICollectionLiteralTarget<C, T>` / `: IOwningCollectionLiteralTarget<C, T>` to the class header*); `TSelf.FromCollectionLiteral(…)` through a constraint; a constrained type parameter as a literal target; the ten built-in factories callable directly by name (stdlib §4.4–§4.11); `is`/`as` against either interface `true` for those ten types (§11.8's static-member interfaces are ordinary types) — a new runtime-observable fact.
- **Newly rejected:** conformance to either interface by a value kind (record, choice, enum) — the erasure of §11.8 would otherwise admit one silently; a class declaring both conformances on itself, or one of them at two element types; an interface type as a target (unchanged in effect — the compiler already declined it, `isCollectionLiteralInterfaceTarget`).
- **Newly accepted (the retired conjunct diagnostics):** a `FromCollectionLiteral` that is non-`public`, non-`static`, inherited, extension-supplied, or of a non-matching return type, on a class declaring no conformance, is no longer rejected at its declaration — it is an ordinary member of a type that is simply not a target, and the three §15.4.1 conjunct diagnostics that used to name it are retired into §11.2 conformance matching (which reaches it only once a conformance is declared) plus the declaration-site diagnostic above.
- **Diagnostic couplings (the reason the test track moves with the compiler, not with this revision):** `tests/sema/collection_literal_factory_conjuncts_3032_test.cpp` pins the three retired §15.4.1 sentences verbatim (*"must be `public`"*, *"must return `owned C<T>`"*, *"exactly one may be declared"*) and states that its predicates are selective on that wording; it is **re-authored**, not clause-patched, against §11.2 conformance diagnostics and the new declaration-site diagnostic, and its cells have successors only once each declares the conformance (its non-`public`-without-conformance cell has none — that program is newly accepted). The other three files take one header clause per site.
- **Spec-ahead-of-impl.** Prerequisite [#3857](https://github.com/s8lap/bezel-lang/issues/3857) (the compiler never received v0.272's `ReadOnlySpan<T>` → `ReadOnlyArraySpan<T>` carrier rename: `src/sema/TypeResolution_Checks.cpp:1625`, `src/codegen/LlvmBackend_Setup.cpp:31450`) lands first. The promotion's own catch-up is filed as [#3866](https://github.com/s8lap/bezel-lang/issues/3866): synthesize the conformance and the listed member on the ten built-in `TypeSymbol`s (today *"null `decl` and empty members"*); re-key `isCollectionLiteralAdmissibleTarget` and the empty-literal predicate (`src/sema/ExprTypecheck_Calls.cpp:503-530`, `:573-592`) to a conformance query at `TSelf` = target, class-only, plus the type-parameter case; retire `checkCollectionLiteralFactoryConjuncts` (`src/sema/TypeResolution_Checks.cpp:1576-1751`) into §11.2 matching while keeping the value-kind and own-declared-dual rejections and adding the declaration-site diagnostic; keep the built-ins' direct intrinsic construction at a literal and add a callable static body for their listed factory; re-author `_3032` and clause the other three files; add spec-cited tests for the non-target conformance at `TSelf ≠ C`, the derived class declaring its own, the interface non-target, the type-parameter target, and the migration diagnostic.
- **Compiler claims are source reads** — no build exists in the authoring environment; every `src/` and `tests/` citation above was read, not run.
- **Kernel classification: derivation** — no §28/§30.5 rule moves. §15.4's D4 borrowing-family and nested-fresh-producer clauses are re-keyed from *"a user-defined type whose `FromCollectionLiteral` declares the borrowing form"* to *"every borrowing-form target"* / *"any sequence-literal target"*, the same set under the new predicate; v0.272's derivation (K1/K2.3/K4.3 through §28.11's borrow column) stands unchanged, no kill is added, the consume table and the axiom register are untouched. **Body-locality: local** — a declaration-site check on the conformer plus a use-site predicate over the target's declared conformances (for a type parameter, over the constraints in scope in the same body); no cross-body fact is consumed.
- **Doc-only:** 13 spec edits (§11.8's follow-up sentence; §15.4's target bullet, sequence-form sentence and two D4 clauses; §15.4.1's opt-in paragraph, both form headings, the conjunct paragraph, *Lowering* with its built-in carve, *Recognition category* and the `RingBuffer` example header; §17.5's taxonomy sentence), 20 stdlib edits (§2.19 heading, opening, producer markers and the new interface pair; §4.3's roster sentence; ten conformance declarations over eleven §4.4–§4.11 header lines with eleven members — ten listed signatures and `OwningArray<T>`'s reference-implementation body; §2.14 `:732`, `:747`, `:769`), 8 rationale edits (§5.16 four, §5.19 two, §18 two), one code-generation-reference §5-index line **and its pin (→ v0.294** — required by process §9 because the stdlib surface changed; the pin was one revision behind, inside the gate's allowance), five new `DECLINED.md` index rows, two corrected rows (the dual-shape row's carrier; the `ToArray()` row's amendment) and one register entry (D-0009). Guides: no contact (none mentions the protocol).
- **Editorial repairs carried, not dropped silently:** §15.4.1's conjunct paragraph still named the retired `ReadOnlySpan<T>` as an element-type carrier (v0.272 replaced it), and `DECLINED.md`'s dual-shape row carried the same stale carrier; both now read `ReadOnlyArraySpan<T>`. Stdlib `:747` and `:769` called a `Self` type *"separately deferred"* while `DECLINED.md` records it `rejected` with no reopening bar; both sentences now name the decline. Stdlib §2.14 `:732` taught that `FromCollectionLiteral` is recognized by name; it now says so in the past tense, dated.
- **Corpus (#1380):** no case is deleted. `spec_0075` (the `RingBuffer` example) gains the header clause and is re-transcribed by hand; `spec_0073`/`spec_0074` are unchanged (the two form code blocks stay as `C<T>`-spelled witness listings — only their prose headings move); the §2.19 interface pair adds one new `stdlib_NNNN` row, `skip:signature-listing`; the ten §4 listing rows and `OwningArray<T>`'s reference-implementation row are re-transcribed. Every row keeps its `skip` status until the compiler catch-up lands.
- **Part B ([#1507](https://github.com/s8lap/bezel-lang/issues/1507)) is deferred, not landed.** `IDeserializable` stays the marker interface it is. The recorded design — `IDeserializable<TOut>` with `static Deserialize(d: IDeserializer): owned TOut`, no recurrence, and an admissibility clause on `TOut` (`C` or `Shared<C>` for a class, `V` for a value kind) — fits all three return shapes §2.14 and §9.8 state, the shared-node entry `owned Shared<C>` (§9.8, *Synthesized `Deserialize`*) included, as `TOut = Shared<C>`. It sits in proposal 0125 §3 behind a three-item bar that is about cost and two unverified obligations, not about any gap in the spec: an owner decision on the 621-occurrence breaking header change across 90 files (one hard revision, or a ramp — and a ramp needs a ruling that a bare and a generic `IDeserializable` may coexist by arity); the §9.8 synthesis generator learning to emit `Deserialize` as an interface-member witness; and `class Node: IDeserializable<Shared<Node>>` verified admissible as a conformance header under §11.2/§11.8/§12.7 as written, with the `TOut` clause ruled. Separately, the `owned Shared<C>` entry shape is spec-ahead-of-impl (sema types every `Deserialize` as returning the declaring type; codegen mints no `Shared` entry) and is filed as [#3867](https://github.com/s8lap/bezel-lang/issues/3867) — a tracking item, not a deferral ground. §11.8's follow-up sentence and stdlib §2.14 now say exactly that.
- **Issues this revision resolves:** [#1506](https://github.com/s8lap/bezel-lang/issues/1506) closes with the citation (spec §15.4.1, stdlib §2.19, v0.294); [#1507](https://github.com/s8lap/bezel-lang/issues/1507) is re-pointed at proposal 0125 Part B and its bar. **Tracking issues:** [#3857](https://github.com/s8lap/bezel-lang/issues/3857) (prerequisite, open); [#3866](https://github.com/s8lap/bezel-lang/issues/3866) (this revision's catch-up); [#3867](https://github.com/s8lap/bezel-lang/issues/3867) (the `owned Shared<C>` entry shape); [#3831](https://github.com/s8lap/bezel-lang/issues/3831) (the `SortedDictionary` dictionary-form delta, cited not re-filed).
- **Gate round (adversarial critique, two passes, 2026-09-05): LAND-WITH-CORRECTIONS, twice.** Pass 1 — 8 blocking, 11 corrections, 8 notes. What bit: the built-ins conformed to nothing under §12.7's own definition, and the edit-set never touched it (fixed by nominal declaration per type, not by a new implicit case); the direct built-in call would have re-created the #1424 invented API (fixed by listing the factory in all ten sections); the in-tree break was claimed as three declaration sites and measured as seventeen, ten of them pinning diagnostics this revision deletes; an interface conformer was an admitted-but-unconstructible target; the uniqueness rule over-rejected a derived class that declares its own conformance; two form blocks and the *Lowering* sentence kept name-keyed text no edit reached; the migration had no declaration-site signal — the exact silent non-opt-in #3857 documents as a bug; and the code-generation pin move was called optional. Pass 2 — 5 blocking, 11 corrections, 5 notes; it confirmed each pass-1 resolution against the tree and ran both gates green on the applied set, and it bit five more times: pass 1's own C1 had injected a **false absence** about §9.8's `owned Shared<C>` shape into the record (the shape is at `:1546` in both trees; reverted, and Part B re-grounded on cost and two unverified obligations); the ten declared factories reopened `DECLINED.md`'s `ToArray()` row without naming it (named, evidenced, amended, registered as D-0009); the re-keyed §15.4.1 *Lowering* governed `Array<T>` circularly (carved); stdlib §2.14 still taught by-name recognition (retired, dated); and this changelog entry itself was missing.
