# #703 spec-edits draft (E1–E7) — type-driven serialization migration

## ✅ LANDED v0.141 (commit 3776948, pushed). Phases 0+1 DONE: rationale §5.22 + §5.19 para + §18 status; spec §7.9/§9.8/grammar/AppC; stdlib §2.14 return+tutorial+citation; changelog + version bumps. All 3 docs grep-clean. #717 filed (stale §28.3 sentence). NEXT: Phase 2 test migration (~206 lines/18 files), then Phase 3 compiler S1-S9.

NOT yet landed. To be adversarially reviewed (like §5.22), then applied to the live docs against exact
current text, with a changelog entry + version bump, as ONE revision together with rationale §5.22 + the
§5.19 closing paragraph. Authority for the model: serialization-703-rationale-5.22-DRAFT.md (verified).

**The model (from §5.22), as the spec must state it:**
- `owned`/value field → an **owning edge**, serialized **inline**.
- `Shared<T>` field → an **identity participant**: written once at its first occurrence (identity tag keyed
  on the underlying value `Get()` borrows, i.e. `Object.ReferenceEquals` identity), a `$ref` at every alias;
  reconstructed by `Clone()`ing off the identity table.
- `Weak<T>` field → **always a back-reference** (`$ref`); reconstructed by `Downgrade()` from the
  reconstructed `Shared<T>`. Nullable (`Weak<T>?`) is the cycle-closing idiom.
- plain borrowing field → a back-reference (`$ref`), reconstructed as a constructor argument — **unchanged**.
- A class enters synthesis by **declaring `ISerializable`/`IDeserializable`** with eligible components; the
  serialized shape is **read from the field types**, not asserted by a header clause. The
  `serialized tree`/`serialized graph`/`serialized cyclic` clauses are **removed**.
- Weak-reachability is enforced **dynamically** (`SerializationException` for a `Weak` target never strongly
  tagged); a cheap static lint catches the trivial intra-class case (necessary, not sufficient).
- `polymorphic` registry: **rehomed** to a class-body member declaration; mechanism/security unchanged.
- Co-owned `Deserialize` return: `owned Shared<C>` when the entry class **is** the shared node; `owned C`
  when it merely **holds** `Shared`/`Weak` fields (locally decidable).

---

## E1 + E2a — §9.8 eligibility (spec :997)

**OLD** (the synthesized-class clause inside the eligibility list):
> a serialization-eligible **synthesized class** (a class with a `serialized tree`, `serialized graph`, or `serialized cyclic` clause, below)

**NEW:**
> a serialization-eligible **synthesized class** (a class that declares `ISerializable`/`IDeserializable` and whose serialized components are each serialization-eligible, below); a `Shared<T>` or `Weak<T>` whose `T` is a serialization-eligible class

*(E1 admits Shared/Weak recursively; E2a replaces the keyword-clause class criterion with a
declaration-based one. The rest of the eligibility sentence — primitives, string, enum, record, nullable,
collections, dictionary — is unchanged.)*

---

## E2b — §9.8 graph-shape clause definitions (spec :1006–1009) — REMOVE + REPLACE

**OLD** (:1006–1009, the `serialized tree` / `serialized graph` definitions and the "either clause" eligibility paragraph):
> - **`serialized tree`** asserts the class's serialized object graph is a **tree**: along the serialized edges, no two references alias the same object, and there are no cycles.
> - **`serialized graph`** asserts the serialized object graph is a **directed acyclic graph**: serialized edges *may* alias a shared object (the same object reachable by more than one path), but there are no cycles.
>
> A class with either clause is serialization-eligible when every serialized component is itself serialization-eligible by the rule above (which recursively admits other `serialized tree`/`serialized graph` classes and owned collections of them). The two clauses differ only in how aliasing is treated; cycles are rejected by both. (`sealed` is irrelevant to serialization: a sealed class follows the same eligibility rules and wire form as any other class, clarified in v0.136.)

