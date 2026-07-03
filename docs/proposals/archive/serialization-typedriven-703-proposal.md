> **Status:** proposal for the owner's decision — commits no spec text, changes no code. #703 follow-on.
> **Provenance:** drafted by a 5-lane research workflow (wf_3d75de53-82d) over the spec, rationale, stdlib, and serialization implementation; load-bearing claims independently verified by the orchestrator (wire markers, §9.8 eligibility list, the Shared-field-ineligible behavior on main, keyword surface). One draft error (a `Shared<T>` field being “misclassified as a back-edge”) was corrected to the verified behavior (sema rejects it as ineligible first).

---

# Design Note: Type-Driven Serialization — Can `Shared<T>`/`Weak<T>` Field Types Subsume the Serialized-Shape Keywords?

*A proposal for the owner's decision. #703 follow-on. Reasoning-first, in the style of Rationale §5.19; commits no spec text.*

---

## 1. The problem: two representations of shared data, only one serializable

Bezel today has **two distinct ways to express that data is shared**, and they do not meet.

**Representation A — owned-tree + borrow + shape clause (§9.8).** A serializable class declares one of three shape keywords on its header (`serialized tree | graph | cyclic`). The owning spanning tree is read directly off the `owned`/value field annotations; borrowing fields (non-`owned` class-typed fields) are back-edges into that tree (spec §9.8, Bezel_Language_Spec.md:1013). This representation is **serializable** — it is the whole of the §9.8 synthesis machinery — and it returns a single `owned T` to one caller (Bezel_Standard_Library.md:545–548). But it is **not user-constructible as a co-owned graph**: a borrow is a static reference into a tree owned exclusively elsewhere, not a thing two objects can independently keep alive.

**Representation B — `Shared<T>`/`Weak<T>` (§28.13, v0.134/0.140).** N handles jointly own one underlying value; the last handle's disposal disposes it. `Clone()` mints a second independent handle to the *same* value (spec §28.13, v0.140); `Weak<T>` is a non-owning observer that breaks cycles. This representation **is** user-constructible and genuinely co-owned. But it is **not serializable at all**: `Shared<T>` and `Weak<T>` appear nowhere in the §9.8 eligible-component enumeration (Bezel_Language_Spec.md:997), `isSerializationEligibleType` returns false for any class without a `serializedClass` clause (TypeResolution_Checks.cpp:2135–2144), and a field typed `Shared<T>`/`Weak<T>` makes its enclosing type synthesis-ineligible with a compile error at the conformance declaration (TypeResolution_Checks.cpp:2706–2716). The two mechanisms share **zero lines** of code: `grep 'Shared|Weak'` over LlvmBackend_Serialization.cpp returns nothing.

**The #703 gap, precisely.** A program that models a shared object graph in the natural, user-facing way — `Shared<Node>` fields, `Weak<Node>` back-references — cannot persist that graph. To serialize, the author must *restructure the type* into the owned-tree-plus-borrow form, which is a different ownership model with a different (single) disposal topology. The data is the same shape on the wire (`$id`/`$ref` DAG either way); only the in-memory type differs.

**The observation that motivates this note.** The shape keywords were designed v0.114–v0.124, *before* `Shared<T>`/`Weak<T>` landed at v0.134. They are, in a real sense, **pre-`Shared`/`Weak` scaffolding**: a way to talk about reference identity and cycles in a language that, at the time, had no first-class shared-ownership type. Now that it does, the natural question is whether the *type* of a field already says everything the shape keyword was invented to say.

---

## 2. The proposal: derive wire-form from field type

Let the **field type** drive the wire form, per a small fixed table:

| Field form | Wire form | Already implemented as |
|---|---|---|
| `owned T` / value field | inline body | `BZ_SER_INLINE` (0x01) + LE32 tag, `bz_binser_write_inline_tag` |
| `Shared<T>` | identity node: `$id` on first reach, `$ref` thereafter | the *same* `$id`/`$ref` DAG path, keyed on the underlying value pointer |
| `Weak<T>` | `$ref` back-reference only (never owns the body) | `BZ_SER_REF` (0x02) + LE32 tag, `bz_binser_write_ref_tag` |

Under this rule the **shape keyword becomes redundant as a gate**: a class with any `Shared<T>`/`Weak<T>` field is *ipso facto* a graph; a class with only `owned`/value fields is a tree. The keyword could be retired, or — the safer variant developed in §6 — retained as an **optional, compiler-checked assertion** of the same fact, so that writing `serialized tree` on a class that has a `Shared<T>` field is a diagnosed contradiction rather than silently honored.

**Before** (today — must restructure to serialize):

```
class Department: ISerializable, IDeserializable serialized graph
    owned head: Employee          // owning edge
    members: Array<Employee>      // borrows into the same tree (back-edges)
end Department
// Employees are NOT independently co-owned; the Department tree owns them all.
// Deserialize returns owned Department; one caller owns the whole tree.
```

**After** (the in-memory co-owned type *is* the serializable type):

```
class Department: ISerializable, IDeserializable    // no shape keyword needed
    head: Shared<Employee>            // co-owned identity node -> $id / $ref
    members: Array<Shared<Employee>>  // each a $ref to its $id
    manager: Weak<Employee>           // non-owning back-ref -> $ref
end Department
```

The wire bytes are the same `$id`/`$ref` DAG in both cases. The difference is that the *After* type is the one users would actually write to model a shared org chart, and it round-trips through `Shared`/`Weak` rather than collapsing to a single-owner tree.

---

## 3. Mechanism: what is reused, what is new

The encouraging finding is that **the wire machinery is fully generic and reusable unchanged**. `BZ_SER_INLINE`/`BZ_SER_REF`, `bz_binser_write_inline_tag`, `bz_binser_write_ref_tag`, the serializer tag table (`tag_ptrs` in `bz_binser_t`, `bz_binser_graph_assign_tag`/`get_tag`), and the deserializer tables (`tag_ptrs`/`tag_in_progress` in `bz_binder_t`) are format-agnostic and operate on raw object pointers. The JSON projection (`"$id":n` injected as the first field by `bz_binser_begin_object`; `{"$ref":n}` standalone by `bz_binser_write_ref_tag`) is likewise generic. **The proposal's `Shared<T> -> $id/$ref` rule maps exactly onto what `bz_binser_graph_assign_tag` + `bz_binser_write_inline_tag` already do for owned class components** — because `Clone()` produces a second handle to the *same* underlying heap object (§28.13), keying the identity table on the value pointer (which `Shared<T>.Get()` exposes) de-duplicates correctly and necessarily.

Four concrete interventions are required:

**(1) Sema eligibility + `collectClassComponents` classification (two-part).** *Verified correction:* a `Shared<T>`/`Weak<T>` field does NOT reach codegen today — sema's §9.8 eligibility check rejects the enclosing type first (“field `head` has a type that is not serialization-eligible”, a compile error at the conformance declaration; confirmed by compiling a `serialized graph` class with a `Shared<Emp>` field on main). So the fix is two-part: (a) **sema** must admit `Shared<T>`/`Weak<T>` to the eligible-component set (§4 below); and only then (b) **codegen** `collectClassComponents` (LlvmBackend_Serialization.cpp:140–164), which today partitions fields into `isOwned` (the `owned` keyword) vs `isBackEdge` (non-`owned` + Named class type), must classify `Shared<T>` to the **inline+tag** path and `Weak<T>` to the **ref** path — otherwise a non-`owned` `Shared<T>` field would fall into the default back-edge branch. Both keyed on a compiler-known type, not on a keyword.

**(2) The `isGraphMode` switch.** Today `isGraphMode` is gated *solely* by the shape keyword (LlvmBackend_Serialization.cpp:1355–1359, 1640–1642) and controls the entire tier-3 code path. To retire the keyword, a class must enter graph mode when it has any `Shared<T>`/`Weak<T>` field. This is a **codegen architectural change**, not just a classification tweak — the switch's source of truth moves from the header clause to the field-type set.

**(3) Synthesis trigger is unchanged.** What triggers synthesis stays `ISerializable`/`IDeserializable` conformance (the marker-interface opt-in, mirroring auto-equality; `IDeserializable` is matched by the static `Deserialize` factory name/shape per Bezel_Standard_Library.md:542–553). **The interfaces are load-bearing and the proposal does not touch them.** Only the *shape gate* — currently the keyword — is what field types subsume.

**(4) Eligibility-rule amendment.** `Shared<T>`/`Weak<T>` must be added to the §9.8 eligible-component set (recursively: `Shared<T>` eligible iff `T` is). This is a **normative spec change** (Bezel_Language_Spec.md:997; sema TypeResolution_Checks.cpp:2069–2144), and per CLAUDE.md spec amendments are out of scope for autonomous work — it needs the owner.

