# Proposal 0036 (RFC) — Demote owning inline arrays (and the move-only value-type category) to an Extended feature

| | |
|---|---|
| **Status** | **accepted (owner, 2026-07-12)** — graduation in progress this run. *(prior: draft (RFC))* |
| **Tracking issue** | [#1551](https://github.com/s8lap/bezel-lang/issues/1551) |
| **Target module(s)** | None (spec-only). Appendix [D.2](../Bezel_Language_Spec.md#sec-extended-features) (one new feature row + the closing "Everything not listed is Core" paragraph), [D.3](../Bezel_Language_Spec.md#sec-feature-dependencies) (one dependency bullet). No stdlib surface. No semantics change anywhere. |
| **Written against** | spec/stdlib **v0.179**. One touched clause (§28.9's move-only capture rule) sits in text the v0.180 capture train (landing) revises; its location must be re-verified when that train lands (§8, Q4). |
| **Design character** | **RFC** — a conformance-partition question, not a semantics question. The recommendation is argued; the owner decides. Nothing here lands in the spec this run. |

## 0. Summary

Move owning inline arrays — `[N owned]T` / `[N owned]T?` ([§15.1.1](../Bezel_Language_Spec.md#sec-owning-inline-arrays)) — and with them the entire *move-only value type* category ([§15.3](../Bezel_Language_Spec.md#sec-move-only-value-types)) out of Core and into a new Appendix D.2 Extended feature, structured as clause-level carve-outs on the model of D.2's existing FFI-clause and buffer/span-clause rows.

This is a **conformance-staging** change only. No rule changes meaning; no program changes behavior; the reference compiler (Full-level) is unaffected. What changes is what a *Core* conformance claim obligates an implementation to build — and what the Core test partition exercises.

Two things this proposal is **not**:

- **Not a deprecation.** The feature stays fully normative at the Full level. Rationale [§5.14](../Bezel_Design_Rationale.md#sec-inline-arrays-and-array-kind-split)'s self-hosting story for the owning collections *depends* on `[N owned]T?`, and this proposal reinforces rather than undermines it (§3.3).
- **Not the "OwningBuffer subsumes it" argument.** It doesn't, and an earlier draft of this argument died on exactly that point — see §3.3 for the correct implementability argument and the explicit refutation of the tempting wrong one.

## 1. The category today

Owning inline arrays are the sole member of a type-system category of their own. [§15.3](../Bezel_Language_Spec.md#sec-move-only-value-types) (v0.179):

> The single move-only value type kind in this specification is the owning inline array (`[N owned]T` or `[N owned]T?`, see [§15.1.1](#sec-owning-inline-arrays)). Other value types — primitives, records, and inline arrays without the `owned` modifier — remain copyable.

The category has exactly **one syntactic entry point** — the optional `owned` marker inside the inline-array type form (Appendix A.5):

```
InlineArrayType ::= "[" ConstIntExpr "owned"? "]" Type
```

Everything else about the category is downstream of a type having been formed with that marker: moved-from state on a *value* type, move-not-copy assignment, the whole-value-reassignment leak discipline, the per-slot restoration rules, the instantiation-time copy-operation rejection inside `where T: value` generic bodies, the capture-as-borrow rule, the property/indexer prohibition, the boxing prohibition, the sendability exclusion. A Core implementation that rejects the `owned` marker at the type-form site — with D.1's diagnostic naming the construct and the feature — renders every one of those rules unreachable. Appendix [D.1](../Bezel_Language_Spec.md#sec-the-levels):

> An implementation conforms at the Core level when it implements every normative rule of this specification *except* the Extended features listed in D.2, and *rejects* — with a diagnostic naming the construct and the feature — any program that uses an Extended feature it does not implement.

No other candidate for demotion has a boundary this clean: one grammar token gates the entire category.

## 2. Why Core placement was never decided

Appendix D was created in v0.132 (#602 leg C; rationale [v0.131→v0.132 changelog](../Bezel_Design_Rationale.md#sec-v0-131-to-v0-132-changelog)). Its Extended list at birth — Events, Asynchronous programming, Serialization synthesis, FFI, Buffers and spans, Sequence operations — was assembled from the features that were *obviously* separable. Owning inline arrays were never considered for either side: nothing in the v0.132 changelog, in any later changelog through v0.179, or in the rationale records a decision to place move-only value types in Core. Their Core status is a **default**, not a decision — they landed in Core because "everything not listed is Core."

There is likewise **no recorded decline** of demotion to engage: this question has not been asked before. The nearest recorded decisions cut the other way or are orthogonal:

- Rationale §5.14 records why the category exists (it is "foundational — move semantics for owning slots are unavoidable" *for the self-hosting substrate*) and deliberately scopes it tightly ("only owning inline arrays exhibit it"). That is an argument about the *Full* language, and §3.3 below shows the self-hosting substrate is already Extended.
- #1334 (closed) endorsed the `[N owned]T` *spelling* as part of the two-axis ownership model review; it says nothing about conformance level. No conflict.

## 3. The case for demotion

### 3.1 The category is the affine model's only surviving carve-out

Core's headline ownership claim, [§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows):

> These rules ensure that every owned class instance is disposed **exactly once** at runtime and that no owned reference is silently leaked (with drop-on-overwrite, [§28.7](#sec-reassigning-owned-fields), the claim has no carve-outs).

And [§28.7](../Bezel_Language_Spec.md#sec-reassigning-owned-fields) (v0.161): "with overwrite covered, the §28.3 claim — *every owned value is disposed exactly once on every path* — holds with no carve-outs." A few paragraphs later, the same section carves one out:

> **Move-only inline arrays:** whole-value reassignment of `[N owned]T` values keeps the [§15.3](#sec-move-only-value-types) manual discipline for this revision — the slot-wise disposal interacts with the open #747 design and is deferred with its own tracking issue.

The tracking issue is [#1385](https://github.com/s8lap/bezel-lang/issues/1385) (open). The retained manual discipline, [§15.3](../Bezel_Language_Spec.md#sec-move-only-value-types), is a *documented leak*:

> **Whole-value reassignment** `arr := newValue;` on a move-only local or field: the assignment is plain — the compiler does not insert automatic cleanup of the previous slots. If the LHS holds any slot that is not in moved-from state at the assignment site, those slots leak.

So today the "no carve-outs" sentences are textually false, and the one construct that falsifies them is this category. (The immediate text fix — amending §28.3's parenthetical and §28.7's v0.161 paragraph to name the §15.3/#1385 exception — is **not** this RFC's ask; it is landing separately in this run's small-rulings amendment train, and is correct whether or not this demotion is adopted.)

Demotion relocates the exception to where the partition already puts semantic complexity: after it, the Core level's affine claim is exactly the clean two-word story — *disposed exactly once* — with the one open design debt (#1385) living inside an Extended feature alongside the machinery (#747-family slot disposal) it interacts with.

### 3.2 The category is where value types stop being simple

Everywhere else in Core, value types are the *easy* case: copy on assignment, no ownership state, no lifetime obligations beyond borrow-bearing records' source tracking. Move-only value types are the single place a Core implementation must additionally build:

- **moved-from state on value-typed locals, fields, and slots**, with read-of-moved-from rejection ([§15.3](../Bezel_Language_Spec.md#sec-move-only-value-types));
- **per-slot move tracking and the field-restoration discipline** for `move arr[i]` on inline storage ([§15.1.1](../Bezel_Language_Spec.md#sec-owning-inline-arrays), [§28.8.1](../Bezel_Language_Spec.md#sec-the-move-expression));
- **instantiation-time copy-operation auditing of generic bodies**: under `where T: value`, "for move-only inline arrays (§15.3), copy operations are replaced by moves, and any operation the generic body uses that requires copy semantics … is rejected at instantiation with a message naming the move-only type" ([§7.10](../Bezel_Language_Spec.md#sec-generics-and-nullability), [§14.2](../Bezel_Language_Spec.md#sec-constraints));
- **a third slot category in every storage-position dispatch**: class fields ([§28.5](../Bezel_Language_Spec.md#sec-owned-fields)), static fields ([§22.13](../Bezel_Language_Spec.md#sec-static-initialization-order)), iterator state machines ([§29.8](../Bezel_Language_Spec.md#sec-iterators-with-owned-parameters)), discard statements ([§17.13](../Bezel_Language_Spec.md#sec-the-discard-_)), captures ([§28.9](../Bezel_Language_Spec.md#sec-runtime-managed-types)).

The reference compiler's own history quantifies what this costs an implementer: the category produced a sustained family of unsoundness findings — #780 (borrow accepted into an owning slot), #781 (return doesn't materialize), #782/#806 (closure-captured arrays leak elements), #1016 (discard disposal), #1212 (static fields never disposed at exit) — plus the #747-audit findings (capture-boxing disposal crash; `move arr[i]` not nulling the slot). All of those are now fixed, but they were the cost of Core-by-default; a *future* Core implementation should get to decline that cost while still claiming Core.

### 3.3 Core loses nothing on implementability — the correct argument, and the wrong one refuted

**The wrong argument** (refuted here so it stays refuted): *"OwningBuffer<T> subsumes `[N owned]T`, so Core keeps the capability."* False twice over. First, `OwningBuffer<T>` **rejects move-only element types** (stdlib [§4.14](../Bezel_Standard_Library.md#sec-owned-buffers-and-spans): "Move-only value types (currently only the owning inline arrays `[N owned]T` and `[N owned]T?` …) are also not admitted in v0.102 — the slot semantics for buffers of move-only types require additional design work and are deferred"). Second, the two forms are *complements*, not substitutes: `OwningBuffer<T>` exists precisely because `[N owned]T?` requires compile-time N — stdlib §4.14 states the buffer is compiler-known for one reason, "**allocation of N slots where N is a runtime constructor parameter**. There is no user-level Bezel syntax for 'a class field holding a runtime-determined-size array of slots'; inline arrays `[N owned]T?` and `[N]T` require compile-time N." Rationale [§5.14](../Bezel_Design_Rationale.md#sec-inline-arrays-and-array-kind-split) confirms the pairing: "the intrinsic surface of the entire owning-collection family reduces to one well-scoped primitive (`OwningBuffer<T>`) plus the inline-array form (`[N owned]T?`); every resizable owning collection composes over these in pure Bezel."

**The correct argument:** self-hosting the owning collections is *already* a beyond-Core story, so demoting `[N owned]T` costs Core conformance nothing. The self-hosting substrate is `OwningBuffer<T>` **plus** `[N owned]T?` — and `OwningBuffer<T>` already lives in the Extended **Buffers and spans** feature (D.2). A Core-only implementation therefore ships its owning collections (`OwningArray<T>` et al., which *are* Core stdlib) as intrinsics **regardless** of where `[N owned]T` sits: with half the substrate absent, the pure-Bezel composition of rationale §5.14 is not available at the Core level today, and moving the other half to Extended changes nothing about that. The implementability story the category was created for is a **Full-level** story, and at the Full level it remains fully intact — indeed more coherent, with both halves of the substrate on the same side of the partition.

### 3.4 Nothing composes over the category from Core

No standard-library type stores, accepts, or returns a move-only value. The stdlib's only mentions of `[N owned]…` are *exclusions* (`OwningBuffer`/`Span`/`ReadOnlySpan` reject move-only T; owning collections take class/interface T only) plus one reflection kind-predicate note. The FFI boundary excludes them ([§32.5](../Bezel_Language_Spec.md#sec-ffi-compatible-types): "Owning inline arrays (`[N owned]T?`, `[N owned]T`) are not admitted"). Records cannot contain them ([§9.7](../Bezel_Language_Spec.md#sec-records-and-disposal)); properties and indexers cannot be typed by them ([§23.5](../Bezel_Language_Spec.md#sec-properties-and-indexers-restrictions)); boxing rejects them ([§8.6](../Bezel_Language_Spec.md#sec-boxing)); sendability excludes them as bare values ([§30.14](../Bezel_Language_Spec.md#sec-sendable-types)). The category is a leaf: cutting it strands nothing else.

## 4. What Core programs lose

**Semantics: nothing.** Plain inline arrays `[N]T` / `[N]T?` (value-typed and class-typed borrow slots) stay Core untouched — the demotion gates only the `owned` slot marker. A Core program that needs a fixed-size group of owned objects uses the Core owning collection:

```bezel
// Extended (this proposal): inline owned slots, no heap allocation for the container
var pool: [16 owned]Connection?;

// Core: heap-allocated owning container, same ownership semantics (stdlib §4.5)
using var pool := new OwningArray<Connection>(capacity := 16);
pool.Add(Connect("primary"));          // takes owned
var first := pool[0];                  // borrow of slot
using var taken := pool.PopAt(0);      // transfer out; container disposes the rest
```

**Honestly stated, one thing is lost: the zero-allocation form.** `[16 owned]Connection?` embeds sixteen owned slots inline in a class's layout or a stack frame with no container allocation; `OwningArray<Connection>` is a heap object. That is a real difference — but it is an *optimization and self-hosting substrate*, not an expressiveness gap, and it is precisely the kind of difference the partition already tolerates: a Core implementation also lacks `OwningBuffer<T>`, `Span<T>`, and every other zero-copy tool of the Buffers-and-spans feature. The programs Core is for do not stop being writable; they allocate one container object where a Full implementation could inline the slots.

## 5. The proposed D.2 row — clause-level carve-outs

D.2 already contains two rows that name *clauses* rather than whole sections: FFI ("the FFI clauses of [§15.1.3](#sec-inline-array-ffi)") and Buffers and spans ("the buffer/span clauses of [§28.11](#sec-collection-element-ownership)"). This demotion follows that precedent exactly. Proposed row:

| Feature | Specification sections | Standard-library sections |
|---|---|---|
| **Move-only value types (owning inline arrays)** | [§15.1.1](../Bezel_Language_Spec.md#sec-owning-inline-arrays) and the `owned` slot marker of Appendix A.5's `InlineArrayType`; [§15.3](../Bezel_Language_Spec.md#sec-move-only-value-types); the move-only discard clause of [§17.13](../Bezel_Language_Spec.md#sec-the-discard-_); the owning-inline-array field clauses of [§28.5](../Bezel_Language_Spec.md#sec-owned-fields) (including the double-marking rejection) and [§22.13](../Bezel_Language_Spec.md#sec-static-initialization-order); the move-only clause of [§28.7](../Bezel_Language_Spec.md#sec-reassigning-owned-fields); the move-only rows of [§28.11](../Bezel_Language_Spec.md#sec-element-slot-policy)'s element-category tables; the owned-slot clauses of [§29.2](../Bezel_Language_Spec.md#sec-desugaring-of-for)'s inline-array desugaring and the move-only paragraph of [§29.8](../Bezel_Language_Spec.md#sec-iterators-with-owned-parameters); the move-only instantiation checks of [§7.10](../Bezel_Language_Spec.md#sec-generics-and-nullability)/[§14.2](../Bezel_Language_Spec.md#sec-constraints) (`where T: value`) | — |

(Deliberately **not** in the row: [§14.4](../Bezel_Language_Spec.md#sec-owned-as-a-generic-argument). It carries no move-only instantiation check — the `where T: value` copy-audit lives entirely in §7.10/§14.2 — and its no-`owned`-through-generic-arguments model and owning-collection restrictions are Core-critical, load-bearing for the whole owning-collections story. Its one move-only mention is the `OwningBuffer<T>`/`Span<T>`/`ReadOnlySpan<T>` *rejection* of move-only T — a descriptive exclusion on the Buffers-and-spans side, handled below with the other tendrils.)

Supporting edits, mostly mechanical — with one honest caveat: unlike the FFI precedent, which carved whole clauses, three of the row's carves are *within-sentence or within-table* sub-clauses — [§28.5](../Bezel_Language_Spec.md#sec-owned-fields)'s double-marking parenthetical, [§28.11](../Bezel_Language_Spec.md#sec-element-slot-policy)'s move-only table rows, and [§29.2](../Bezel_Language_Spec.md#sec-desugaring-of-for)'s desugaring, whose single opening sentence covers plain and owning slot forms together ("including the borrow-bearing and owning slot-marker forms") — and need surgical sub-clause tagging rather than a whole-clause annotation:

- **D.2's closing paragraph** ("Everything not listed is **Core**: … the entire ownership system of [§28] (except §28.12 and §28.13) …") adds this row's §28 clauses to the exception list.
- **D.3** gains one bullet: *Move-only value types* depend only on Core. Buffers and spans does **not** depend on it (`OwningBuffer<T>` rejects move-only T); Asynchronous programming *references* it (§30.14 sendability rule 2's owning-inline-array field alternative; the async-frame move-only-parameter clause of §30) but does not depend on it — those clauses are normative only for implementations of both features, the same rule D.3 already states for Core-section references into Extended sections, generalized to Extended→Extended.
- **No action needed** for clauses already inside other Extended features: §30.14 rule 2 and the async-frame clause sit inside Extended Asynchronous programming; §32.5's exclusion sentence sits inside Extended FFI. They stay where they are.
- **Descriptive tendrils stay as prose.** The category is *mentioned* in many more places — §3.2's value-type taxonomy note, §8.3's equality/hash row, §8.6's boxing exclusion, §9.7's record exclusion, §17.11's `default()` admission, §23.5's property rejection, §28.1's slot-marker bullet and §28's obligations-summary row, §28.8.1's application to owning-inline-array slots (the `move` expression itself stays Core for owned fields and owning-collection slots; only its inline-array-slot use becomes unreachable), §28.9's capture-as-borrow clause, §28.10's `using var` clause, §14.4's `OwningBuffer`/`Span`/`ReadOnlySpan` move-only rejection (a Buffers-and-spans-side exclusion), Appendix C.1/C.3/C.4's invariant rows, stdlib §4.14's exclusions, the reflection kind predicates. These are exclusions and cross-references, not implementable surface; under D.1 they become vacuous in a Core implementation (the types they speak of cannot be formed) and need no textual change, exactly as §15.1.3's FFI prose needed none when the FFI clauses were carved. The D.2 row lists the clauses that *carry obligations*; D.3's existing reference rule covers the rest.

Because the category's one syntactic gate is the grammar marker, the **Core-conforming rejection** is a single check: an `InlineArrayType` with the `owned` marker is rejected at the type-form site with a diagnostic naming owning inline arrays and the feature. Every downstream rule is then unreachable. This is the cheapest reject-obligation of any D.2 row.

## 6. Compiler cost — sized honestly (mostly *not* a saving for this implementation)

An earlier form of this argument claimed the demotion spares the reference compiler the category's implementation cost. **That claim is stale and is withdrawn.** The reference compiler has already landed most of the machinery: scope-end slot disposal works (probe: exit 0), and #780, #781, #782, #806, #1016, and #1212 are all closed. (One citation hygiene note: "#747" in earlier working notes names a session-memory design audit; the GitHub issue #747 is the closed R42 discovery omnibus that merely *contained* the original finding. This proposal cites the individual issues.)

What the demotion actually saves, and for whom:

- **For the reference compiler (Full): nothing.** It keeps the category; the only live work here is the un-landed tail — [#1385](https://github.com/s8lap/bezel-lang/issues/1385) whole-value drop-on-overwrite and the missing §15.3 warning (§7) — and that work exists whether or not the demotion is adopted.
- **For future Core implementations: the whole category.** Moved-from state on value types, per-slot move tracking, restoration analysis, the generic-body copy-audit, and the third slot category in every storage dispatch — replaced by one grammar-site rejection.
- **For the test suite and teaching material now:** the partition D was created to serve ("the test-suite and teaching material can follow the same boundary"). On the order of fifty test cases across ~10 files move to the Extended bucket; the Core conformance bucket stops exercising the one construct whose disposal story is still open.

## 7. Interaction with #747 / #1385, and the filed warning bug (#1549)

**#1385 should be resolved regardless of this ruling.** Its v0.161 deferral reason — "the slot-wise disposal interacts with the open #747 design" — has substantially expired: the slot-disposal machinery it was waiting on (scope-end disposal, capture-boxing interaction, static-field disposal) has landed. Defining drop-on-overwrite for `[N owned]T` whole-value reassignment (dispose each non-moved-from slot of the displaced value, reverse slot order, then store — #1385's own sketch) is now an incremental extension of shipped machinery, not a new design. If adopted alongside this demotion, the affine claim becomes carve-out-free at *both* levels: Core by exclusion, Full by #1385's fix. The two are independent and compatible; this RFC recommends both.

**The companion compiler bug is filed: [#1549](https://github.com/s8lap/bezel-lang/issues/1549)** (*"move-only whole-value reassignment leaks silently — §15.3's mandated provably-non-moved-from warning is not implemented"*). This proposal's adversarial verification found that the reference compiler emits **no diagnostic at all** on provably-leaking move-only whole-value reassignment, though §15.3 mandates one — "The compiler emits a warning when reassignment is reachable from a state where some slot is provably non-moved-from; the warning identifies the slots at risk and points at the standard discharge mechanisms." Live probe: whole-value reassignment of a `[2 owned]Cell?` local with both slots live compiles clean, runs, and leaks both cells (leak-counter exit 2) with zero diagnostics. The documented leak is thus fully *silent* in practice — spec says warn-and-leak; the implementation just leaks. #1549 is independent of both #1385 and this ruling; a Full implementation owes the fix regardless of the demotion.

**Also independent (already routed):** the §28.3 parenthetical and §28.7's "no carve-outs" sentences are textually false while #1385 is open; the one-line fixes naming the §15.3/#1385 exception are landing separately in this run's small-rulings train, not here.

## 8. Migration

**None.** Demotion changes conformance levels, not semantics:

- No program changes meaning; no source edit is ever required.
- The reference compiler claims Full and implements everything either way.
- A hypothetical Core implementation gains a *license* (reject the marker, feature-named) it did not have; nothing it previously accepted becomes invalid because no Core implementation exists yet.
- Docs churn is one appendix (D.2/D.3), a changelog entry, and optionally a "(Extended: move-only value types)" tag at the carved clauses, matching how carved FFI/buffer clauses read today.
- Conformance-suite churn: retag the move-only tests (order of fifty cases, ~10 files) into the Extended bucket.

## 9. Sizing

| Piece | Size |
|---|---|
| Spec edit (D.2 row + closing-paragraph exception + D.3 bullet + changelog) | Small — one appendix, no normative-rule rewording |
| Optional clause tags in §15/§17/§22/§28/§29/§14 | ~10 one-line annotations; mostly mechanical, except three within-sentence/within-table sub-clause tags (§28.5 parenthetical, §28.11 rows, §29.2's combined sentence — §5) needing surgical wording |
| Stdlib edit | None (no stdlib surface; §4.14's exclusion prose already correct) |
| Compiler work | None required (reference is Full). #1385 + #1549 (§7) are independent, recommended, and now incremental |
| Test suite | Retag ~50 cases / ~10 files to the Extended bucket |

## 10. Open questions for the owner

1. **Standalone row vs. folding into Buffers and spans?** Recommended: **standalone** ("Move-only value types (owning inline arrays)"). Neither feature depends on the other — `OwningBuffer<T>` rejects move-only T, and `[N owned]T` needs no buffer machinery — and D.1 diagnostics should name the feature the user actually tripped on. Folding would also falsely suggest a dependency the D.3 graph doesn't have. (The two are *jointly* the self-hosting substrate, which argues for them being at the same level — Extended — not in the same row.)
2. **Feature name.** "Move-only value types" (the §15.3 category, recommended, with the array form parenthesized) vs. "Owning inline arrays" (the only inhabitant). If user-defined move-only types ever arrive — rationale §5.14 explicitly leaves that door open — the category-named row is where they land, making that future cheaper rather than harder.
3. **Carve-out granularity.** This proposal lists obligation-bearing clauses in the row and leaves descriptive tendrils to D.3's reference rule (the FFI precedent). Alternative: exhaustively tag every mention. Recommended: the precedent; exhaustive tagging is churn without a conformance consequence.
4. **v0.180 coordination.** The §28.9 capture-as-borrow clause for move-only locals sits in capture/borrow text the v0.180 capture train (landing) revises; the carve-out list's §28.9 pointer must be re-verified against v0.180's section geometry before the D.2 row lands.
5. **Sequencing with #1385.** Resolve #1385 first (making Full carve-out-free), then demote? Or demote first? Recommended: either order works — they are independent — but both should happen; adopting *only* the demotion would leave the documented leak at the Full level with its "no carve-outs" prose fixed but its behavior unimproved.

## 11. Recommendation

Adopt the demotion as specified in §5: a standalone Extended feature "Move-only value types (owning inline arrays)," clause-level carve-outs per the FFI precedent, no semantics change, no migration. Independently: fix the §28.3/§28.7 "no carve-outs" prose now (already routed to the small-rulings train), fix the silent-leak warning gap [#1549](https://github.com/s8lap/bezel-lang/issues/1549) (§7, already filed), and resolve #1385 on its own merits — it is cheaper now than at its v0.161 deferral, and it closes the affine model's last documented leak for the Full level while this demotion closes it for Core.