**NEW:**
> A class enters serialization synthesis by **declaring `ISerializable` and/or `IDeserializable`**; it needs no shape clause, because the **serialized shape is read from the field types** — exactly as the owning-tree/back-edge partition has always been read from the `owned`/borrow annotations the language already requires. A field's type states its serialized role: an `owned` (or value) field is an **owning edge**, written inline; a `Shared<T>` field is an **identity participant**, written once and referenced by identity at each alias; a `Weak<T>` field, and a plain borrowing field, are **back-references**. The graph the class serializes is a tree, a directed acyclic graph, or a cyclic graph as a consequence of those types — a class with no back-references serializes as a tree; one with borrowing or `Shared<T>` fields may alias; one with `Weak<T>` fields may cycle — and the deserializer reconstructs whichever shape the field types and the wire identity tags describe, with no header assertion to declare or to police. (`sealed` is irrelevant to serialization: a sealed class follows the same eligibility rules and wire form as any other class.)

---

## E2c — §9.8 owning-tree/back-edge partition + synthesized Serialize/Deserialize (spec :1013–1017) — REPLACE the component classification

**OLD** (:1013, the serialized-components partition):
> The serialized components of a class are its `owned` and value-typed fields and auto-properties together with its **borrowing** reference fields (non-owning class-typed fields whose target is owned elsewhere in the same serialized graph), in declaration order. The owning (`owned`) and value edges form the **owning spanning tree**; the borrowing fields are **back-edges** into that tree. This owning-tree/back-edge partition is read directly from the `owned`/borrow annotations the language already requires — it is not inferred and not guessed.

**NEW:**
> The serialized components of a class are its `owned` and value-typed fields and auto-properties, its `Shared<T>` and `Weak<T>` fields, and its plain **borrowing** reference fields, in declaration order. The `owned`/value edges form the **owning spanning tree**; `Shared<T>` fields are **identity participants** (an owned share of a value reachable elsewhere by another handle); `Weak<T>` fields and plain borrowing fields are **back-edges** into the graph. This partition is read directly from the field types the language already requires — it is not inferred and not guessed.

**OLD** (:1015, synthesized Serialize — the back-edge handling):
> **Synthesized `Serialize`.** The serializer assigns each owned object an identity tag the first time it is reached through an owning edge, and writes the object's components there. A borrowing field is *not* written inline; it is written as a reference to the tag of the object it borrows (which is necessarily present in the owning tree, because a borrow's lifetime is bounded by an owner that outlives it — the language already guarantees the owner exists within the graph). Under `serialized tree`, the serializer additionally asserts no tag is ever reached twice through owning edges and no reference targets an already-emitted-as-reference object; a violation is the programmer's contract breach, surfaced at read (below). Under `serialized graph`, a shared object is written once at its owning-tree position and referenced by tag elsewhere.