What the **synthesized `Serialize`** does per field type: for `owned`/value, recurse inline as today; for `Shared<T>`, `get_tag` on the value pointer — assign + inline-body on first reach, emit `$ref` to the existing tag thereafter; for `Weak<T>`, `TryUpgrade`, then emit `$ref` to the (already-assigned) tag of the live target, or a null marker if dead. What the synthesized **`Deserialize`** does is the unresolved part — see §4(a).

**Notably, the common `Shared`/`Weak` graph does *not* need the deferred cyclic two-phase.** Because `Shared<T>` is the owning edge and `Weak<T>` is the non-owning edge, the *ownership* tree is acyclic; a cycle in the object graph closes through a `Weak<T>`, which is just a `$ref` to an already-emitted `Shared<T>` `$id`. That is the ordinary graph-DAG resolve path, not the two-phase bind-back. This is why the proposal can claim to subsume `serialized cyclic` **for the `Shared`/`Weak` shape** without first landing `feat-550-cyclic` — though see §6 for the caveat about user-written `owned`-cycles, which remain impossible regardless.

---

## 4. The hard questions, worked honestly

### (a) What does `Deserialize` return for a co-owned graph? — **UNRESOLVED. This is the sharpest residue.**

The current `Deserialize` returns `owned T` — a single root whose disposal cascade owns the entire spanning tree (Bezel_Standard_Library.md:545–548; spec §9.8:1017). **A `Shared<T>` graph has no single root by construction**: N handles co-own one value with no designated owner (§28.13; the stdlib cache example has `_index` and a local each owning a clone, neither "the" owner). So `owned T` is not a meaningful return shape for a co-owned graph.

The candidate answers, none obviously correct:
- **Return `owned Shared<T>` to the root.** The caller holds one vote in the refcount; internal co-owners hold clones minted during construction. Clean-ish, but presumes a distinguished "root," which a general graph does not have.
- **Return an `OwnedArray<Shared<T>>` of entry handles.** Honest about multiplicity, but awkward and unprecedented.
- **A new wrapper type.** More machinery; defers rather than answers.

The disposal-soundness corollary is real: §28's single-owner-single-tree disposal guarantee (reverse-declaration cascade, §28.4/§28.11, fix #598) **composes cleanly only when each object has exactly one owner at any time**. A deserialized graph where external callers hold clones *and* internal fields hold clones has disposal order undefined across those callers. **This question has no answer in the current spec and must be settled at the type-contract level before any synthesis can be written.** It is the gating decision for the whole proposal.

### (b) `Shared` identity = the underlying value, mapped to `$id`/`$ref`. — **RESOLVED.**

`Clone()` makes a second handle to the *same* value (§28.13, v0.140), and the existing tag table keys on object identity. Keying on the value pointer (via `Get()`) makes two handles serialize as one `$id` node with subsequent `$ref`s — exactly the de-duplication §9.8 already promises and the precise opposite of the serde "silently duplicate" footgun the rationale names (§5.19, Bezel_Design_Rationale.md:581). This part is semantically clean and needs only the classification change of §3(1).

### (c) `Weak<T>` resolution and the dangling hazard. — **PARTIALLY RESOLVED; a real residue remains.**

The *wire* form is clean: `Weak<T> -> $ref`, resolving to an already-emitted `Shared<T>` `$id`. The *binding* is the problem. Today's graph back-edges are §7.9-safe because **borrows are passed through the constructor** (rationale §5.19:599): the target is provably constructed before the borrower. **`Weak<T>` is not a borrow — it is an owned class instance** minted via `Downgrade()` from a live `Shared<T>` handle. The constructor-argument-injection argument does not transfer. Two sub-hazards:
- **Target absent from the stream.** Unspecified today. Must be defined: `SerializationException`, a nullable `Weak<T>?`, or a defined "dead-on-arrival" `Weak`.
- **Target present but handle disposed during construction** (e.g., an exception path drops the `Shared` before the `Weak` is minted). The `Weak` is dangling immediately; `TryUpgrade` returns false. The mechanism must prove the source handle is alive at the minting point.

The wire-form mapping is decided; **the binding discipline must be worked out §5.19-style before spec text.** Residue: not blocking the *concept*, but blocking *soundness*.

### (d) The runtime-aliasing-of-a-plain-`owned`-field ambiguity that `tree` currently catches. — **A GENUINE LOSS the proposal must answer.**

`serialized tree` is not only a gate; it is a **dynamic assertion** that no two references alias the same object, enforced at read-time by raising `SerializationException` on repeated identity (spec §9.8:1006, 1021–1024). The type `owned Shared<T>` says "I own a handle" — it does **not** assert the underlying value is unaliased, because `Clone()` lets two `owned Shared<T>` fields at distinct owners legally co-own one value. **Types do not encode the tree-ness assertion.** If the keyword is retired outright, this assertion loses its anchor. This argues strongly for the **retain-as-assertion** variant: keep `serialized tree` available as the compiler/runtime-checked claim "this graph is in fact a tree," even when field types alone cannot prove it. A class with a `Shared<T>` field simply could not bear `serialized tree` (a clean contradiction diagnostic).

### (e) §7.9 / disposal / domain-locality interactions. — **MIXED.**

- **§7.9 (definite assignment):** the `Weak` back-edge binding (4c) is the live §7.9-adjacent tension; unresolved.
- **Disposal:** the co-owned cascade (4a) is the unresolved core.
- **Domain-locality (§28.13/§30.14):** `Shared<T>`/`Weak<T>` are domain-local and *not sendable* unless `T` is sendable-and-synchronized (the `Mutex`/`Atomic`/`AtomicInteger`/`Semaphore` set). **This bounds the proposal's scope to external serialization** (file/network/persistence), where domain-locality does not apply. It does **not** enable cross-domain in-process `Bezel`-to-`Bezel` graph transfer for general `T` — only `Shared<synchronized T>` could cross a channel, which is a narrow slice. The proposal should explicitly target persistence/wire, not channels. **Resolved as a scoping statement, not a capability.**

---

## 5. Tradeoffs vs the current model

**Owned-tree + borrow + shape clause (current):**
- *Gained:* refcount-free — no per-node control block, no atomic count; a single clean `owned T` return; a strict disposal tree with defined reverse-order cascade; the owning-tree/back-edge partition read off annotations with an owning cycle *structurally impossible* (a cycle can only close through a borrow). The deserializer always has a topological construction order.
- *Lost:* **not user-constructible as a genuine co-owned graph.** The model is single-owner with static borrows; you cannot hand two objects independent, lifetime-extending ownership of one child. To serialize a shared graph you must contort it into this shape.

**`Shared<T>` / `Weak<T>` (proposal):**
- *Gained:* the in-memory type users *actually write* for shared data becomes serializable directly — no restructuring; the two representations of shared data unify into one; cycles handled by `Weak` without the deferred two-phase.
- *Lost:* refcounting cost (control block, count maintenance); **cycles leak** if the `Weak` discipline is not maintained, and the compiler does not detect this (§28.13; rationale §5.21 — "cycle detection would be a tracing collector by another name"); the clean single-`owned T` return is gone (4a); the static tree-ness assertion is gone (4d); the round-trip **loses the strong/weak intent** — a `Weak` that was a deliberate cycle-breaker is indistinguishable on the wire from a `Weak` that was merely a non-owning observer, so the deserializer cannot re-derive the original discipline from types alone (this is the deepest semantic loss: types under-determine the reconstruction).

Cross-language precedent sharpens the read. Only **Java default serialization** and **C++ boost::serialization** are genuinely "type-driven identity" — and both lean on properties Bezel deliberately lacks: Java on a GC that keeps objects alive through serialization and tolerates half-constructed objects on load; boost on fixed-address value-at-pointer C++ semantics. The rationale already cites Java at exactly this seam (§5.19:603): "Java, with full GC and unrestricted mutable identity, cannot preserve a cycle through an immutable record's constructor for precisely the reason Bezel's two-phase-plus-self-contained-constructor design is engineered around." **Rust serde and Swift Codable explicitly do *not* preserve reference identity** — they duplicate (the serde footgun) or overflow on cycles. The honest reading: type-driven identity is achievable, but the mainstream successes achieve it by relying on a GC. Bezel's no-GC promise is exactly why this is hard, not incidental to it.

---

## 6. Migration

**Shipped on main (tiers 1–4):** records/enums auto-synthesize from `ISerializable`/`IDeserializable`; classes synthesize from a shape clause; `$id`/`$ref` DAG (`serialized graph`) round-trips; polymorphic `$type`. **`serialized cyclic` is aliased to graph mode on main** (`isGraphMode` true for both; LlvmBackend_Serialization.cpp:1355–1359) — a genuine cycle raises `SerializationException` (the in-progress resolve returns null → `emitThrowBuiltin`). So "cyclic ≡ graph" on main.

**Held (tier-5):** the genuine two-phase bind-back lives only on `feat-550-cyclic` (`bz_binder_cyclic_defer_binding`/`do_bind_back`/`enter`/`exit`; 1404 net insertions; **not merged**). The proposal's `Shared`/`Weak` path does **not** require it for the acyclic-ownership `Shared`+`Weak`-back-edge case (§3). It *would* be required only for a user-written **`owned`-field cycle**, which the ownership model forbids anyway — so the residual need for tier-5 is the original keyword-based `serialized cyclic` for *records/borrows*, independent of this proposal.

**Breaking surface — well-bounded.** The keyword appears in **190 lines across 15 test files** (`tree` 112, `graph` 69, `cyclic` 15; largest: serialization_tier2_classes_test.cpp, 87), stdlib doc examples (Bezel_Standard_Library.md ~600–644), the §9.8 normative text, and a handful of conformance cases. **Zero guide occurrences. No external users** (CLAUDE.md: "no users yet, the compiler does not yet exist"). The *practical* cost of retirement is near-zero.

**But the *doctrinal* cost is real.** Retiring the keyword is **source-breaking** by the rationale's own §19 category, and it contradicts the per-tier non-breaking staging discipline every serialization revision honored ("purely additive; classes without it are unaffected," v0.119–v0.124). And the placement criterion (§5.19) grounds the keyword as *type-scoped because shape is an intrinsic property of the type's serialization contract.* The adversarial counter is sharp: whether a `Shared`/`Weak` instance is a DAG or a cycle is a property of the **runtime graph, not the type** — the same class can instantiate acyclically or cyclically. So field-type inference does **not** cleanly satisfy the criterion the way an explicit assertion does.

**This yields two migration paths:**
- **Clean replacement:** retire `tree/graph/cyclic`; infer everything from field types. Maximally unifying; incurs the source-break, the placement-principle conflict, and the loss of the tree-ness assertion (4d).
- **Non-breaking coexistence (recommended posture):** keep the keywords as **optional, compiler-checked assertions**; add `Shared<T>`/`Weak<T>` to the eligible set as `$id`/`$ref` participants. A class with `Shared`/`Weak` fields enters graph mode automatically; writing `serialized tree` on it is a diagnosed contradiction; writing `serialized graph` is a redundant-but-honored confirmation. This **closes the #703 gap, dodges the v0.118 placement conflict, preserves the aliasing assertion, and breaks nothing.**

**Core/Extended implications:** Serialization (§9.8) and Shared-ownership (§28.13) are *separate* Appendix-D Extended features with **no inter-feature dependency in either direction**. Any unification must be declared as a **new cross-feature interaction**, not as an extension of either — which is itself a design-surface decision for the owner.

---

## 7. Recommendation and the decisions only the owner can make

**My honest read:** the *kernel* is right and worth pursuing — `Shared<T>` → `$id`/`$ref` is a clean, correct, de-duplicating fit onto machinery that already exists, and "the type users write for shared data cannot be serialized" is a real, unprincipled gap that #703 correctly names. **But the maximal form — retiring the shape keywords — is not yet sound and is doctrinally costly.** Three things block the maximal form: the co-owned `Deserialize` return type (4a, no answer in the spec), the `Weak` binding discipline (4c, no §7.9-safe construction story), and the loss of the tree-ness assertion (4d, types do not encode it). The cross-language evidence (only the GC languages get type-driven identity for free) reinforces that this fights the no-GC foundation rather than riding it.

**Recommendation:** pursue the **narrow, non-breaking form** — *keep the keywords as optional assertions; admit `Shared<T>`/`Weak<T>` into the eligible-component set as participants in the existing `$id`/`$ref` DAG path.* This delivers the actual value of #703 (shared graphs become serializable without restructuring) while leaving the keywords' assertion role intact and breaking nothing. Treat full keyword retirement as a **separate, later question** contingent on first resolving 4a/4c in a §5.19-style working-out. Do **not** undertake the spec amendment autonomously — it is out of scope per CLAUDE.md.

**Sharp decisions for the owner:**

1. **Scope of ambition:** narrow (admit `Shared`/`Weak`, keep keywords) vs broad (retire keywords, infer shape from types)? Everything else follows from this.
2. **The co-owned `Deserialize` return type (4a) — the gating question.** `owned Shared<T>` to a root? `OwnedArray<Shared<T>>`? A new wrapper? Until this is decided, no synthesis is sound. *What does it mean to "own" a deserialized graph that owns itself?*
3. **`Weak` target absence and dangling (4c):** `SerializationException`, nullable `Weak<T>?`, or a defined dead-on-arrival `Weak`? And what is the §7.9-safe equivalent of constructor-argument injection for an owned `Weak` field?
4. **Does field-type inference satisfy the §5.19 placement criterion?** The adversarial case (DAG-vs-cycle is an instance property, not a type property) says it does not cleanly — the owner must rule on whether the criterion demands an explicit assertion.
5. **Eligibility shape:** `Shared<T>` eligible unconditionally, or only when `T` is serialization-eligible? (Recursive rule makes the latter mechanical, but it is a normative choice.)
6. **Wire-format compatibility:** is binary/JSON backward-compat with keyword-bearing data a hard requirement? If yes, retirement needs a wire-version migration story; if no (no users), it is free.
7. **The strong/weak-intent loss (§5):** is it acceptable that a round-trip cannot distinguish a cycle-breaking `Weak` from an observer `Weak`, or must the wire carry that metadata?

The discipline this project runs under says: ship the part that fits cleanly now (admit `Shared`/`Weak` to the DAG path), decline-with-reasons the part that fights the foundations (keyword retirement, until 4a/4c are worked out), and keep the adjacent open questions explicitly scoped (the seven above). That is the recommendation.

---

# Appendix A — Worked out: the co-owned `Deserialize`-return contract (resolves §4(a))

> **Orchestrator verification.** Drafted by workflow wf_d7338d97-50a (mechanics-grounding + adversarial disposal-soundness + alternatives → Opus). Load-bearing runtime claims independently re-checked against main: `bz_binder_free` has NO per-element release (the handle-mode teardown is genuinely net-new); `bz_shared_release` disposes exactly once at `strong→0` behind a `value_disposed` guard (double-free structurally impossible — confirmed); `bz_shared_clone` is an atomic `fetch_add`. **One refinement to §4(e) below (orphan-via-`Weak`):** that case is largely *pre-closed* by an existing serialize-side invariant — the serializer raises `SerializationException` for any back-edge (`$ref`, which `Weak`→`$ref` maps to) whose target was never reached through an owning edge (`bz_binser_graph_get_tag==0`, LlvmBackend_Serialization.cpp:1499–1513). So any *successfully-serialized* stream already guarantees every `Weak` target is strong-reachable; on deserialize it has a strong holder and is not orphaned. The deserialize-side O(N) reachability check the working-out proposes is therefore **defense-in-depth against malformed input, not a new soundness requirement for well-formed round-trips** — the invariant already exists, on the serialize side. Net: there is **no genuinely-new unsoundness in the disposal logic**; the residues are the inherited `Shared`-cycle leak and disposal-order divergence, plus three mechanical implementation items.

## The co-owned Deserialize-return contract for type-driven `Shared<T>`/`Weak<T>` serialization

*Working-out for the owner — analysis only, no spec text proposed. Citations are to `Bezel_Language_Spec.md` (§), `Bezel_Standard_Library.md` (stdlib §), and the rationale, all at the revision under which #703 is being considered. This is the gating question for the whole proposal; everything downstream of it is mechanical.*

---

## 1. The question, sharply

The synthesized `Deserialize` factory has exactly one return shape today. Per the recognized-factory rule (stdlib §2.14) and §9.8, it is:

```
public static method Deserialize(d: IDeserializer): owned T   // T = declaring class
```

`owned T` means precisely one thing, and the language enforces it everywhere (§28.3): **the holder is responsible for disposing the referenced object exactly once.** A returned `owned T` is a sole-disposal vote. The caller binds it with `using var` or an `owned` field, and on scope exit the destructor cascade (§28.4) runs once, descending owned fields in reverse declaration order. This works because the reconstructed graph for `tree`/`DAG`/`cyclic` clauses is a **single-owner spanning tree**: each node is pointed to by exactly one `owned` field, borrow back-edges (`$ref`) carry no disposal obligation, and the root Dispose cascade reaches every node exactly once (§9.8 line 1013).