**NEW:**
> **Synthesized `Serialize`.** The serializer assigns an identity tag the first time it reaches an object through an owning edge, and writes the object's components there. A `Shared<T>` field participates by identity: the tag is keyed on the **underlying value** the handle's `Get()` borrows (object-reference identity, [§8](#sec-the-object-class)) — so two independently `Clone()`d handles to one value collapse to one tag — written once at first occurrence and as a `$ref` at every later occurrence. A `Weak<T>` field and a plain borrowing field are never written inline; each is written as a `$ref` to the tag of the object it references (for a plain borrow the target is necessarily present in the owning tree; for a `Weak<T>` the target must be strongly reached elsewhere in the graph, enforced at read, below). No header assertion is made about aliasing or cycles: the field types fix which components alias and which are back-references, and the identity-tag mechanism records the actual sharing in the data.

**OLD** (:1017, synthesized Deserialize):
> **Synthesized `Deserialize`.** The synthesized factory has the class return shape of [§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary) — `Deserialize(d: IDeserializer): owned T` (a class instance is an owned heap value transferred to the caller, [§28.8](#sec-ownership-transfer)). It reconstructs the owning spanning tree first, in **owner-before-borrower order**: each owned object is constructed by an ordinary call to its constructor, exactly as in the tree case, and every borrowing field is supplied **as a constructor argument** — the reference is resolved against the tag→object table, which already contains the target because the owning tree is built in an order where every borrow target precedes every borrower. A directed acyclic graph always admits such an order; this is precisely the property `serialized graph` asserts. Because each borrowing field is bound *through the constructor*, before any constructor body runs, there is no program point at which a constructed object has an unbound non-nullable reference field: [§7.9](#sec-definite-assignment-for-non-nullable-fields) is satisfied unmodified, with no nullable-field relaxation and no observable intermediate state. The ownership contract for the owning edges lives at the constructor signature, a static program point, exactly as for records and tree-shaped classes; the borrowing arguments are ordinary borrows whose lifetime is established by the constructed owning tree.

**NEW:**
> **Synthesized `Deserialize`.** The synthesized factory reconstructs the owning spanning tree first, in **owner-before-borrower order**: each owned object is constructed by an ordinary call to its constructor. A `Shared<T>` component is reconstructed at its first (tagged) occurrence and registered in the tag→object table; each later `$ref` to that tag is bound by `Clone()`ing the registered handle (identity preserved, the shared count incremented). A plain borrowing field is supplied **as a constructor argument** resolved against the tag→object table, exactly as before — a directed acyclic graph admits an owner-before-borrower order, so the target precedes the borrower and [§7.9](#sec-definite-assignment-for-non-nullable-fields) is satisfied **unmodified**, with no observable intermediate state. A `Weak<T>` `$ref` is bound by `Downgrade()` from the reconstructed `Shared<T>` its tag resolves to; because a `Weak<T>` field is **nullable** in the cycle-closing idiom (its target may be released), it is assigned after construction as ordinary mutation, so a cycle needs no two-phase window and [§7.9](#sec-definite-assignment-for-non-nullable-fields) is again satisfied — the null initializer discharges it, the later wiring is ordinary assignment. The return shape is [§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary)'s: `owned C` for a class that holds shared/weak components, and `owned Shared<C>` when the class *is* a shared node (the co-owned entry); the choice is locally decidable from the entry type ([§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary)).

---

## E2d — §9.8 type-scoped justification + exhaustiveness note (spec :1026, :1028) — REMOVE/REPLACE

**OLD** (:1026): the "`serialized tree` and `serialized graph` are **type-scoped** because graph shape is an intrinsic property…" paragraph. **NEW:** delete it — there is no clause to scope; the field types are intrinsic to the type by construction. (Retain the contrast with the operation-scoped unknown-field policy by relocating, if needed, a one-clause reminder: "the deserializer's unknown-field policy remains operation-scoped, stdlib [§2.15](Bezel_Standard_Library.md#sec-concrete-serialization-formats).")

**OLD** (:1028): "**The graph-shape clauses are exhaustive as of v0.124.** Every class object graph is a tree, a directed acyclic graph, or a general (possibly cyclic) graph; `serialized tree`, `serialized graph`, and `serialized cyclic` cover all three. …" **NEW:** delete; replaced by the field-type model (E2b). The honest forward-pointer lives in rationale §5.19's closing paragraph + §5.22.

---

## E2e — §9.8 read-side enforcement (spec :1023–1024) — REPLACE keyword-shape detection with identity + Weak-reachability

**OLD** (:1023–1024, the under-`serialized tree`/`serialized graph` read-side rules):
> - Under `serialized tree`, any repeated object identity in the input … raises `SerializationException` …
> - Under `serialized graph`, a reference whose target has already been constructed is bound to that object (identity preserved …). A **cycle** … raises `SerializationException`. …

**NEW:**
> - **Identity is preserved, never duplicated.** A `$ref` whose target has already been reconstructed binds to that object: a `Shared<T>` alias `Clone()`s the registered handle, a `Weak<T>` reference `Downgrade()`s it, a plain borrow takes the resolved reference. The reference-unaware-serializer failure mode (distinct objects where the input shared one) cannot occur.
> - **Weak-reachability is checked at read.** A `Weak<T>` `$ref` whose target is never strongly reached anywhere in the graph (no `Shared<T>` occurrence ever registered that tag) raises `SerializationException` — the same assert-and-enforce discipline graph shape used, now for the one property the field types cannot decide (the strong owner of a `Weak` is a per-value, not per-type, fact).

---

## E2f / P6 — §9.8 cyclic synthesis (spec :1028–1053) — REMOVE the keyword machinery

Remove `serialized cyclic`, the **two-phase construction** (:1046–1053), and the **self-contained-constructor eligibility rule** (:1035–1044) in their entirety. They are retired, not relocated: a cyclic graph is now expressed with `Weak<T>` back-edges on a `Shared<T>` spine, constructed in ordinary ownership-valid Bezel (the `Weak<T>?` field is nullable and wired after the spine exists), and reconstructed by the ordinary identity/`$ref` resolve (a `Weak` `$ref` `Downgrade()`s the already-registered `Shared<T>` — no forward reference into an in-progress group, no mutable post-construction window to prove unobservable). The definite-assignment guarantee ([§7.9](#sec-definite-assignment-for-non-nullable-fields)) is unaffected: cycle-closing `Weak<T>?` fields are nullable, so there is no non-nullable back-edge to bind in an unobservable interval. (Rationale: §5.22; the §7.9 "one compiler-controlled construction mode" sentence at spec :655 is updated by E5 below to drop the two-phase reference.)

---

## E2g / P6 — §9.8 polymorphic dispatch (spec :1055–1076) — REHOME the registry

Keep the mechanism unchanged: discriminator written first (`$type`), read first and dispatched through a **closed** registry, an unregistered discriminator/run-time-type raising `SerializationException`; no reflective subtype discovery (the explicit-registration-never-reflection principle, [§5.18](Bezel_Design_Rationale.md#sec-type-identity-not-reflection), and the reflective-`$type`-RCE-vector reason recorded in rationale §5.19). Two coupled changes:

**OLD** (:1058): "declares a `polymorphic` registry **as part of its graph-shape clause**:" with the header example `serialized graph polymorphic ("circle": Circle, "square": Square)`.
**NEW:** the registry is a **class-body member declaration**:
> A base class whose serialized components may, at run time, hold instances of registered derived classes declares a `polymorphic` registry as a member of its body:
> ```
> public class Shape: ISerializable, IDeserializable
>   polymorphic ("circle": Circle, "square": Square);
>   ...
> end Shape;
> ```

**OLD** (:1066): "A `polymorphic` registry **may appear only together with a graph-shape assertion**… Each named type … must itself **carry a graph-shape clause** … making it serialization-eligible."
**NEW:**
> A `polymorphic` registry is a member of a class that declares `ISerializable`/`IDeserializable`; it has no dependence on any header clause. Each named type must be assignable to the declaring class and itself **type-driven serialization-eligible** (it declares `ISerializable`/`IDeserializable` with serialization-eligible components); a registered type that is not is a compile error at the registry.

*(The rest of §9.8's polymorphic prose — `$type`-first, identity/`$ref` coexistence, dispatch-before-binding — is unchanged in mechanism; only "graph-shape clause" references are reworded to "the registered type's type-driven form".)*

---

## E2h — §9.8 "neither clause" closing (spec :1076)

**OLD:** "Classes that declare **neither** clause remain outside synthesis entirely … A graph-shape clause is the only route by which a class enters synthesis …"
**NEW:**
> Classes that declare neither `ISerializable` nor `IDeserializable` remain outside synthesis entirely and participate only by hand-writing `Serialize` and a recognized static `Deserialize` (stdlib [§2.14](Bezel_Standard_Library.md#sec-serialization-vocabulary)). Declaring the conformance is the only route into synthesis; the field types then fix the serialized shape with no further annotation.

---

## E3 — §9.8 Weak-reachability rule (NEW short subsection, placed near the read-side rules)

> **Weak-reachability.** A `Weak<T>` component serializes and deserializes as a back-reference to a `Shared<T>` value strongly reached elsewhere in the same graph. The synthesized deserializer raises `SerializationException` if a `Weak<T>` `$ref` resolves to a tag that no `Shared<T>` occurrence ever registered (a target never strongly reached) — strong-reachability is a per-value property the field types cannot decide. A static lint additionally flags, at the conformance declaration, a `Weak<T>` field in a class that serializes no `Shared<T>` of a matching `T`; the lint is a necessary-not-sufficient early check and does not replace the dynamic guarantee.

---

## E4 — Appendix A grammar

**E4a** ClassDecl (:7387–7389): remove the `SerializedClassClause?` line from the production.
**E4b** (:7393): delete `SerializedClassClause ::= "serialized" ( "tree" | "graph" | "cyclic" ) PolymorphicClause?`.
**E4c** (:7394) — move `PolymorphicClause` into the class-member grammar as `PolymorphicMember`:
> `PolymorphicMember  ::= "polymorphic" "(" PolyCase ("," PolyCase)* ")" ";"`
> `PolyCase           ::= StringLiteral ":" TypeName`
> (added to the `ClassMember` alternatives; at most one per class; `polymorphic` is a contextual word here.)
**E4d** contextual-keyword list (:8063) and the §B.2 table (:8088): `serialized` **stays** (the per-member field-name clause: `serialized "name"` / `skip` / `optional`). Update the §B.2 `serialized` row to drop "and the class-header `serialized tree`/`graph`/`cyclic` clause"; `tree`, `graph`, `cyclic` are **no longer contextual keywords** (they had keyword meaning only inside the removed clause). Add `polymorphic` as a contextual word in the class-member `polymorphic(...)` position.

---

## E5 — Appendix C §C.8 invariants (spec :8229–8235)

- Bullet 1 (ineligible-conformance compile error): **keep**, unchanged.
- Bullet 2 ("a class enters synthesis only by declaring a graph-shape clause; the clause appears in the header"): **replace** → "A class enters serialization synthesis by declaring `ISerializable`/`IDeserializable`; its serialized shape is read from field types (`owned`→owning edge, `Shared<T>`→identity participant, `Weak<T>`/borrow→back-edge). No header shape clause."
- Bullet 3 ("declaring a weaker graph shape … detected … raises"): **remove** (no declared shape).
- Bullet 4 ("self-contained construction for `serialized cyclic`"): **remove** (rule retired).
- Bullet 5 ("two-phase deserialization … one unit for definite-assignment"): **remove** (no two-phase; cycles via nullable `Weak<T>?`).
- Bullet 6 (polymorphic registry closed): **keep** the closed-registry/no-reflection security, **update** → registry is a class-body member; each registered type must be type-driven serialization-eligible.
- Bullet 7 ("`serialized`, and the in-clause words `skip`, `optional`, `tree`, `graph`, `cyclic`, `polymorphic`, are contextual"): **replace** → "`serialized` (the per-member field-name clause) and its in-clause words `skip`, `optional` are contextual; `polymorphic` is contextual in the class-member registry position."
- **ADD** two bullets: (i) "`Shared<T>`/`Weak<T>` are serialization-eligible when `T` is a serialization-eligible class; a `Shared<T>` serializes by identity (tag keyed on the value `Get()` borrows), a `Weak<T>` as a back-reference." (ii) "A `Weak<T>` `$ref` whose target is never strongly reached in the graph raises `SerializationException` at deserialization."

---

## E6 — stdlib §2.14 recognized-`Deserialize` return (stdlib :545–553)

**OLD** (the two recognized signatures + the return-rule paragraph): value-type → `T`; class → `owned T`.
**NEW:** add a third recognized form and the criterion:
> ```bezel
> public static method Deserialize(d: IDeserializer): owned Shared<T>;  // co-owned entry:
>                                                                       //   the class IS a shared node
> ```
> and amend the return-rule paragraph: the recognized factory returns the declaring type, written concretely;
> `owned T` for a class, plain `T` for a value-type target, and `owned Shared<T>` when the declaring class is
> itself a shared node (it participates in its own serialized graph by identity, deserialized as a co-owned
> handle). Which form a synthesized class uses is locally decidable: a class that *holds* `Shared`/`Weak`
> fields returns `owned T`; a class that *is* the shared node returns `owned Shared<T>`.

---

## E7 — stdlib §2.14/§2.15 $id/$ref citation (stdlib :704)

**OLD:** "the graph-identity operations of the synthesis (language spec [§9.8] — first-occurrence tags and back-references **under `serialized graph`/`serialized cyclic`**) map to JSON as follows…"
**NEW:** "the graph-identity operations of the synthesis (language spec [§9.8] — first-occurrence tags for `Shared<T>` identity participants and `$ref` back-references for `Weak<T>` and borrowing fields) map to JSON as follows…". The `{"$id":n}` / `{"$ref":n}` wire forms and the `$`-reserved-field-name rule are **unchanged**. Audit §2.15 for other `serialized graph`/`cyclic` keyword references and reword to the type-driven description (wire form unchanged).

---

## Cross-cutting / apply notes
- Apply against EXACT current text (re-read each anchor at apply time; the OLD blocks above are quoted from
  reads but verify verbatim before Edit).
- Keep every UNCHANGED part of §9.8 (Serialize/Deserialize wire token sequences, field-name customization
  v0.117, unknown-field policy v0.118, the record/enum synthesis) intact.
- §7.9 spec:655 ("one compiler-controlled construction mode … two-phase deserialization of a
  `serialized cyclic` object group") must be updated: the two-phase cyclic mode is gone. Reword to the
  DAG borrowing-back-edge constructor-injection as the compiler-controlled mode (which remains), or drop the
  serialization-specific exception if borrowing back-edges via ctor-injection are covered by the ordinary
  rule. (Tie-in to #717: the §28.3 borrow-in-field wording is being corrected separately.)
- Version: land with §5.22 + §5.19-closing-para as ONE revision. Number TBD (PLAN said v0.142; current
  v0.140; owner-held 0.141 errata separate) — confirm with owner before the header bump + changelog.

---

## REMAINING-EDITS CHECKLIST (post 3-lens review wf_26202bb7-9ca; full punch-list in that task output)
Review verdict: APPLY-WITH-FIXES — model sound, edit set was INCOMPLETE (dangling refs). Apply order:

**DONE:** §7.9:655 two-phase paragraph → type-driven statement. **§9.8 FULLY MIGRATED** (E1+E2a eligibility :997; heading→"Class serialization synthesis" (anchor kept); declaration-based intro; type-driven shape/partition/Serialize/Deserialize; ctor-selection; read-side identity+Weak-reachability; type-scoped/operation-scoped note (exhaustive para removed); polymorphic-deferral reword; cyclic subsection→short Weak-on-Shared paragraph (sec-cyclic-class-synthesis anchor REMOVED); polymorphic rehomed to class-body member + all keyword/two-phase/cyclic-link refs cleared). Verified: §9.8 body grep-clean of residual keywords; only remaining sec-cyclic-class-synthesis link is at AppC :8209 (removed by E5).

**SPEC FULLY DONE & VERIFIED CLEAN** (Bezel_Language_Spec.md, net -57 lines): §7.9 + ALL §9.8 + grammar (ClassDecl drops SerializedClassClause; SerializedClassClause/PolymorphicClause productions → PolymorphicMember added to Member; both prose comments handled; §B.1+§B.2 updated, `polymorphic` now a standalone contextual kw) + AppC §C.8 (7 bullets → declaration-based + Shared/Weak eligibility + identity/Weak-reachability + rehomed polymorphic; the LAST sec-cyclic-class-synthesis link removed). grep: 0 residual `serialized tree/graph/cyclic` (except the negating new text), 0 dangling cyclic anchors, 0 two-phase/self-contained outside negations.

**REMAINING:** stdlib §2.14 (E6 return :545-553; tutorial :600-656; E7 citation :704) + rationale (§5.22 insert before `## 6`; §5.19 closing para; §18:1273) → then validate + v0.141 changelog/bump + commit.

**§9.8 (spec) — apply E1-E7 core + the missed dangling sites:**
- [ ] :997 eligibility (E1+E2a) — VERBATIM-confirmed.
- [ ] :1002 heading "Tree-shaped and shared-reference class synthesis" → rename "Class serialization synthesis" BUT KEEP anchor id=sec-tree-shaped-class-synthesis (links at :1072 depend on it).
- [ ] :1004 intro "A class may opt into … declaring a `serialized` graph-shape clause … two forms" → declaration-based opt-in (E2b extended UP from :1006).
- [ ] :1006-1009 tree/graph defs → type-driven shape (E2b) — VERBATIM.
- [ ] :1011 record-component "must carry a graph-shape clause" → "must be type-driven serialization-eligible (declares ISerializable/IDeserializable)". (Consider: do Shared/Weak record components need exclusion? a record has no owning tree — FLAG.)
- [ ] :1013/1015/1017 partition + Serialize + Deserialize (E2c) — VERBATIM (use FULL :1024 bullet text, not ellipsis).
- [ ] :1019 ctor-selection — clarify Weak<T>? not ctor-bound, Shared via Clone (minor).
- [ ] :1021 "graph shape is an assertion … enforces it dynamically" lead-in → identity-preservation + Weak-reachability lead-in.
- [ ] :1023-1024 read-side bullets (E2e) → identity-preserved + Weak-reachability — VERBATIM.
- [ ] :1026 type-scoped para → delete (E2d).
- [ ] :1028 "graph-shape clauses exhaustive as of v0.124" → delete (E2d).
- [ ] :1030 polymorphic-deferral "under any graph-shape clause" → "for a type-driven serializable class".
- [ ] :1032-1053 cyclic subsection (heading+body) → REMOVE (E2f); its anchor sec-cyclic-class-synthesis is deleted → audit ALL links to it.
- [ ] :1055-1076 polymorphic (E2g/P6): :1058 header→class-body member; :1066 coupling+eligibility→type-driven; :1070 "phase-two bind-back under serialized cyclic"→reword; :1072 dead sec-cyclic link + "two-phase binding"→Clone/Downgrade resolution; :1074 "composes with serialized tree/graph/cyclic … two-phase"→type-driven; :1076 neither-clause→declaration-based (E2h).
- [ ] E3 Weak-reachability subsection — SINGLE statement (dedup with E2e: make E3 normative, E2e cross-ref).

**Grammar (Appendix A spec):**
- [ ] :7389 drop SerializedClassClause? from ClassDecl; :7393 delete SerializedClassClause; :7394+:7395 PolymorphicClause/PolyCase → move to a **Member** alternative (NOT ClassMember; Member at :7540) as PolymorphicMember (visibility-less, at most one/class).
- [ ] :7396-7447 prose comment block: DELETE the SerializedClassClause comment (:7413-7447); REWRITE the PolymorphicClause comment (:7396-7412) for the member form (drop graph-shape coupling).
- [ ] §B.2 table :8088 row `serialized`: drop "class-header serialized tree/graph/cyclic clause"; tree/graph/cyclic no longer contextual; add `polymorphic` (class-member position). B.1 :8063/:8071 need NO change (only `serialized` listed there).

**Appendix C §C.8 (spec :8229-8235):** E5 — bullet2 replace, bullets 3/4/5 remove, bullet6 keep+rehome, bullet7 reword, +2 ADD (Shared/Weak eligibility; Weak-reachability dynamic). VERBATIM-confirmed (7 bullets).

**stdlib §2.14:**
- [ ] :545-553 add `owned Shared<C>` recognized return + locally-decidable criterion (E6; spell `Shared<C>` concretely, C=declaring class).
- [ ] :600-656 WORKED-EXAMPLE TUTORIAL (E9, the largest miss): Order(tree)/Invoice(graph)+Deserialize-prose:624/Shape(poly header)/Node(cyclic)+two-phase:654/closing:656 → ALL to type-driven (declaration-only classes; owned/Shared/Weak fields; class-body polymorphic member; Shared-spine+nullable-Weak?-back-edge cyclic example; rewritten closing). Use REAL §2.16 APIs.
- [ ] :704 $id/$ref citation (E7) — drop serialized graph/cyclic, type-driven framing. VERBATIM. (§2.15 :706 = generic §9.8 ref, leave.)

**Rationale (Bezel_Design_Rationale.md):**
- [ ] Insert §5.22 (serialization-703-rationale-5.22-DRAFT.md) before `## 6. Nullability` (~L644); KEEP the §5.22 self-anchor.
- [ ] Append the §5.19 closing paragraph (from the rationale draft).
- [ ] §18:1273 "Serialization: complete … only remaining reason … self-contained" → update (self-contained retired; point to §5.22).
- [ ] §1 glossary :280 "serialization-eligible | records and enums" → broaden (classes + Shared/Weak) [minor, optional].

**Land:** changelog (rationale §19) + version bump **v0.140 → v0.141** (3 doc headers; OWNER-CONFIRMED 2026-06-14: the migration is v0.141; the held errata batch stays SEPARATE and lands later as v0.142+) + mechanical validation (grep NO residual `serialized tree/graph/cyclic`/two-phase/self-contained outside changelog history; all anchors resolve) → commit (git add the 3 docs ONLY, NOT -A) → push → mark task #34 done.