A co-owned `Shared<T>` graph breaks this at the root. `Shared<T>` is "the value is disposed exactly once, when the last handle is disposed" (§28.13 line 5751). The entry node of a deserialized co-owned graph has a refcount that is **≥ 1**, and the caller who receives it is *one of N co-owners*, not the sole disposer. If we hand that node back as `owned T`:

- Either the caller's drop runs T's destructor while sibling handles still point at it — a use-after-free of every other co-owner's view, violating exactly-once;
- Or the caller's drop is silently *not* a disposal (because T carries no refcount logic of its own) — in which case `owned T` is lying about the contract, and nothing decrements the count.

Both are unsound. **`owned T` cannot model a node whose disposal obligation is fractional.** This is why the return type gates the entire proposal: until the caller's disposal contract is named correctly, there is no sound place to *put* the result of a co-owned deserialization, and no point implementing the wire-format and tag-table machinery underneath it.

Note one upstream precondition that is *not* the subject of this working-out but bounds it: the existing §9.8 synthesis vocabulary (owned spanning tree + borrow back-edges) has no wire construct for an **owned-but-aliased** edge — two distinct nodes each owning a `Shared` handle to the same value. A spanning tree has no aliased edges. Resolving the return type assumes that gap is closed by a new synthesis mode mapping `Shared` edges to `$id`/`$ref` with handle-cloning on read; the disposal analysis below applies *to that extension*, and that extension does not yet have spec text. I flag it here and return to it in §7.

---

## 2. The recommended contract

> **For a co-owned entry node, `Deserialize` returns `owned Shared<T>` — one handle (one disposal vote) to the serialization-entry node.**

This is the uniquely correct existing type. `Shared<T>` is an ordinary public class, `IDisposable`, `where T: class` (stdlib §2.16); `owned` is admissible on a class return type (§28.1); and a handle is "produced owned ... discharged by `using var`, an owned field, an owned parameter or return, move, or explicit `Dispose()`" (§28.13 line 5747). Handing the caller `owned Shared<T>` gives them exactly one vote in the shared count, discharged by the standard rules — the precise shape of a co-owner's obligation. Dropping it decrements; the value lives until the last handle drops. No new type-system mechanism is required.

### The deserializer handle-lifecycle (the seven-step discipline)

The synthesizer emits a deserializer that maintains a **tag table holding owned `Shared` handles** (not raw pointers — see §5/§7 for why this is a net-new runtime requirement over today's borrow-table). The sequence, grounded in the `bz_shared_*` runtime primitives and §28.13:

1. **Build each inline node owned.** For each `BZ_SER_INLINE` node: allocate the underlying `T` and run its constructor exactly as the current owned-`T` path does (`emitClassDeserialize`), producing an `owned T`.
2. **Wrap in a handle.** `new Shared<T>(owned node)` — an owned-consuming position (§28.13 line 5748); `strong = 1`. This handle is the table's handle for that node's tag. Store it in the table.
3. **Clone for `$ref`.** For each back-edge that must re-bind as a strong co-owner: `Clone()` the table-stored handle (`owned Shared<T>`, `strong += 1` — §28.13) and store the clone into the receiving field. The table keeps its own handle.
4. **Downgrade for `Weak`.** For each `Weak<T>` field: `target.Downgrade()` (`owned Weak<T>`, `strong` unchanged, `weak += 1` — §28.13 line 5753). **This must run while the target's table handle is still live** (it always is at this point — step 6 has not run). Store the `Weak` handle in the field.
5. **(Entry is exempt.)** The entry node's table handle is the one handle the deserializer does *not* release; it becomes the return value.
6. **Release transient table handles.** After *all* constructors complete and *all* clones/downgrades are bound, release every non-entry table handle via `bz_shared_release`. Each non-entry node now has `strong` = (number of field references that Cloned it) — its table's extra +1 is gone. Crucially this happens *after* step 4, so no `Weak` was minted from an already-released handle.
7. **Return the entry handle** as `owned Shared<T>`. Its count is `1 + (aliases pointing at the entry)` — not necessarily 1.

The tag table itself must be an **owning** structure under `using var` (the idiomatic shape is `OwnedDictionary<int, Shared<T>>`), so that on *any* exit path — normal, return, or exception — its remaining handles are released by the collection's disposal cascade (§28.10 line 5467; §28.11 line 5501). The entry slot is nulled out before the table is torn down (its handle was moved into the return), so the teardown skips it. A *borrowing* table (`Array<Shared<T>>`, whose Dispose does not cascade — §28.11 line 5499) would leak every constructed-but-not-yet-returned node on an exception.

---

## 3. The disposal-soundness argument

Claim: with the seven-step discipline, every node in a happy-path deserialized co-owned graph (cycles closed by `Weak`) is disposed **exactly once**, when its last handle drops. Walk the cascade.

**State at return.** After step 6, refcounts are:
- entry node: `strong = 1 + (Shared aliases to entry)`, of which the returned handle is one;
- each non-entry owned node: `strong = (number of fields that Cloned a handle to it)` ≥ 1, since every node is in the owning spanning tree (§9.8 line 1013) and thus reachable by at least one strong edge;
- each `Weak` field: `weak += 1` on its target, `strong` unchanged — it does not keep the value alive (§28.13 line 5753).

**Caller drops the entry handle.** The caller binds the return with `using var entry := T.Deserialize(d)` and, at scope exit, disposes it. `bz_shared_release` does an atomic `fetch_sub(1)` on `strong`. Two sub-cases:

- *No other `Shared` holds the entry.* The entry's `strong` was 1; it now reaches 0; `dispose_value` fires once, running the entry node's own destructor cascade in normal §28.4 reverse-declaration order. That cascade disposes the entry's `owned Shared<U>` fields — each is a `bz_shared_release`, decrementing the *child's* count by one. Those children's counts now reach 0 (if the entry held their only remaining strong reference), firing *their* destructor cascades, and so the disposal propagates outward through the whole graph. Each node fires `dispose_value` exactly once, from its own last release.
- *Another `Shared` holds the entry* (a strong back-edge to the root). Then the entry's `strong` was ≥ 2 and drops to ≥ 1 — the entry is **not** disposed. This is a `Shared` cycle and it leaks; see §4. It is *not* a double-free or a premature free.

**`Weak` back-edges don't hold.** A back-edge re-bound as `Weak` contributes only to the `weak` count. When the strong cascade above drives a node's `strong` to 0, `dispose_value` runs regardless of how many `Weak` handles still point at it; those `Weak` handles' subsequent `TryUpgrade()` correctly return false (the CAS-on-`strong`>0 loop fails — `bz_weak_try_upgrade`), and the control block is freed only when `weak` also reaches 0. So a `Weak` cycle-breaker lets the strong cascade complete: the graph disposes.

**Exactly-once is structural, not incidental.** Two independent invariants guarantee no double-free:
- A node's `dispose_value` can run only when `strong` reaches 0, i.e. only after *every* handle has been released, and each release is a unique atomic `fetch_sub` delta (`bz_shared_release`). There is no path that decrements without disposing a handle, because handles are ordinary owned objects under §28.13 (line 5747) and obey the rest of §28's exactly-once tracking.
- Even were `Dispose()` reached twice, §28.2 (line 4807) makes it idempotent. So double-free is *structurally* impossible given correct handle ownership; the only failure modes available are **leak** (a handle never released) or **dangling-Weak** (a value released too early relative to a surviving observer) — both treated in §4.

The soundness therefore reduces to a single discipline obligation: **step 6 must release exactly the non-entry table handles, exactly once, on every exit path.** The `using var` owning table discharges that automatically; the entry-slot nulling keeps the returned handle out of the teardown.

---

## 4. The honest residues

Five things the analysis surfaces. Some are resolved; some are named residues or rules-needed.

**(a) The `Shared`-cycle leak — NOT new; identical to in-memory. RESOLVED (as a known, accepted property).**
If the user put `Shared<T>` (not `Weak<T>`) on a back-edge, the reconstructed ring keeps every node's `strong` ≥ 1 forever. When the caller drops the entry handle, the entry's count falls to (held-by-predecessor) ≥ 1, never 0; nothing downstream disposes. This is *exactly* §28.13's "Cycles leak ... the compiler does not detect ownership cycles through `Shared<T>`" (line 5752), applied to deserialized state instead of constructed state. The deserializer did nothing wrong and *cannot* fix it; the fix is the same as in-memory — the programmer uses `Weak<T>` for the cycle-closing edge (rationale §5.21; stdlib §2.16 usage note "break them with `Weak<T>`"). **Deserialization does not make this worse than today.** It is a property to document, not a defect to repair.

**(b) Refcount-order vs reverse-declaration disposal order. RESOLVED (acceptable divergence).**
A single-owner tree disposes its fields in reverse declaration order (§28.4). A `Shared` graph disposes in **refcount-reaches-zero order** — which node's last handle drops first. Within any *one* node, the destructor still runs the normal §28.4 reverse-declaration cascade; what is not ordered is *across* nodes. The programmer cannot rely on node A disposing before node B merely because of a declaration order. This diverges from the tree/DAG case but is **identical to in-memory `Shared<T>` disposal** ("disposing the last live handle disposes the underlying value (running its destructor cascade normally)" — §28.13 line 5751, where "normally" is the single-object cascade, not a global graph ordering). Not a regression, not a soundness defect — a semantic difference to document.

**(c) Exception-cleanup obligation. RESOLVED in principle; needs a codegen rule and a runtime hook. RULE-NEEDED.**
If construction throws partway (truncated stream, missing required field, `SerializationException`), every handle the table holds must be released — the field references were never established, so the table holds the only strong references to the partially-built nodes. With an `OwnedDictionary<int, Shared<T>>` table under `using var`, the unwind path runs the collection's disposal cascade and releases each handle exactly once (§28.10 line 5467; §28.11 line 5501) — partially-built nodes with `strong = 1` reach 0 and dispose cleanly; nodes already wired to a live entry handle keep `strong` ≥ 1 and are not prematurely disposed. **Two concrete obligations follow:** (1) the synthesized deserializer *must* use an owning, not borrowing, table; (2) the existing exception cleanup path (`emitClassDeserialize` → `emitDisposeAndReclaim` on already-filled class slots) calls vtable-`Dispose` + free, which is the **wrong operation on a `Shared` control block** — it must call `bz_shared_release` instead. That requires the slot-type tracking in the emitter to distinguish `Shared` slots from plain class slots (a new cleanup branch). This is a rule the proposal must make explicit, plus a small runtime/codegen change.

**(d) The return-type asymmetry (`owned T` vs `owned Shared<T>`). RESOLVED (it mirrors the type, not arbitrary).**
The factory returns `owned T` for a singly-owned root and `owned Shared<T>` for a co-owned entry. This *looks* like a special case but is the type system being honest: the entry's disposal contract is encoded in its type, exactly as `owned`/`borrow`/`Shared`/`Weak` annotations force the contract at every field and parameter site (§28.1). A caller holding `owned T` has the sole vote; a caller holding `owned Shared<T>` has one of N. The asymmetry maps one-to-one onto the ownership difference. The *residue* here is purely how the recognized-factory rule (stdlib §2.14: "return type is the declaring type, written concretely") is amended to admit a return of `owned Shared<DeclaredType>` — a naming/spec-shape decision, not a soundness one (see §5 and §7).

**(e) The orphan-via-`Weak` case. NAMED RESIDUE — NEW RULE NEEDED.**
This is the one genuinely new unsoundness the proposal introduces, and the most important residue. Consider a node B held *only* by a `Weak<T>` field in node A, with no `Shared`/owned strong path from the entry. After step 6 releases B's transient table handle, B's `strong` reaches 0 and B is disposed **during table teardown**, before the caller can reach anything. If A survives (reachable from the entry), then `A.weakField.TryUpgrade()` returns false *immediately after `Deserialize` returns* — the deserialized graph has a **dangling `Weak`** that the serialized graph did not. This is mechanically "correct" (a node with no strong holder *must* be disposed), but it is structurally inconsistent with the round-tripped form, and surprising.

The cure is an invariant the existing borrow model already enforces for `$ref` borrows ("a borrow's lifetime is bounded by an owner that outlives it ... necessarily present in the owning tree" — §9.8 line 1013), lifted to `Weak`: **every `Weak<T>` target in a deserialized graph must be reachable from the entry via at least one chain of strong (owned/`Shared`) edges.** Whether this is enforced *statically* (a shape analysis of `Weak`-vs-`Shared` reachability in the type graph at synthesis time) or *dynamically* (the deserializer verifies, after step 6, that every `Weak` target's `strong` > 0 before returning — O(N)) is an open sub-decision (§7). This rule does not exist today and must be made explicit before the feature is sound.

---

## 5. The return-type rule, and the container-vs-co-owned-node distinction

The return type is **type-driven and mechanical**:

- **Return `owned Shared<T>`** when the *entry node itself is co-owned* — i.e., the declaring type `T` is the thing reached via `Shared<T>` handles in the serialized model, and the caller is handed one of N votes to that node.
- **Return `owned T`** when the entry is *singly owned at the root* — the ordinary `tree`/`DAG`/`cyclic` case over a single-owner spanning tree.
- **Return `owned Container`** (not `owned Shared<Container>`) when `Container` is a single-owner root that *internally holds* a shared graph — e.g. `owned _graph: Node` where `Node` uses `Shared`/`Weak` among its own fields.

The container distinction is the subtle one and it is decisive. A `Container` with an `owned _graph: Node` field **owns** its graph by holding an owned field, not by being wrapped in a handle (§28.13 line 5748: `new Shared<T>(owned value)` *consumes* an owned T — `Container` is the consumer/root, not a co-owned participant). Its `Deserialize` reconstructs the owning spanning tree (Container → Node children, §9.8 line 1013) and returns `owned Container`; the `Shared`/`Weak` co-ownership is entirely *internal* to the graph and is discharged by Container's own reverse-declaration field cascade (§28.4) when Container is dropped. The caller holds **no handle directly** — only the root. This is the least-surprising shape for the common case (a document/scene/registry object that internally shares some nodes), and it keeps the stdlib §2.14 factory shape literally intact.

**Can the synthesizer determine which case applies?** Yes, mechanically, from the declaring type and its field annotations:

- If the type `T` that declares `IDeserializable` conformance *is itself* the type parameter of incoming `Shared<T>` handles in the model (the wire form carries `$id` identity for `T` nodes, and the caller's in-memory form was `Shared<T>`), the synthesized factory returns `owned Shared<T>`.
- Otherwise — `T` is singly owned at the root, with any `Shared` internals reached only through its owned fields — it returns `owned T`.

The signal is static: `Shared<T>`/`Weak<T>` are static field annotations, available at synthesis time, exactly as the borrow-vs-owned partition is today. The reconstruction always follows the *static field type in the target class* (a `$ref` re-binds as `Shared` Clone or `Weak` Downgrade according to the receiving field's declared type), which is correct **so long as the serialized type and the deserialized type are the same class** — the wire form carries no signal distinguishing "this `$ref` will be re-bound `Weak`" from "...re-bound as a second `Shared`." For same-version, same-language round-trips (the only case the type system underwrites) this is sound. For cross-version or cross-language it is not, and the proposal should not claim it.

One unresolved wrinkle (carried to §7): the `IDeserializable` conformance is declared on `T`, never on `Shared<T>` — the user cannot declare conformance on the compiler-known closed `Shared<T>` type. So the synthesized method whose return is `owned Shared<T>` is a **method on `T` with a return type that is not `T`** — a deliberate deviation from "the return type is the declaring type." That is the spec-shape decision, not a mechanics decision.

---

## 6. Alternatives rejected, with reasons

**`owned T` (bare entry) — rejected: mis-models the disposal contract.**
Already argued in §1. `owned T` is a sole-disposal promise (§28.3, §28.8); a co-owned node cannot satisfy it. The caller's drop either runs the destructor while siblings hold references (use-after-free, exactly-once violation) or is silently not-a-disposal (the contract is a lie and the count is never decremented). Structurally the wrong type for a fractional obligation.

**`OwnedArray<Shared<T>>` — rejected: solves a non-problem; wrong level of abstraction.**
Every synthesized `Deserialize` reconstructs from a *single* entry, because `Serialize` is called on a single root (`x.Serialize(s)`), so deserialization yields one root (§9.8: the factory has the single-value class return shape of §2.14). `OwnedArray<Shared<T>>` is syntactically admissible (`OwnedArray<T> where T: class`, stdlib §4.5 / §28.11) but forces the caller to index `[0]` for the entry, adds the array's own discharge discipline, and conveys nothing `owned Shared<T>` doesn't. It is the right shape only for a genuine **forest** (multiple roots with no owning predecessor) — which the single-root `Serialize` entry point does not produce. Flagged as an open question for forests (§7), but wrong as the default.

**`Deserialized<T>` wrapper — rejected: does not exist; cannot be invented; no expressiveness gain.**
There is no `Deserialized<T>` anywhere in `Bezel_Standard_Library.md` or the spec, and CLAUDE.md rule 2 forbids inventing stdlib APIs. The only thing a wrapper could bundle is a post-`Deserialize` handle table for further `$ref` resolution — but serialization synthesis is a *closed*, compiler-generated operation; there is no caller-visible `$ref` resolution after `Deserialize` returns. The caller's contract is exactly "receive one owned handle to the entry; dispose it when done," which `owned Shared<T>` already expresses. A wrapper adds indirection and a new type for zero soundness or expressiveness benefit.

---

## 7. What is resolved vs the remaining owner sub-decisions

### Resolved — sound and shippable once the spec-shape and the two small runtime/codegen changes land

- **The return type for a co-owned entry is `owned Shared<T>`.** Uniquely correct among the four candidates; no new type-system mechanism needed (§2, §6).
- **The seven-step deserializer handle-lifecycle** (build owned → wrap → tag → Clone/Downgrade → release transient handles → return entry) maps cleanly onto the existing `bz_shared_new`/`Clone`/`Downgrade`/`release` primitives and the `BZ_SER_INLINE`/`BZ_SER_REF` wire form, which need no change (§2).
- **The disposal cascade is exactly-once on the happy path**; double-free is *structurally* impossible (count invariant + §28.2 idempotence) (§3).
- **The `Shared`-cycle leak and the refcount-vs-declaration disposal order are inherited from in-memory `Shared<T>`, not new** — document, don't fix (§4a, §4b).
- **The return-type asymmetry and the container-vs-co-owned-node rule are mechanical and type-driven**; the synthesizer can decide from static field annotations (§5).

### Two concrete implementation gaps that must close (mechanical, not design)

- **Runtime:** the tag table must hold `Shared` handles, and `bz_binder_free` (today `free(tag_ptrs); free(tag_in_progress); free(d)` — no per-element callback) must gain a per-element `bz_shared_release` loop, *skipping the entry slot* (the entry handle is moved to the return; null its slot before teardown). The cleanest threading is an `is_shared_table` flag on `bz_binder_t` so teardown knows it is in handle-mode vs today's raw-pointer-borrow mode.
- **Codegen:** the exception cleanup branch must call `bz_shared_release` on `Shared` slots instead of `emitDisposeAndReclaim` (vtable-Dispose + free), which is wrong on a control block (§4c).

### Sub-decisions that need the owner's ruling

1. **Recognized-factory shape (the central spec decision).** The `IDeserializable` conformance is declared on `T`, but the synthesized method returns `owned Shared<T>`, not `owned T`. Is this (a) a deliberate extension of the single recognized-factory rule (§2.14) to admit `owned Shared<DeclaringType>`, or (b) a *second* recognized factory shape distinguished from the single-owner one? Neither needs new soundness machinery — it is a naming/spec-shape ruling.

2. **Explicit clause vs inference.** Is the co-owned-entry case marked by a class-header clause (analogous to `serialized tree`/`graph`/`cyclic`), or inferred from the presence of `Shared<T>` field types? The type-scoped-placement principle (rationale §5.19) argues for an explicit clause — a type either serializes co-owned-at-entry *for every caller* or it does not — but the inference path may be mechanical enough to avoid a new clause. Owner's call.

3. **The orphan-via-`Weak` rule (§4e) — static or dynamic enforcement?** Static = a `Weak`-vs-`Shared` reachability shape analysis at synthesis time; dynamic = an O(N) post-teardown check that every `Weak` target's `strong` > 0 before returning. Or is there a simpler structural constraint that rules out orphans entirely (e.g. "a `Weak<T>` field may only appear in a class that also has an owned/`Shared` path to `T` among its ancestors")? This rule *must* exist before the feature is sound; only its form is open.

4. **The upstream wire-vocabulary gap.** The existing §9.8 spanning-tree + borrow-back-edge model has no construct for an **owned-but-aliased** edge (two nodes each owning a `Shared` handle to the same value). This must be resolved *before* any of the above applies — the disposal analysis here presupposes a new synthesis mode that maps `Shared` edges to `$id`/`$ref` with Clone-on-read. This is the true prerequisite spec item.

5. **Forests and polymorphism (carry-forward).** If a `Shared` graph has two roots with no owning predecessor, a single `owned Shared<T>` can represent only one; the other is disposed on teardown. Is the single-root `Serialize` entry point a strong enough guarantee that the deserialized form is always singly-rooted, or is `OwnedArray<Shared<T>>` needed for forests? Separately, when a co-owned entry is polymorphic (a base-typed handle over a derived instance), does the concrete type's `Deserialize` return `owned Shared<Concrete>` or `owned Shared<Base>` — and does the latter require a `Shared<T>` covariance the type system may not have? And §28.13's sendability limit (a non-synchronized `Shared` graph is domain-local) means the returned handle cannot cross domains — should the return type or an attribute surface that? These are downstream of the gating decision but should be named so they are not lost.

---

**Bottom line for the owner:** the contract should be `Deserialize → owned Shared<T>` for a co-owned entry, `owned T` for a single-owner root, `owned Container` for a single-owner container holding a shared graph; the choice is mechanically inferable from static field types. The disposal argument is sound and exactly-once on the happy path, with double-free structurally impossible. The leaks and disposal-order divergence are inherited from in-memory `Shared<T>`, not new. **The one genuinely new unsoundness is orphan-via-`Weak`, which needs a new reachability rule.** The blockers to shipping are not in the disposal logic — they are (i) the upstream owned-aliased wire vocabulary, (ii) the recognized-factory spec-shape ruling, and (iii) the two small runtime/codegen changes (handle-mode `bz_binder_free` teardown, `Shared`-aware exception cleanup). Everything else is resolved.


---

# Appendix B — Worked out: the owned-aliased wire vocabulary (#703 decision #1)

> **Orchestrator verification.** Drafted by workflow wf_b13e4295-1cf (ground-current-perfield + adversarial "does the wire need a new construct?" + synthesis-logic/wire-richness → Opus). Linchpins independently re-checked against main:
> - **The dynamic inline-vs-`$ref` template already exists** on the owned-field graph path (LlvmBackend_Serialization.cpp:1561-1604): `get_tag(fieldVal)` → if 0: `assign_tag`+`write_inline_tag`+body, else `write_ref_tag` — with a comment that the aliased branch is defensive-and-shouldn't-happen for a tree. For `Shared<T>` that branch becomes the *normal* path. So **no new wire construct** — confirmed across all eight adversarial cases.
> - **The one load-bearing synthesis invariant** (adversarial case 6): the identity key for a `Shared<T>` field MUST be the underlying value pointer (`Get()`), NOT the handle pointer — the existing template keys on `fieldVal` (the object ptr), which for a handle is the *wrong* key (clones are distinct handle objects over one value → the value would serialize twice, aliasing lost). This is a correctness landmine, not a wire question.
> - **Polymorphic `Shared<Base>` is already fully specified** (§9.8:1070): first occurrence = identity tag + `$type` + body; subsequent = pure `$ref`, no `$type`/body; composes with every graph-shape clause. Adversarial case 5 needs nothing new.
> - The deserialize identity table must hold `Shared` handles (not raw `T*`) so `$ref` re-binds via `Clone`/`Downgrade` — the same net-new runtime item Appendix A flagged.
>
> **Net:** no new wire token; the change is synthesis logic at three sites + the deserialize handle-table ABI; the TRUE upstream gate is the spec **eligibility amendment** (`Shared`/`Weak` are not in §9.8's closed component set, §9.8:997). Wire-richness recommendation: **minimal/field-type-driven** (the spec already declines to mark owned/borrow on the wire; strong/weak is a static field-type property, unlike `$type` which dispatch literally needs).

## Working-out: the owned-aliased wire vocabulary (#703, decision #1)

*Style: Rationale §5.19 reasoning-first. This works the problem; it commits no spec text. Citations carried from the grounding analysis are pinned where they bear weight.*

---

## 1. The gap, sharply

Bezel's serialization model (§9.8) rests on a single structural assumption: the reference graph reachable from a serialized root partitions cleanly into a **spanning tree of owning edges** plus a set of **back-edges that borrow**. The synthesizer reads that partition straight off the language's own annotations — `owned` means "write inline, this is the canonical body"; a plain (non-owned) class field means "borrow, write a `$ref` to the tag of the object it borrows, which is necessarily already present in the owning tree because a borrow's lifetime is bounded by an owner that outlives it" (§9.8:1013, §9.8:1015). The whole scheme is sound precisely because ownership is a tree: every value has exactly one owning edge, so there is exactly one obvious place to put its body, and every other edge to it is a borrow that resolves backward.

`Shared<T>` breaks that assumption at its root. Two fields — in the same class or in two unrelated classes — can each **own** a `Shared` handle to the same underlying value. Neither is a borrow (both participate in the value's lifetime via the shared count; §28.13, stdlib §2.16). Neither is "the" owner (that is the entire point of `Shared` — there is no distinguished owner). The owning-edge set is no longer a tree; it is a DAG with multiple owning in-edges to one node. The spanning-tree+borrow vocabulary has **no construct for this edge**: it is owned (so the borrow/`$ref`-always rule does not apply) yet aliased (so the `owned`/inline-once rule cannot be read statically off a single annotation, because *which* of the N owning fields gets to be canonical is not knowable from the annotations alone).

This gates everything downstream because **type-driven `Shared`/`Weak` serialization is the whole feature**. Until the model can name an owned-but-aliased edge and say what the wire and the synthesizer do with it, you cannot serialize a `Shared<T>` field at all, cannot serialize a `Weak<T>` observer, cannot serialize a collection of handles, and cannot round-trip any object graph that uses reference counting. Decision #1 is the vocabulary question; everything else (the runtime ABI for handle extraction, the deserialize re-bind table, the eligibility rule) is downstream of settling it.

---

## 2. The key result

**Verdict: YES — the existing `$id`/`$ref`/inline wire FORMAT suffices. What is new is SYNTHESIS LOGIC, not a wire construct.**

The one-line reason: **identity de-duplication is done by raw pointer, and the de-dup table is agnostic to whether the edge owns or borrows.** The runtime tag table (`bz_binser_graph_get_tag` / `assign_tag`, `bz_binder_graph_register_tag` / `resolve_tag`) is a flat `void**` keyed on pointer equality — it stores no type, no field name, no ownership-strength flag (bezel_runtime.cpp:8102-8123, :7480). The wire likewise carries no ownership byte: `BZ_SER_INLINE` is `0x01 + LE32 tag`, `BZ_SER_REF` is `0x02 + LE32 tag`, and that is the whole alphabet (bezel_runtime.cpp:8131-8171, :7449-7450). "First occurrence inline, every later occurrence a `$ref`" is therefore expressible for an owned-aliased edge using exactly the tokens that already exist.

Stronger: the **dynamic** "is this pointer already tagged?" branch that an aliased owned edge needs is *already implemented and already running* on the `owned` path in graph/cyclic mode — `get_tag` first; if 0, `assign_tag` + `write_inline_tag` + recurse; if non-zero, `write_ref_tag` (LlvmBackend_Serialization.cpp:1561-1603). The code even carries a comment that this aliased-while-owned branch "shouldn't happen for tree-structured owned edges, but handle it gracefully" (:1565). For `Shared<T>` that defensive branch stops being defensive and becomes the *normal* path. The skeleton is in place; what is missing is the classification that routes `Shared`/`Weak` fields into it and the read-side re-binding. No new wire token.

One precision, carried forward to §3 and §6: this verdict is about the wire *format*. There is also a **prerequisite spec-eligibility gate** — `Shared<T>` and `Weak<T>` are not in §9.8's closed enumeration of serialization-eligible component types (§9.8:997). That gate must be opened by spec amendment before any of this synthesis can be reached. It is not a wire question; it is owner work, flagged in §6.

---

## 3. The new synthesis logic

Three sites change. None of them touches the wire alphabet.

### 3a. `collectClassComponents` — classification goes from two categories to four

Today every field is either `isOwned` (the `owned` modifier string scanned off the AST → inline; LlvmBackend_Serialization.cpp:140-144) or `isBackEdge` (a non-owned class-typed field → static `$ref`; :163). Two categories are added, keyed on the field's `TypeRef` being `Shared<T>` or `Weak<T>` (today both are entirely absent from the serializer — grep confirms zero references):

- **`Shared<T>` → identity participant.** Owned, but *cannot be statically designated the canonical inline position*, because any of the N `Shared` fields aliasing one value is equally entitled. Routed to the dynamic get-tag branch.
- **`Weak<T>` → `$ref`-only.** Non-owning observer (§28.13: "a non-owning observer minted from a handle"); it can never be the canonical body, so it is always a back-reference — structurally the same as the borrow back-edge, but reconstructed differently (see 3c).

The existing two categories are untouched: `owned` keyword still means static inline (unaliased spanning tree), plain borrow still means static `$ref`.

### 3b. Serialize per field

**`Shared<T>` field** — run the existing dynamic branch (the :1561-1603 template) but **key the tag table on the underlying `T*`, not the handle pointer.** This is the single most important synthesis subtlety, and it is where the naive reading of the hypothesis fails. `Clone()` produces a *new* `Shared<T>` handle object at a *different* address that points (via the shared control block) to the *same* underlying `T`. If the de-dup key were the handle pointer, two clones would look distinct, `T`'s body would be written twice, and the `$ref` collapse would never fire — corrupt output that silently violates the aliasing contract. So: extract the underlying `T*` (via `Get()` / a compiler-internal control-block accessor), call `get_tag(underlyingT)`; if 0 → `assign_tag` + `write_inline_tag` + recurse into `T`'s body; else → `write_ref_tag`.

**`Weak<T>` field** — always `$ref`. `TryUpgrade` to extract the underlying `T*` (a `Weak` cannot `Get()` without a live strong handle), then `get_tag(underlyingT)`; if non-zero → `write_ref_tag`; if 0 → `SerializationException` (the target was never inlined through any owning edge in this walk — the `Weak` points outside the serialized graph, or is dead). This mirrors the existing back-edge error path exactly (:1494-1511).

**The inline-before-ref ordering requirement — and whether it is guaranteed.** For the `$ref` to resolve, the target must have been tagged (inlined) *before* the referencing edge is reached. For plain borrows this is *guaranteed by the language*: a borrow's owner outlives it and is reached first in the owning-tree DFS (§9.8:1015). **For `Shared`/`Weak` it is NOT guaranteed.** There is no single "the owning edge" to put first in declaration order; the canonical inliner is discovered dynamically (first `Shared`/`owned` field to reach the value wins). The adversarial case (§4) is: class A has a `Weak<B>` field, class B has a `Shared<A>` field; serialize A first, and A's `Weak<B>` fires `get_tag` on a `B` not yet seen → tag 0 → exception. The dynamic check catches it correctly, but it converts "a valid object graph" into "serializes-or-not depending on traversal order." The honest resolution is a **new static eligibility rule** (a `Weak<T>`'s target must be reachable first through a `Shared<T>`/`owned` field — a DAG-ordering invariant analogous to owner-before-borrower), so the failure is a clean compile-time/declaration-time diagnostic rather than a late, order-dependent runtime fault. That rule is owner spec work (§6); the dynamic guard is already present as the floor.

### 3c. Deserialize — re-bind by the *receiving* field type

`resolve_tag` hands back a raw pointer. The synthesized per-field code (the field type is static at every call site — synthesis emits per-field code, not a generic "read a node" routine) re-binds three ways:

- **`Shared<T>` field** → produce a new owned `Shared<T>` by `Clone()` on the handle registered for that identity (increment the shared count).
- **`Weak<T>` field** → `Downgrade()` on the registered handle to mint an owned `Weak<T>`.
- **plain borrow field** → the existing path: store the resolved raw pointer as a borrow anchored to the constructed owning graph (no refcount op — matches today's behavior at :1937-1951, :9383-9388).

This forces the one real **runtime-ABI delta**: the deserialize identity table today stores raw `T*` (`register_tag` stores `childPtr`). For `Shared` inline targets it must store (or make recoverable) the **`Shared<T>` handle**, because `Clone()`/`Downgrade()` operate on handles, not bare pointers. Either extend the table entry to `{raw T*, Shared<T> handle | null}` or run a parallel handle table. This is the single largest implementation decision gated on decision #1, but it is a table-format choice, **not a wire-format choice** — flagged in §6.

---

## 4. The adversarial findings

The skeptic ran eight cases. For each: does the existing wire suffice, or is a new construct needed? Honest verdict per case.

| # | Case | Wire suffices? | Note |
|---|------|----------------|------|
| 1 | **`Shared`/`Weak` wire-indistinguishability** | **Yes (wire), with a caveat** | Both produce `$ref` after first reach; the wire carries no strong/weak bit. Reconstruction is driven by the *reader's* field type. Sound for same-schema round-trips. **Caveat:** under schema evolution (a field's type flips `Shared`↔`Weak`), an old `$ref` is silently re-read as the wrong handle kind — a lossless-round-trip gap, *not* a missing wire construct. See §5. |
| 2 | **Which-edge-inlines non-determinism** | **Yes** | Within a class, fields are walked in declaration order (§9.8:1013), so the first `Shared` field inlines deterministically. Across two unrelated owner classes, *which* reaches the value first is DFS-order-dependent — but the per-serializer identity table makes any single traversal self-consistent and round-trippable. Byte-for-byte reproducibility wants a canonical traversal policy; that is a *stability* concern, not a wire gap. |
| 3 | **Mixed `Shared` + borrow to one value** | **Yes** | `Shared` field inlines (first reach via the owning edge); the borrow field emits the existing back-edge `$ref`. On read, the borrow must route through the reconstructed handle's `Get()` rather than a direct `T*`. Synthesis-coordination cost; existing wire fully sufficient. |
| 4 | **Collections of `Shared` (`Array`/`OwnedArray<Shared<T>>`)** | **Yes** | If eligibility admits `Shared`, the existing `BeginArray`/element/`EndArray` framing + per-element dynamic inline-or-`$ref` handles element-level identity de-dup. Two elements aliasing one `T`: first inline, rest `$ref`. No new construct. |
| 5 | **Polymorphic `Shared<Base>` holding `Derived`** | **Yes — already specified** | §9.8:1070 already fixes the `$type`+identity composition: first occurrence carries identity tag + `$type` + body; every later occurrence is a *pure* `$ref` (no `$type`, no body). Implemented already for the borrow case (`emitPolyClassSerialize`/`Deserialize`, :1115-1327). No new construct. |
| 6 | **Nested `Shared` (`Shared<T>` where `T` has `Shared<U>` fields; clone chains)** | **Yes for the wire — but ONLY if tagging is by underlying-value identity** | This is the case that *kills the naive reading*. Clones are distinct handle objects over one `T`. Keyed on handle pointer → `T` written twice, aliasing lost, corrupt. Keyed on underlying `T*` (the §3b rule) → correct. The fix is a synthesis divergence (tag by value, not handle), **not** a wire token. The recursion into `T`'s own `Shared<U>` fields applies the same rule transitively. |
| 7 | **`Weak` to an already-dead value** | **Yes** | The wire is a fixed-point snapshot: a `$ref` always points at a value present in the serialized data. Liveness is never a *read* problem. At *write* time, a dead `Weak` (`TryUpgrade` false) means the graph is ill-formed → `SerializationException` (the §3b guard). No wire construct needed to represent "weak ref to a live value." |
| 8 | **Entry node is itself a `Shared` participant** | **Yes** | The root is simply the first object serialized; its identity tag is assigned on first reach (by underlying-value identity per §3b). On read, the root `Deserialize` returns an owned `Shared<T>`. The spec adds no root marker for any other case and needs none here. |

**The honest bottom line of the adversarial pass:** *no case requires a new wire construct.* Case 6 requires a non-trivial **synthesis** change (tag by underlying value, not by handle), and case 1 leaves a **schema-evolution** gap that is a *format-richness policy choice* (§5), not a missing token. The skeptic's sharpest correction to the original hypothesis: "just do dynamic inline-vs-`$ref`" is true but *underspecified* — the identity key must be the underlying value pointer, and the deserialize table must carry handles, not raw pointers.

---

## 5. Minimal vs self-describing wire — the central fork

The fork is whether the wire stays **field-type-driven (minimal)** or becomes **self-describing** by carrying strong/weak/borrow intent in the `$ref` payload.

**Minimal (recommended).** The wire keeps only `BZ_SER_INLINE`/`BZ_SER_REF`; the receiving field's declared Bezel type drives `Clone`/`Downgrade`/borrow on read. No wire change, no ABI revision to the wire, existing readers keep working. Sound for **same-version, same-domain round-trips** — which is the actual scope of this feature.

**Self-describing.** A 1-byte sub-marker in the `$ref` payload (e.g. `BZ_SER_REF_STRONG 0x02 0x01`, `BZ_SER_REF_WEAK 0x02 0x02`, `BZ_SER_REF_BORROW 0x02 0x00`). A Bezel reader would *ignore* it (it already knows the field type); its only value is to a **cross-version migration tool or a cross-language reader** that wants to validate or migrate `$ref` intent — exactly the schema-evolution gap from §4 case 1. The cost: a wire-format revision (existing `0x02` readers break), a new runtime ABI token, and — decisively — it would be the **first wire-visible ownership annotation** in the format.

**Recommendation: MINIMAL, grounded in the §5.19 placement principle.** Rationale §5.19 (line 577) says a knob belongs on the *type* when it is part of the type's wire contract, and on the *operation* when it is a property of a particular read/write. The strong/weak distinction is neither a structural property of the wire nor an operation knob — it is a property of *each field's declared type at each use site*, which the synthesizer already has statically. Encoding it on the wire is the same category of redundancy the spec **deliberately declines** for the existing `owned`/borrow back-edge (the wire does not mark borrow-vs-owned today either). The contrast with `$type` is the discriminator: `$type` is on the wire because the deserializer *literally cannot dispatch without it* (§9.8:1072 treats it as structural, never field-customized); a strong/weak marker is *not* needed for dispatch, because the field type is statically known. That asymmetry is the principled grounds for declining the self-describing variant.

Reinforcing the recommendation: `Shared<T>` is **domain-local and not sendable** for non-synchronized `T` (§28.13). A cross-language receiver has *no semantic correspondent* for `Shared`/`Weak` at all, so the self-describing variant's headline benefit (cross-language round-trip) is largely undefined in the first place. The schema-evolution robustness it buys is real but narrow, and it is better addressed by a schema-versioning protocol than by minting the format's first ownership byte.

So: **minimal wire, type-driven reconstruction.** The self-describing variant is left explicitly as the owner's call (§6) for the day cross-version diagnostic richness is judged worth the precedent.

---

## 6. What's resolved vs owner sub-decisions

**Resolved (evidence is decisive):**

- **No new wire construct is needed.** `BZ_SER_INLINE`/`BZ_SER_REF` + LE32 tag suffices for every adversarial case (§2, §4). Identity de-dup is by pointer, agnostic to owning/borrowing (bezel_runtime.cpp:8102-8123).
- **The change is synthesis logic at three sites:** `collectClassComponents` classification (two new categories), serialize (dynamic get-tag for `Shared`, always-`$ref` for `Weak`), deserialize (re-bind by receiving field type: `Clone`/`Downgrade`/borrow) — §3.
- **The identity key for a `Shared` field must be the underlying value pointer (`Get()`), not the handle pointer.** Keying on the handle is a correctness bug (case 6): clones are distinct handle objects over one value. This is the single load-bearing synthesis invariant.
- **Wire richness: minimal, field-type-driven.** Grounded in §5.19's placement principle and the spec's existing refusal to mark owned/borrow on the wire (§5).
- **The `Weak`-reached-before-`Shared` ordering hazard is *caught* today** by the `tag==0 → SerializationException` guard. The floor is sound.

**Owner sub-decisions (the residue — do not resolve autonomously):**

1. **Spec eligibility amendment.** `Shared<T>`/`Weak<T>` are *not* in §9.8's closed component-type enumeration (§9.8:997). The closed set is closed by design; admitting them — directly in the enumeration vs. via a new graph-shape clause — is a spec change requiring a rationale entry. **This gates everything; it is the true upstream prerequisite.**
2. **`Weak` ordering: static rule vs dynamic-only.** Add a static eligibility rule (a `Weak<T>` target must be reachable first via a `Shared`/`owned` field — DAG ordering analogous to owner-before-borrower) for a clean compile-time diagnostic, or rely on the existing late runtime exception? Spec-design question (§3b, §4 case-by-implication).
3. **Deserialize identity-table format.** Extend entries to `{raw T*, Shared<T> handle | null}` or run a parallel handle table, so `$ref` resolution can `Clone()`/`Downgrade()`. Changes the `bz_binder_graph_register_tag` ABI. Largest implementation decision gated here (§3c).
4. **Does a class with `Shared<T>` fields *require* `serialized graph` (vs `serialized tree`)?** `Shared` aliasing lives outside the owning spanning tree; a `tree` assertion may be violated dynamically. Is `tree` still admissible if the programmer asserts no two handles alias one value (checked dynamically as today), or must such classes declare `graph`? Spec-scoping question.
5. **Runtime-rep prerequisites for the synthesis:** the control-block layout / underlying-pointer offset for `bz_shared_get_underlying`; whether the control block is findable from the inner pointer (for `Weak` weak-count ops); whether `Clone()` is well-defined on an in-progress handle during cyclic two-phase construction (:1924-1939); and the nullable-`Shared` null guard before `get_tag`. Implementation-internal, but each must be answered before the serialize branch can be written.
6. **Self-describing wire — held in reserve.** Recommended *against* for this revision (§5). Owner's call to revisit if cross-version/cross-language diagnostic value is later judged worth minting the format's first wire-visible ownership annotation.
