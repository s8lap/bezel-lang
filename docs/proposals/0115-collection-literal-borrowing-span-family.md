# Proposal 0115 — §15.4.1's borrowing form names the *owning* span family; it must name the borrowing one

| | |
|---|---|
| **Status** | graduated-v0.272 — superseded — graduated v0.272 (ruled under the owner's 2026-08-21 delegation: D1 accepted (`ReadOnlyArraySpan<T>`), D4 accepted as mandatory in the same train, D3 accepted (the unexercisable stack-allocation permission retires), D9 confirmed (sequences before [#3247](https://github.com/s8lap/bezel-lang/issues/3247)), and R3 **declined** — no `ReadOnlyArraySpan<T>.ToArray()` rides this signature fix); was under-review — draft (2026-08-21), authored against v0.270 for [#3310](https://github.com/s8lap/bezel-lang/issues/3310), which the v0.270 ruling investigation deliberately **routed to a proposal rather than ruled** (issue comment, 2026-08-21): the fix is a source-breaking signature change to a recognized protocol already scheduled for interface promotion, and the choice of replacement type is a real design question. |
| **Tracking issue** | [#3310](https://github.com/s8lap/bezel-lang/issues/3310) |
| **Target module(s)** | **spec** — [§15.4.1](../Bezel_Language_Spec.md#sec-the-user-defined-sequence-literal-protocol) (the borrowing form's signature, its *Lowering* clause, its empty-literal clause, and the `RingBuffer<T>` example: four lines), [§15.4](../Bezel_Language_Spec.md#sec-collection-literals) (one parenthetical, D4). **rationale** — [§5.16](../Bezel_Design_Rationale.md#sec-collection-literals-for-complex-collections), three lines. **stdlib** — **no change**: `ReadOnlyArraySpan<T>` is used exactly as [stdlib §4.15](../Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant) declares it. **`Bezel_For_Code_Generation.md`** — no change (it does not document the protocol; verified §5). **guides** — no change (none teaches the protocol; verified §5). **Appendix A** — untouched: no syntax, no token. **`DECLINED.md`** — three decline-index rows (this proposal's §8). |
| **Written against** | spec/stdlib/rationale/changelog **v0.270** (HEAD `fbe22213`). Every quotation was re-verified against that tree; every count in §5 was re-derived at that tree rather than carried from #3310's comment. |
| **Currency** | v0.270. **Quoted:** spec §3.6, §8.6, §9.1, §15.4, §15.4.1, §15.5, §28.11, §29.2; stdlib §4.4, §4.14, §4.15, §4.16; rationale §5.15, §5.16; changelog v0.107, v0.111. **Cited without quotation:** spec §11.8, §14.4, §28.3, §28.6, §28.9; stdlib §1, §4.5. **Re-verify before graduation:** spec §15.4.1 (rewritten in three places), §15.4's borrowing-family parenthetical, rationale §5.16's three `ReadOnlySpan` lines — and stdlib §4.15, on which this proposal's whole argument rests. |
| **Origin** | [#3310](https://github.com/s8lap/bezel-lang/issues/3310), filed by the v0.269 spec-example sweep as *"the `RingBuffer<T>` collection-literal protocol is well-formed only at value-typed `T`"*. The v0.270 investigation found **both candidates the issue named to be wrong** and re-scoped it: the seam is in neither §15.4.1's constraint surface nor stdlib §4.14's indexer, but in which span family §15.4.1's borrowing form names. |

*Reference convention: unprefixed §-references are **language-spec** references at v0.270; **stdlib**, **rationale**, **changelog**, and **process** references are marked as such. References to this document's own sections are always written "this proposal's §N". ▲ marks proposed normative text, landed nowhere. Every quotation is verbatim from the v0.270 tree; bold and italics inside quotations are the source's own. Source citations are `file:line` at `fbe22213`.*

---

## 1. Motivation

### 1.1 Bezel has two span families, and the protocol names the wrong one

The language has two collection families and, mirroring them, two span families. stdlib §4.15 states the correspondence and the prohibition in one sentence (`docs/Bezel_Standard_Library.md:3028`):

> `ArraySpan<T>` is the writable form; `ReadOnlyArraySpan<T>` is the read-only form. The split parallels `Span<T>` / `ReadOnlySpan<T>` ([§4.14]) but with `Array<T>` as the source instead of `OwningBuffer<T>`. The two span families coexist; **they are not interchangeable** (a method that takes `Span<T>` does not accept `ArraySpan<T>` and vice versa) **because the underlying storage models differ — `OwningBuffer<T>` owns its slots for class T, while `Array<T>` borrows them.**

So the two families are:

| Family | Storage | Slots at class `T` | Read-only view |
|---|---|---|---|
| **owning** | `OwningBuffer<T>` (stdlib §4.14) | *owned* `T?` — *"the buffer owns each non-null slot's referent"* (`Bezel_Standard_Library.md:2407`) | **`ReadOnlySpan<T>`** |
| **borrowing** | `Array<T>` (stdlib §4.4, spec §28.11) | *borrows* — *"store references that the caller continues to own"* (`Bezel_Language_Spec.md:7426`) | **`ReadOnlyArraySpan<T>`** |

§15.4.1's two factory forms are named by exactly this axis (`Bezel_Language_Spec.md:3016`, `:3021`):

> **Borrowing form.** For a type `C<T>` **whose elements are borrows** (the type does not own its elements):
> ```bezel
> public static method FromCollectionLiteral(elements: ReadOnlySpan<T>): owned C<T>;
> ```
> **Owning form.** For a type `C<T>` **that owns its elements** (`where T: class`):
> ```bezel
> public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned C<T>;
> ```

The owning form names the owning family's resizable collection. The borrowing form names the **owning** family's view. **A protocol whose elements are *defined* as borrows is declared to receive the view backed by storage that owns them.** That is not a nullability wart; it is an ownership-family mismatch, of exactly the kind stdlib §4.15's sentence exists to forbid.

The v0.111 changelog reveals the error being made (`docs/Bezel_Changelog.md:4185`): *"Two factory shapes are recognized — a borrowing form taking `ReadOnlySpan<T>` and an owning form taking `owned OwningArray<T>` — **mirroring Bezel's borrowing/owning collection duality**."* The stated intent is exactly right; the type chosen does not implement it.

**And the right type already existed.** `ArraySpan<T>` and `ReadOnlyArraySpan<T>` landed at **v0.107** — *four revisions before* the protocol (`docs/Bezel_Changelog.md:3943`: *"v0.107 adds `ArraySpan<T>` and `ReadOnlyArraySpan<T>`, span views over a heap `Array<T>` source"*), with the same entry already recording (`:3958`) that *"Both carry a borrow of the source `Array<T>`"*. So v0.111 was not choosing the only view available; it was choosing between two, by analogy to a language that has one. Rationale §5.16 records the analogy in its own words (`docs/Bezel_Design_Rationale.md:651`): C#'s builder class *"holds the static generic `Create<T>(ReadOnlySpan<T>)`"*, and the Bezel factory *"does exactly that"*. C#'s `ReadOnlySpan<T>` does not own its elements. Bezel's does. The name matched; the semantics did not, and the same-name-different-meaning trap is what the entry did not catch.

### 1.2 The compiler already disagrees with the spec, and the compiler is right

Codegen's user-type collection-literal arm lowers the borrowing form **onto Array-family storage with no element disposer** (`src/codegen/LlvmBackend_Setup.cpp:26313–26391`). The comment says so in the code's own words (`:26295`, `:26325`):

> Build a heap-allocated `bz_array_t<U>` as the span's backing storage, fill it with the n element values via `bz_array_add`, and pass it as the source field of the `ReadOnlySpan<U>` value.
> […] Borrowing form — **no element disposer** (the runtime header disposer is null). Mirrors the literal arm's call when name is `"Array"` (**a borrowing family**); the `OwningArray` `stampOwningElemDisposer` call happens only in the owning form below.

The span value is then built by `buildArraySpan` (`:26378`) — *"the same helper `Array<T>.AsReadOnlySpan()` and the `Span`/`ReadOnlySpan` constructors use"* (`:26308`). That helper is the `ReadOnlyArraySpan` constructor path: `src/codegen/LlvmBackend_Calls3.cpp:3052–3062` lowers `Array<T>.AsReadOnlySpan()` through the identical call, under a header (`:2818–2824`) that names the representation *"lower `Array<T>.AsSpan()`/`AsReadOnlySpan()` … `ArraySpan<T>`/`ReadOnlyArraySpan<T>`"* and states of `source` that it is *"the underlying `bz_array_t*` pointer (**borrowed; never freed by the span**)"*.

The lowering allocates no `OwningBuffer`, stamps no element disposer, and frees only the backing (`:26386–26391`). It is, in every observable respect, a `ReadOnlyArraySpan<U>`. **The implementation is already doing what the spec should say**, and the disagreement is confined to a single string comparison — `paramName == "ReadOnlySpan"` at `:26313`, and its sema twin `carrierName == "ReadOnlySpan"` at `src/sema/TypeResolution_Checks.cpp:1455`.

Per `CLAUDE.md` rule 1 the implementation agreeing with the spec is not evidence about the spec. But a *shipped lowering that cannot be written any other way* is evidence about the **type**: there is no `OwningBuffer<U>` the compiler could hand a borrowing factory without either taking ownership of the elements (which the borrowing form forbids) or constructing an owning buffer whose slots it then must not dispose (a buffer that violates its own §4.14 contract).

### 1.3 Three symptoms, one disease

#3310 reported three symptoms. All three are consequences of the family mismatch, and all three dissolve under the rename — which is what distinguishes a diagnosis from a patch.

**(a) The `T?` indexer.** At class `T`, `ReadOnlySpan<T>` is declared `public property [i: int]: T?` (`Bezel_Standard_Library.md:2681`), because `OwningBuffer<T>` slots *are* nullable-and-owned. So §15.4.1's own example body — `rb.Push(elements[i])` against `public method Push(item: T)` (`Bezel_Language_Spec.md:3058`, `:3053`) — feeds a `T?` to a `T`. `ReadOnlyArraySpan<T>`'s indexer is `public property [i: int]: T` (`Bezel_Standard_Library.md:3116`), uniformly, and stdlib §4.15 states why (`:3090`): *"Unlike `Span<T>` over `OwningBuffer<T>`, there is no `T?` wrapping; the source array does not store nullable wrappers for class T (the array stores borrows directly)."* The example type-checks verbatim.

**(b) `string` elements are unreachable.** stdlib §4.14 (`:2412`): *"Runtime-managed reference types (`string`, function types) are not admitted as T"* for `OwningBuffer<T>`. So `ReadOnlySpan<string>` is not a well-formed type, and today a user collection of strings — one of the two most obvious element types for a literal protocol — cannot declare the borrowing form at all. §28.11's slot table gives runtime-managed references their own borrowing-family row, *"shared references"* (`Bezel_Language_Spec.md:7440`), and `Array<string>` is ordinary. The rename makes `[” a”, ” b”]` reachable.

**(c) The element borrow's source is wrong.** #3310's second symptom: *"the element borrow's **source** is the temporary span being stored into a field."* Under `ReadOnlySpan`, that is exactly right and exactly fatal: the `OwningBuffer` **is** the elements' owner, so a factory storing `elements[i]` stores a borrow of a compiler temporary that dies at the call. Under `ReadOnlyArraySpan`, §28.11's own indexer-get row settles it (`Bezel_Language_Spec.md:7454`, borrow-slots column): *"borrow out (**source: the element's original owner**; the collection is a transit point)"*. The source becomes the caller's binding — the ordinary, already-specified §28.11 residual, not a statically-certain dangle.

Three independent defects, one edit. That is the shape of a diagnosis.

### 1.4 Both candidates #3310 named are wrong

**Candidate 1 — restrict the protocol at class `T`** (`where T: value`, or a stated restriction in §15.4.1). Self-defeating. §28.11's element-slot-policy table (`Bezel_Language_Spec.md:7437–7442`) places the slot kind **borrows** in exactly one row:

| Element category | Borrowing family slots | Owning family slots |
|---|---|---|
| value type (non-move-only) | copies | *(rejected at instantiation)* |
| runtime-managed reference | shared references | *(rejected at instantiation)* |
| **class / interface type** | **borrows** | owned |
| move-only value type | *(rejected)* | *(rejected)* |

A form whose declared purpose is *"a type `C<T>` whose elements are borrows"* would be admitted precisely where its elements are **not** borrows and forbidden precisely where they are. The restriction is not narrow; it is the empty set of the form's own definition.

**Candidate 2 — change `ReadOnlySpan`'s class-`T` indexer to `T`.** This is a much larger change to a landed family and it is *wrong on the merits*, not merely expensive. `OwningBuffer<T>` slots are nullable **because the buffer owns them**: `new OwningBuffer<T>(length)` produces *"all slots null"* (`Bezel_Standard_Library.md:2421`), and the `move`-and-restore discipline needs `buf[i] := null` (`:2487`). Non-nullable slots would break the storage primitive, not just the view. The cost confirms it: at least **fifteen** normative sites state the class-`T` nullable shape — stdlib `:2407`, `:2487`, `:2497`, `:2513`, `:2541`, `:2630`, `:2634`, `:2646`, `:2678`, `:2696`, `:3220`, `:3222`, `:3228`, and spec `:2530`, `:6648` — plus a landed compiler fix and its regression test, whose header states the accept-trap the current typing exists to close (`tests/codegen/ownedbuffer_class_slot_nullable_1157_test.cpp:1–21`):

> ACCEPT-TRAP: `var x := buf[0]; return x.v;` over an empty (null) slot compiled clean and null-dereferenced at runtime (SIGSEGV). With the slot typed `T?`, the member access requires a null guard / `?.`.

Reverting that is reintroducing a shipped segfault to fix an unrelated protocol's signature.

### 1.5 Why it survived nine revisions

It was never exercised. The conformance corpus carries three §15.4.1 rows and **all three are skipped** (`tests/conformance/manifest.tsv:74–76`): `spec_0072` and `spec_0073` are the two bare signature lines (`skip:signature-listing`), and `spec_0074` — the `RingBuffer<T>` example, the only place the protocol is *used* — is `skip:elided`, because the doc block writes `public method Push(item: T) ... end;`. The elision that made the case unrunnable is on the very method whose call is ill-typed.

In `tests/`, ten borrowing-form declarations exist and **every one is at a value element type**:

| # | Site | Element type | Indexes the span? |
|---|---|---|---|
| 1–2 | `tests/sema/collection_literal_dict_form_target_3031_test.cpp:261`, `:286` | `C<int>` | no |
| 3–9 | `tests/sema/collection_literal_factory_conjuncts_3032_test.cpp:117`, `:149`, `:177`, `:195`, `:214`, `:241`, `:276` | `<int>` (five are declaration-diagnostic cells that never instantiate) | no |
| 10 | `tests/codegen/collection_literal_user_type_fcl_2957_test.cpp:177` | `ReadOnlySpan<int>` | yes — `items[0] + items[1] + items[2]` |

Site 10 is the only one that reads through the span, and it reads at `int`, where the two families' indexers agree. Nothing in the tree has ever instantiated the borrowing form at a class element type. The seam was structurally invisible to the gate, however many times it ran.

---

## 2. The design ▲

### D1 — The borrowing form's parameter type becomes `ReadOnlyArraySpan<T>` ▲

▲ §15.4.1's borrowing form is declared:

```bezel
public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned C<T>;
```

`ReadOnlyArraySpan<T>` is stdlib §4.15's existing read-only view over an `Array<T>` (`Bezel_Standard_Library.md:3114–3143`). It is used exactly as declared; **this proposal adds no stdlib surface and changes none** (D5).

Four properties make it the right type, in strength order:

1. **It is the borrowing family's view.** stdlib §4.15's non-interchangeability sentence (`:3028`) says the families differ *by whether the storage owns class-`T` slots*, and the borrowing form's own definition is *"whose elements are borrows"*. The declared type now matches the declared meaning.
2. **Its indexer is category-uniform.** `[i: int]: T` for every `T` (`:3116`). The rationale records this as the deliberate consequence of the borrowing storage model (`docs/Bezel_Design_Rationale.md:591`): *"(a) the indexer return type is `T` directly (the array's element type), not `T?` (no nullability wrapping — the array stores what it was instantiated with)"*.
3. **It is what the compiler already emits** (this proposal's §1.2).
4. **It admits every element category the borrowing family admits** — value, runtime-managed reference (`string`, function types), class, and interface — because its source is `Array<T>`, whose admissible element set is §28.11's borrowing column verbatim.

**On the fixed-extent objection.** The obvious challenge to D1 is that `ReadOnlyArraySpan<T>` views a *resizable* collection while a collection literal has a fixed extent. It does not survive contact with the text. §15.5's `Length`/`Count` table (`Bezel_Language_Spec.md:3075`) classifies the type in **the fixed-extent family**:

> | `.Length` | `[N]T` (inline), `string`, and the **fixed-extent buffer/span family** (`OwningBuffer<T>`, `Span<T>`/`ReadOnlySpan<T>`, `ArraySpan<T>`/**`ReadOnlyArraySpan<T>`**) | Size, compile-time constant for inline arrays |

The *span* is fixed-extent — `(Start, Length)` are set at construction and never move (`Bezel_Standard_Library.md:3114`). What is resizable is the `Array<T>` **source**, and stdlib §4.15's Caveat paragraph (`:3108`) describes the resulting footgun for the general case: *"A span's `Length` is fixed at construction; subsequent mutation of the underlying array does not update the span."* At the literal site that footgun is **structurally unreachable in the ordinary case**: the backing array is materialized by the compiler, lives only for the call, and is named by no binding any caller holds. It is reachable in exactly one way — through the span's own public `Source` field — which is a pre-existing property of the protocol, is *not* introduced by this change, and is strictly less dangerous than what the current spelling exposes. That is §7's first residual, recorded rather than waved away.

### D2 — The owning form is untouched ▲

▲ `public static method FromCollectionLiteral(owned elements: OwningArray<T>): owned C<T>` stands, unchanged, with its `where T: class` (spec §14.4; stdlib §4.5). Its type is already the owning family's; nothing in this proposal reaches it. The *"Exactly one of the two forms may be declared"* rule (`Bezel_Language_Spec.md:3028`) is unchanged, and the two carriers remain distinguishable by their head name, which is how both sema and codegen already discriminate (`src/sema/TypeResolution_Checks.cpp:1455–1456`; `src/codegen/LlvmBackend_Setup.cpp:26313`, `:26406`).

### D3 — The *Lowering* clause follows the type ▲

▲ The borrowing form's lowering materializes a temporary `Array<U>` of the n element values and a `ReadOnlyArraySpan<U>` over it; the array is compiler-owned, its lifetime encloses only the `FromCollectionLiteral` call, and it is released after the call returns.

The current clause's parenthetical — *"the storage for the span may be stack-allocated"* (`Bezel_Language_Spec.md:3032`) — is retired, deliberately. It is a permission no conforming implementation can take once the span's source is an `Array<U>` (a class), and the shipped implementation already declines it in as many words (`src/codegen/LlvmBackend_Setup.cpp:26299–26306`): *"the source MUST be a real `bz_array_t*` — not a raw element buffer; the spec's 'the storage for the span may be stack-allocated' allowance is interpreted to mean 'the backing is allocated by codegen and released after the call'."* Retiring a permission nobody can exercise is a subtraction, not a restriction; **no program's acceptance changes.**

The rest of the clause is retained verbatim, including the load-bearing half: *"the factory must not retain the span beyond its own execution (the span is a borrow-bearing value subject to [§28.6] — storing it in the constructed collection's field is a borrow-lifetime violation, diagnosed in the factory body, not at the literal site)."* `ReadOnlyArraySpan<T>` is a borrow-bearing record exactly as `ReadOnlySpan<T>` is — rationale §5.15 lists both under *"The buffer- and array-backed spans are **borrow-bearing** records"* (`docs/Bezel_Design_Rationale.md:517`) — so §28.6 and §9.7 apply unchanged and the sentence keeps its full force.

### D4 — A borrowing-form target is a borrowing-family target for §15.4's fresh-producer rejection ▲

This is the one place the proposal decides something §15.4.1 does not currently state, and it is **mandatory**, not opportunistic.

§15.4's rule for borrowing-family element expressions (`Bezel_Language_Spec.md:2936`) opens with a **closed list of built-ins**:

> For **borrowing-family** targets (`Array<T>`, `Set<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>`, and the key and value positions of `Dictionary<K, V>`), each element expression of **non-trivial-dispose class type** must be a **borrow** whose source satisfies the store rule of [§28.11]; an element expression that is a **fresh producer** of such a type … is rejected at the literal site.

A user type declaring the borrowing form is not on that list, and §15.4.1 states no element-ownership rule of its own — its *Lowering* clause says only that each `ei` *"is type-checked against `U` and must be implicitly convertible to it"*. So the ownership disposition of `var rb: RingBuffer<Connection> := [new Connection("a")];` is **unstated**, while the lowering stamps no element disposer (`src/codegen/LlvmBackend_Setup.cpp:26325`) and frees only the backing (`:26386–26391`) — the fresh `Connection` reaches no owner at all.

Today that hole is hard to reach, because the class-`T` borrowing form does not type-check (this proposal's §1.3a). **D1 makes it reachable.** Landing D1 without D4 would convert an ill-typed program into a well-typed leak, which is a strictly worse outcome than the defect being fixed. So:

▲ §15.4's borrowing-family parenthetical is extended to name *"a user-defined type whose `FromCollectionLiteral` declares the borrowing form (§15.4.1)"*. Everything that follows — the fresh-producer rejection, the trivial-dispose literal-tree admission, the store rule's residual, the recorded literal-only-rejection asymmetry — then applies verbatim, with no new mechanism and no second rule.

**Body-locality classification (proposal 0105 §1): local.** The check is at the literal site, over the literal's own element expressions and the target type's declared factory carrier; it consumes no cross-body information. It is the existing §15.4 predicate applied to one more target class — the *same* predicate, not a parallel one.

**⚠ Composition note — an in-flight v0.271 edit touches this same line.** While this proposal was being drafted, the working tree at HEAD `fbe22213` carried *uncommitted* v0.271 edits (authored by another lane, not by this proposal), one of which amends the **same sentence** at `Bezel_Language_Spec.md:2936` from the other direction: it adds *"(built-in, or a user-declared target of the [§15.4.1] protocol, whose `FromCollectionLiteral` result is `owned`)"* to the **nested-literal** clause, making a nested literal that targets a user FCL type count as a *fresh producer*. That is the complement of D4, not a conflict — theirs classifies the FCL literal as a producer in *someone else's* element position; D4 classifies an FCL borrowing-form type as a *target* whose own element positions carry the rule. The two compose, and Edit 6's `OLD` anchor (the target parenthetical) is byte-identical in both texts, so the edit still applies uniquely. **Flagged for the composition gate (process §4) rather than resolved here**: if both land in one revision, one reviewer must read the resulting sentence as a whole.

**Kernel classification (v0.198): derivation.** No new axiom and no axiom extension: this is K1's per-slot classification plus K2.3/K4.3's stored-borrow treatment, reached through §28.11's existing borrow-slot column. Appendix C.1's existing bullet (`Bezel_Language_Spec.md:11829`) already states the invariant in target-neutral language (*"A fresh-producer element of non-trivial-dispose class type in a **borrowing-family** collection literal is rejected"*) and needs no edit — D4 makes the invariant's coverage match its wording.

### D5 — No stdlib edit; `ReadOnlyArraySpan<T>` is consumed as declared ▲

Verified, not assumed. The borrowing form's factory bodies need `Length` and the indexer; `ReadOnlyArraySpan<T>` declares both (`Bezel_Standard_Library.md:3114`, `:3116`, and the positional `Length` field's generated public read-only property per spec §9.1 `:1292`). Iteration works too — spec §29.2 already gives `ArraySpan<T>`/`ReadOnlyArraySpan<T>` their own desugaring paragraph (`Bezel_Language_Spec.md:7851`), so `for var e in elements do … end` is admitted in a factory body, which it is *not* for `ReadOnlySpan` beyond the same indexed form. The byte tier is mirrored onto `ReadOnlyArraySpan<byte>` (stdlib `:3030`, `:3155`), so a byte-element factory keeps its parse surface.

**One member is lost, and it is recorded rather than restored.** `ReadOnlySpan<T>` at value `T` declares `ToArray(): owned Array<T>` (`Bezel_Standard_Library.md:2739`, v0.222/#1810); `ReadOnlyArraySpan<T>` does not. A factory body spelled `elements.ToArray()` would stop compiling. In-tree uses: **zero** (no test's factory body calls it — this proposal's §1.5 table). External uses: zero, there being no external users. The remedy is a loop or an explicit `Array<T>`; adding `ReadOnlyArraySpan<T>.ToArray` is a coherent stdlib symmetry gap but is **not proposed here** — it would be a stdlib addition riding a signature fix, and *"dependencies and surface are debt"*. Recorded as a piggyback pin for a stdlib pass (this proposal's §7, residual R3).

### D6 — Restricting the protocol at class `T` is **declined**

Per this proposal's §1.4, candidate 1. **Reopening bar:** evidence that §28.11's element-slot-policy table is wrong to place borrow slots only in the class/interface row — i.e. a demonstration that a borrowing collection's *value*-typed slots are borrows rather than copies. Absent that, restricting the borrowing form at class `T` forbids it exactly where its definition places it, and no restatement of the ergonomic case reopens it.

### D7 — Changing `ReadOnlySpan`'s class-`T` indexer is **declined**

Per this proposal's §1.4, candidate 2. **Reopening bar:** a demonstration that `OwningBuffer<T>` can own class-`T` slots without nullable ones — specifically, a construction and a `move`-restoration discipline that satisfies stdlib §4.14's *"all slots initialized to null"* and the §28.8.1 field-restoration rule without a null slot state — **plus** an account of `tests/codegen/ownedbuffer_class_slot_nullable_1157_test.cpp`'s accept-trap under the new typing. Re-raising the ergonomic complaint that `T?` is inconvenient does not reopen it.

### D8 — Not source-breaking for users; a ten-site text rename in-tree

**There are no external users of Bezel.** The repository states it plainly (`CLAUDE.md`: *"There are no external users yet"*), and this proposal states it plainly too, rather than reasoning about a compatibility surface that does not exist. The change is therefore source-breaking **in classification** — a declared parameter type of a recognized protocol changes, so a hypothetical existing borrowing-form declaration would need one identifier rewritten — and **zero-cost in fact**.

▲ Migration, complete: in every borrowing-form declaration, `ReadOnlySpan<T>` → `ReadOnlyArraySpan<T>`. Nothing else. Factory **bodies** need no change at value element types, because both types expose `Length` and an indexer that yields `T` there; at class element types, bodies that could not compile before now can. The one exception is `ToArray()` (D5). See §5 for the site list.

### D9 — This must sequence before #3247

#3247 (master plan, charter C9 — `docs/proposals/papers/master-plan-2026-07.md:141`) promotes `FromCollectionLiteral` to *"a real `ICollectionLiteralTarget` static interface member"*, a path spec §11.8 records as chosen precisely because it costs no source change (`Bezel_Language_Spec.md:2068`): *"promoting the name-recognized `FromCollectionLiteral` (§15.4.1) to a real `ICollectionLiteralTarget<T>` … both signatures were chosen for zero-source-change promotion."*

That zero-source-change property is a property of **the current signature**. Promote first and the wrong parameter type is frozen into a declared interface member, after which fixing it is an interface-surface change under spec §2.8.6 rather than a four-line spec edit. ▲ **This proposal sequences first**; #3247's promotion then carries the corrected signature and keeps its zero-source-change claim intact.

### D10 — The normative surface ▲

The complete edit set is this proposal's §4. It is four spec lines, one spec parenthetical, and three rationale lines. No stdlib line, no grammar line, no Appendix C row, no guide, no `Bezel_For_Code_Generation.md` line.

---

## 3. The options, compared honestly

### 3.1 Option A — `ReadOnlyArraySpan<T>` (**recommended**, D1)

**For.** Costs no new surface. Is the borrowing family's declared view. Is what the compiler already emits. Dissolves all three of #3310's symptoms (§1.3). Removes the borrowing form's dependence on `T`'s element category entirely, since the type is declared once for all `T` rather than form-dependently (§6.1). Keeps §15.4.1's example, its `Length`, and its indexer text verbatim.

**Against, stated fairly.** (i) The view's *source* is resizable, so §4.15's Caveat paragraph nominally attaches to a protocol where extent is fixed — answered by §15.5's own classification of the type as fixed-extent and by the backing array being compiler-materialized (D1). (ii) `ToArray()` is lost at value `T` (D5). (iii) A factory can reach the compiler's temporary through the span's public `Source` field — a residual, and a *smaller* one than the status quo's (§7, R1).

### 3.2 Option B — a new fixed-extent borrowing view ▲ (**declined**)

The clean-semantics option: a view type whose extent is fixed by construction *and* whose slots are borrows, with no resizable source behind it and no public handle onto the backing.

**It is not one new type; it is a new storage primitive plus a view, in an area the spec explicitly defers.** §28.11 closes the element-slot-policy section with exactly this (`Bezel_Language_Spec.md:7457`):

> The storage embodiment of the owning column is `OwningBuffer<T>` (stdlib [§4.14]), the primitive over which the resizable owning collections compose; **the borrowing family's storage remains a compiler intrinsic in this revision, because a user-expressible borrow-storing primitive needs its own borrow-lifetime rules** (storing borrows in fields is otherwise rejected, [§28.3]) — **that design is deferred**, recorded in the rationale.

A fixed-extent borrowing *view* needs a fixed-extent borrowing *source*. `Array<T>` is the only borrow-slot storage the language has, and it is resizable and compiler-intrinsic. So Option B either (a) re-backs onto `Array<T>` — in which case it is `ReadOnlyArraySpan<T>` with the `Source` field hidden, a near-duplicate whose only content is one field's visibility, or (b) mints the deferred user-expressible borrow-storing primitive, opening the borrow-lifetime design §28.11 declines to open, for a signature fix.

Branch (a) additionally fails the project's own surface discipline. Rationale §5.15 already records the per-type tax as a live cost (`docs/Bezel_Design_Rationale.md:595`): *"Methods that want to operate on 'any span' need overloads — Bezel's nominal record typing doesn't admit implicit conversions between them. The friction is real but bounded."* v0.188 retired `ReadOnlyAsciiByteSpan` to take the family from six types to five (`docs/Bezel_Changelog.md:6667`), and process §10's standing subtraction review exists to ask *"what is no longer earning its place?"* Adding a sixth span three revisions of thinking after deleting the fifth, to avoid renaming an identifier in ten places, inverts that discipline.

**Declined.** ▲ **Reopening bar:** a use for a fixed-extent borrowing view **outside** the collection-literal protocol — a second consumer — *together with* a design for the borrow-storing primitive that discharges §28.11's stated deferral (its own borrow-lifetime rules). One consumer never justifies a type; the deferral is the harder half.

### 3.3 Option C — one carrier for both forms (**declined**)

The issue asks whether the two forms should take the *same* type with the family distinction carried elsewhere. Three shapes were considered; all fail, and the reason is the same one each time.

**C1 — both forms take `IReadOnlySpan<T>`** (stdlib §4.16, the interface that already unifies the family — `Bezel_Standard_Library.md:3203–3213`). Fails four ways. (i) **It does not remove the category split it exists to remove**: §4.16's implements-table (`:3220–3226`) has `ReadOnlySpan<T>` implement `IReadOnlySpan<T?>` at class `T` and `IReadOnlySpan<T>` at value `T`, and *"the interface's TElement parameter is invariant"* (`:3228`) — so the parameter type still reads differently by category. (ii) **It cannot carry the owning form.** The owning form must *transfer*: the factory *"drains the array"* and *"is responsible for the array's discharge per the standard owned-parameter rules"* (`Bezel_Language_Spec.md:3033`). §4.16's surface is deliberately escape-free and read-only — no `move`, no `Slice`, no enumerator (spec §8.6, `:1257`) — so an owning factory could not take ownership of anything through it. (iii) **It erases the discrimination the protocol needs**: §15.4.1 requires *"Exactly one of the two forms"* and distinguishes them by carrier; one carrier for both means the compiler cannot tell which lowering to emit. (iv) **It costs a virtual dispatch per element** on a construction path — stdlib §4.16 says so and says what to do instead (`:3234`): *"For code that accesses spans in inner loops, the recommended pattern is to take the concrete span type."*

**C2 — collapse to one form, always owning.** Rejected on instantiation: `OwningArray<int>` is *"rejected at the instantiation site"* (`Bezel_Standard_Library.md:1528`; spec §14.4), so every value-element collection loses the protocol. And a borrowing target must not receive ownership at all.

**C3 — one nominal carrier, distinguished by the `owned` modifier.** `owned ReadOnlyArraySpan<T>` is not the owning form: the *elements* would still be borrows and only the span record would transfer. There is no spelling in which one type carries both slot policies, because the slot policy is a property of the storage, not of the parameter modifier — which is stdlib §4.15's sentence again.

**Declined.** ▲ **Reopening bar:** #3247's interface promotion producing a shape in which one declared `ICollectionLiteralTarget<T>` member must carry both forms. If it does, the discrimination question returns as *that* design's problem, with a static interface member's constraints available to solve it — none of which exist today.

### 3.4 Option D — `Array<T>` itself, no span at all (**declined**; not considered by the issue)

Adversarially, the most economical option is not a span. Codegen already materializes an `Array<U>` (`src/codegen/LlvmBackend_Setup.cpp:26320–26324`); the span struct is a wrapper over it. Why not declare the borrowing form as `FromCollectionLiteral(elements: Array<T>): owned C<T>` and hand the factory the array directly? `Array<T>` is *the* borrowing collection (spec §28.11's first bullet, `Bezel_Language_Spec.md:7426`), its indexer is uniform in `T`, and it needs no view type at all.

**Declined, on one decisive ground and two supporting ones.**

1. **It loses the static retention check.** §15.4.1's lowering leans on the span being a *borrow-bearing record*: *"the factory must not retain the span beyond its own execution … storing it in the constructed collection's field is a borrow-lifetime violation, **diagnosed in the factory body**"* (`Bezel_Language_Spec.md:3032`). That diagnosis is §9.7/§28.6 machinery over a borrow-bearing **value**. `Array<T>` is a class; storing a class-typed parameter into a field is §28.11's *residual-trust* store rule (`:7469`) — *"the programmer's obligation, with [K4.1]'s backstop as the named safety net"* — i.e. an obligation with a runtime backstop, not a compile-time rejection. Option D downgrades a statically-diagnosed violation to a trusted one, on a value the compiler owns and frees at the call. That is a real loss of guarantee for a cosmetic simplification.
2. **It changes the example's spelling for the worse.** §15.5 (`:3075–3076`) gives `Array<T>` `.Count`, not `.Length`; the example's `elements.Length` and `0..<elements.Length` would both change, and the protocol's "fixed extent" would be spelled with the member the spec reserves for *"resizable collections"* whose size *"may change"*.
3. **It hands the factory a mutable handle by construction.** `Array<T>` has `Add`/`Insert`/`Clear`; a read-only span does not. Option A's residual R1 (reaching the backing through `Source`) becomes Option D's default.

▲ **Reopening bar:** a decision to retire borrow-bearing-record diagnosis in favour of the §28.11 residual-trust rule generally — in which case the protocol should follow the language, not lead it.

### 3.5 The comparison

| | A — `ReadOnlyArraySpan<T>` | B — new fixed-extent borrowing view | C — one carrier (`IReadOnlySpan<T>`) | D — `Array<T>` |
|---|---|---|---|---|
| New stdlib surface | **none** | a view **and** a storage primitive (§28.11's deferred design) | none | none |
| Family-coherent (stdlib §4.15) | **yes** | yes | no — still splits by category | yes |
| Indexer uniform in `T` | **yes** | yes | **no** (`T` vs `T?`) | yes |
| Carries the owning form too | n/a (two forms kept) | n/a | **no** — cannot transfer | n/a |
| Matches the shipped lowering | **yes** | no | no | partly (the wrapper differs) |
| Retention diagnosed statically | **yes** (borrow-bearing record) | yes | yes | **no** (residual trust) |
| Fixed extent | yes (§15.5) — resizable *source*, unreachable at the literal site | yes, by construction | yes | source is the collection |
| Spec edit size | 4 lines + 1 parenthetical | new stdlib §, plus the above | 4 lines, and it does not work | 4 lines |

---

## 4. Exact edits ▲

Five spec edits and three rationale edits. Each `OLD` is verbatim at `fbe22213` and unique in its file.

**Edit 1 — spec §15.4.1, the borrowing form's signature (`Bezel_Language_Spec.md:3019`).**

> **OLD:** `public static method FromCollectionLiteral(elements: ReadOnlySpan<T>): owned C<T>;`
> **NEW:** ▲ `public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned C<T>;`

**Edit 2 — spec §15.4.1, one clarifying sentence after the borrowing form's fence** (new; nothing is replaced).

> **NEW:** ▲ The carrier is the **borrowing family's** read-only view (stdlib [§4.15](Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant)), whose source `Array<T>` borrows its class-`T` slots — matching this form's definition. The owning family's `ReadOnlySpan<T>` is not a carrier for either form: its source `OwningBuffer<T>` owns those slots (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)), and the two span families are not interchangeable.

**Edit 3 — spec §15.4.1, the *Lowering* clause, borrowing form (`Bezel_Language_Spec.md:3032`).**

> **OLD:** `- *Borrowing form.* The compiler materializes a temporary ` `ReadOnlySpan<U>` ` over the n element values (the storage for the span may be stack-allocated; its lifetime encloses only the ` `FromCollectionLiteral` ` call) and emits ` `C<U>.FromCollectionLiteral(thatSpan)` `. Each ` `ei` ` is type-checked against ` `U` ` and must be implicitly convertible to it. The temporary span and its backing storage are released after the call returns; …`
> **NEW:** ▲ `- *Borrowing form.* The compiler materializes a temporary ` `Array<U>` ` holding the n element values and a ` `ReadOnlyArraySpan<U>` ` over it (the backing array is compiler-owned and named by no binding; its lifetime encloses only the ` `FromCollectionLiteral` ` call) and emits ` `C<U>.FromCollectionLiteral(thatSpan)` `. Each ` `ei` ` is type-checked against ` `U` `, must be implicitly convertible to it, and — for class-typed ` `U` ` — is subject to [§15.4](#sec-collection-literals)'s borrowing-family element rule. The temporary span and its backing array are released after the call returns; …`
> *(The clause's remainder — the no-retention sentence and its §28.6 citation — is retained verbatim; `ReadOnlyArraySpan<U>` is a borrow-bearing record exactly as `ReadOnlySpan<U>` is, rationale §5.15.)*

**Edit 4 — spec §15.4.1, the empty-literal clause (`Bezel_Language_Spec.md:3035`).**

> **OLD:** `the borrowing form receives an empty ` `ReadOnlySpan<U>` `; the owning form receives an empty ` `OwningArray<U>` `.`
> **NEW:** ▲ `the borrowing form receives an empty ` `ReadOnlyArraySpan<U>` `; the owning form receives an empty ` `OwningArray<U>` `.`

**Edit 5 — spec §15.4.1, the `RingBuffer<T>` example (`Bezel_Language_Spec.md:3055`).**

> **OLD:** `  public static method FromCollectionLiteral(elements: ReadOnlySpan<T>): owned RingBuffer<T>`
> **NEW:** ▲ `  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned RingBuffer<T>`
>
> The body — `var rb := new RingBuffer<T>(elements.Length);`, `for var i in 0..<elements.Length do`, `rb.Push(elements[i]);` — is **unchanged**, and now type-checks at class-typed `T` as well as value-typed `T`. This is the whole visible point of the proposal.

**Edit 6 — spec §15.4, the borrowing-family parenthetical (D4; `Bezel_Language_Spec.md:2936`).**

> **OLD:** `For **borrowing-family** targets (` `Array<T>` `, ` `Set<T>` `, ` `LinkedList<T>` `, ` `Stack<T>` `, ` `Queue<T>` `, and the key and value positions of ` `Dictionary<K, V>` `), each element expression…`
> **NEW:** ▲ `For **borrowing-family** targets (` `Array<T>` `, ` `Set<T>` `, ` `LinkedList<T>` `, ` `Stack<T>` `, ` `Queue<T>` `, the key and value positions of ` `Dictionary<K, V>` `, and a user-defined type whose ` `FromCollectionLiteral` ` declares the borrowing form ([§15.4.1](#sec-the-user-defined-sequence-literal-protocol))), each element expression…`

**Edit 7 — rationale §5.16, the factory-signature quotation (`Bezel_Design_Rationale.md:651`).**

> **OLD:** `(` `public static method FromCollectionLiteral(elements: ReadOnlySpan<T>): owned C<T>` `, spec §15.4.1)`
> **NEW:** ▲ `(` `public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned C<T>` `, spec §15.4.1)`

**Edit 8 — rationale §5.16, the element-type-from-signature paragraph (`Bezel_Design_Rationale.md:653`).**

> **OLD:** `the element type *is* the ` `ReadOnlySpan<T>` ` (or ` `OwningArray<T>` `) parameter's element type`
> **NEW:** ▲ `the element type *is* the ` `ReadOnlyArraySpan<T>` ` (or ` `OwningArray<T>` `) parameter's element type`

**Edit 9 — rationale §5.16, the two-shapes paragraph (`Bezel_Design_Rationale.md:655`).**

> **OLD:** `the borrowing form takes ` `ReadOnlySpan<T>` ` (the compiler synthesizes a temporary span over the borrowed element values)`
> **NEW:** ▲ `the borrowing form takes ` `ReadOnlyArraySpan<T>` ` — the **borrowing** family's view, over an ` `Array<T>` ` whose slots borrow their class-typed contents (stdlib §4.15); the compiler synthesizes that array and the span over it, both scoped to the call`

▲ **Rationale, one new sentence in the same paragraph**, so the decision is recorded rather than silently corrected:

> ▲ v0.111 named `ReadOnlySpan<T>` here by analogy to C#'s `Create<T>(ReadOnlySpan<T>)`. The analogy does not carry: C#'s `ReadOnlySpan<T>` does not own its elements, and Bezel's does — its source `OwningBuffer<T>` owns each class-typed slot (stdlib §4.14), where the borrowing form's whole premise is that the constructed collection does not. The correction is a family correction, not a nullability one.

**Changelog.** One entry at the graduating revision, class **design change → ruling-class landing** (process §2, §6). Its Migration note states: newly-rejected — nothing; newly-defined — the borrowing form's carrier, and (D4) the fresh-producer rejection at user borrowing-form targets; newly-warned — nothing; source-affecting — a borrowing-form declaration's parameter type name, with no external users and ten in-tree sites (this proposal's §5).

---

## 5. Costs and blast radius, counted at v0.270

### 5a. Normative documents

**Nine edits, eight of them one line.** Spec: 4 lines + 1 parenthetical + 1 new sentence (Edits 1–6). Rationale: 3 lines + 1 new sentence (Edits 7–9). **Stdlib: zero** (D5). **Appendix A: zero** — no syntax, no token. **Appendix C: zero** — bullet `:11829` already states the invariant target-neutrally.

**`Bezel_For_Code_Generation.md`: zero, verified.** `grep -n 'FromCollectionLiteral' docs/Bezel_For_Code_Generation.md` returns nothing; the document indexes `ReadOnlyArraySpan<T>` as a stdlib type (`:990`) and never documents the literal protocol. Its PIN moves with the revision as usual, but this proposal contributes no line to it. **Guides: zero, verified** — Guide 05 mentions the span families (`docs/Bezel_Guide_05_Collections_and_Iteration.md:345`) and no guide teaches `FromCollectionLiteral`.

### 5b. The conformance corpus

Three rows, all currently skipped (`tests/conformance/manifest.tsv:74–76`):

- `spec_0072` (`skip:signature-listing`) — `tests/conformance/cases/spec_0072.bzl:1` is the borrowing-form signature line; **re-extracted text**, still a signature listing, still skipped.
- `spec_0073` (`skip:signature-listing`) — owning form; **untouched** (D2).
- `spec_0074` (`skip:elided`) — the `RingBuffer<T>` example; **re-extracted text**. It remains `skip:elided` for an unrelated reason (the doc block's `public method Push(item: T) ... end;`), so the row's class does not change.

Per process §6, a proposal touching the corpus must list every case to delete-or-convert: **none is deleted, two are re-extracted, one is untouched, and no row's skip class moves.** ⚠ `tests/conformance/README.md:146` asserts *"Today `spec_0072` is live (`fragment`)"*, which the manifest contradicts. The manifest is ground truth; the README line is stale and is flagged here rather than fixed (out of scope, and not this proposal's file to edit).

### 5c. Tests — declaration text only, no semantics

Ten sites, enumerated in this proposal's §1.5 table. Each is a `ReadOnlySpan` → `ReadOnlyArraySpan` substring rewrite inside a C++ string literal. **No test's assertion changes**, because:

- five cells (`3032:117`, `:149`, `:177`, `:195`, `:241`) assert **declaration-site diagnostics** keyed on `public`/`static`/`owned`-return/both-forms conjuncts, not on the carrier name;
- three cells (`3031:261`, `:286`, `3032:276`) instantiate at `C<int>`, where both carriers' `Length` and indexer agree;
- `3032:214` is an arity-mismatch cell at `ReadOnlySpan<int>`;
- `2957:177` is the only body that reads through the span (`items[0] + items[1] + items[2]`), at `int`, where both carriers yield `int`.

**Two diagnostic strings must move with the rename** or the sema cells' `find` calls go stale: `src/sema/TypeResolution_Checks.cpp:1562` and `:1573` both spell the carrier pair as *"(`ReadOnlySpan<T>` and `OwningArray<T>`…"* inside the both-forms message. The corresponding test predicates are `rejectedByBothFormsConjunct` (`tests/sema/collection_literal_factory_conjuncts_3032_test.cpp:59`) — worth naming here because it is the one place a "text-only" migration can silently break a test.

### 5d. Compiler — three identifiers

- `src/sema/TypeResolution_Checks.cpp:1455` — `carrierName == "ReadOnlySpan"` → `"ReadOnlyArraySpan"`; the comment at `:1450` and the two messages at `:1562`/`:1573` follow.
- `src/codegen/LlvmBackend_Setup.cpp:26313` — `paramName == "ReadOnlySpan"` → `"ReadOnlyArraySpan"`; the header comment at `:26171` and the interpretation note at `:26299–26306` follow. **The lowering body underneath is unchanged**, because it is already the `ReadOnlyArraySpan` lowering (this proposal's §1.2).
- D4's fresh-producer rejection is a **new** sema check at the literal site for a user borrowing-form target, reusing §15.4's existing predicate. This is the proposal's only genuine implementation work, and it ships with a spec-cited regression test that fails without it.

A tracking issue is filed at graduation for the spec-ahead-of-impl delta (process §5 item 3): the carrier rename and D4's rejection, cited as `§15.4.1, §15.4, v0.NNN`.

### 5e. The cost that is not a count

The protocol's *"zero-source-change promotion"* claim at spec §11.8 (`:2068`) is spent once. Landing this before #3247 preserves it (D9); landing it after would make the claim retroactively false. That is a sequencing cost, not a code cost, and it is the reason this proposal is urgent rather than merely correct.

---

## 6. Interactions

### 6.1 #3314 — §3.6's category-dispatch reservation — **explicitly out of scope**

[#3314](https://github.com/s8lap/bezel-lang/issues/3314) is open and being worked in parallel. **This proposal does not decide it, and its D-decisions must not be read as deciding it.**

The issue: §3.6 states that dispatch of behavior on a derived type property *"is reserved to compiler-known standard-library types; it is not a user-available mechanism in this revision"* and closes *"users still cannot write code conditioned on a type parameter's category"* (`Bezel_Language_Spec.md:536`), while §15.4.1 asks a **user-authored** generic body to be correct across element categories, over an unconstrained `T` that §3.6 rule 2 says establishes nothing. #3314 states its own independence: *"Renaming the span family does not touch it."*

**What this proposal does change about #3314's §15.4.1 instance, precisely.** #3314's evidence at §15.4.1 is that the *carrier* is declared form-dependently: stdlib §4.14 declares `ReadOnlySpan<T>` twice — `IReadOnlySpan<T?>` with a `T?` indexer for class `T` (`Bezel_Standard_Library.md:2678–2704`) and `IReadOnlySpan<T>` with a `T` indexer for value `T` (`:2710–2743`) — so a body reading `elements[i]` genuinely sees a different type per category. `ReadOnlyArraySpan<T>` is declared **once**, for all `T` (`:3114–3143`), and rationale §5.15 records that this is not an accident (`docs/Bezel_Design_Rationale.md:593`): *"The category-dependent split was a consequence of ownership semantics that `Array<T>` doesn't have."*

So D1 removes **one** category-dependence from the protocol — the one attributable to the carrier. It does **not** follow that #3314 is resolved, and this proposal makes no such claim:

- the **owning** form's `where T: class` and its drain discipline are category-conditioned by construction, and D2 leaves them exactly as they are;
- §15.4's element rule (and D4's extension of it) is itself keyed on the element's category — *"each element expression of **non-trivial-dispose class type**"* (`:2936`) — which is a *compiler-side* dispatch at the literal site, but a user factory author must still reason about it;
- #3314's general question — whether implementing the protocol is a **third** recorded exception to §3.6's reservation, or whether the protocol is implementable only at a category-fixed `T` — is untouched by anything here.

**Scoping rule, stated so a later reader can rely on it.** ▲ Whichever way #3314 is ruled, this proposal's edits stand: they change *which type* the borrowing form names, not *whether* a user body can implement it. If #3314 rules that the protocol is implementable only at a category-fixed `T`, §15.4.1 gains that restriction on top of Edits 1–6 and none of them is reworked. If #3314 rules a third §3.6 exception, this proposal's carrier is the one that exception is written against — and the exception is **narrower** for having been written against a category-uniform carrier. #3314's ruling should cite this proposal's §6.1 and confirm the narrowing rather than assume it.

### 6.2 #3247 — the interface promotion

D9. Sequence this first. The promoted `ICollectionLiteralTarget<T>` member then declares `ReadOnlyArraySpan<T>`, and §11.8's zero-source-change claim survives.

### 6.3 The rest — audited, mostly zeros

- **§28.11 (element ownership).** No edit. The slot-policy table is the *authority* this proposal appeals to, not a thing it changes. Its indexer-get row's *"source: the element's original owner; the collection is a transit point"* (`:7454`) becomes applicable to the borrowing form for the first time, which is §1.3c.
- **§28.6 / §9.7 (leak prevention, borrow-bearing records).** Unchanged and still load-bearing: both span families are borrow-bearing records (rationale `:517`), so §15.4.1's no-retention diagnosis keeps its exact force (D3).
- **§8.6 (boxing of borrow-bearing records).** No contact. Nothing in either form boxes; the `IReadOnlySpan<TElement>` box is Option C's problem, and Option C is declined.
- **§15.5 (`Length` vs `Count`).** No edit; the table already lists `ReadOnlyArraySpan<T>` in the fixed-extent `.Length` family (`:3075`), which is what keeps the example's `elements.Length` valid.
- **§29.2 (`for` desugaring).** No edit; `ReadOnlyArraySpan<T>` already has its paragraph (`:7851`). A factory body may now iterate the carrier directly — a small widening the proposal gains for free and claims no credit for.
- **§14.4 / stdlib §4.5 (`owned` as a generic argument).** Untouched — owning form only.
- **Sendability (§30.14; stdlib §1's classification, `Bezel_Standard_Library.md:1466`).** No contact: the carrier is a call-scoped borrow-bearing record and crosses no domain boundary.
- **Kernel (v0.198).** Derivation, not axiom extension (D4).
- **Body-locality (0105 §1).** **Local** (D4). D1–D3 alter no compile-time rule at all; they change a declared type's name.

---

## 7. Residuals, accepted with reasons

**R1 — a factory can reach the compiler's temporary through the span's `Source` field.** `ReadOnlyArraySpan<T>` is a positional record (`Bezel_Standard_Library.md:3114`), and spec [§9.1](../Bezel_Language_Spec.md#sec-positional-records) generates *"Public read-only properties, one per positional field"* (`Bezel_Language_Spec.md:1292`). So a factory body may write `elements.Source` and, through it, `Add`/`Clear` the array the compiler materialized.

Accepted, for three reasons. (i) **It is not introduced here.** The status quo has the identical exposure via `ReadOnlySpan<T>`'s `Buffer` field, and that one is **worse**: `OwningBuffer<T>` at class `T` *owns* its slots and *"`move buf[i]` is admitted"* (`Bezel_Standard_Library.md:2407`), so today's declared carrier offers a factory an owning drain of elements the lowering supplies as borrows with no disposer — the family mismatch in its most concrete form. `Array<T>`'s slots are borrows and *"`move arrSpan[i]` is **not** admitted for any T"* (`:3094`); the worst a factory can do through `Source` is disturb an array that dies at the call. (ii) It is a property of every span in the language, not of this protocol. (iii) Closing it means hiding a positional field, which is a stdlib-wide question about span shape, not a collection-literal question. ▲ **Recorded as liftable**, not proposed: a general rule about compiler-materialized borrow sources would close it for every span at once.

**R2 — the §28.11 store-rule residual reaches the borrowing form.** Once class-typed borrowing forms compile (D1), a factory storing `elements[i]` into the constructed collection carries §28.11's K4.3 trust clause: *"whether each admitted element's source actually outlives the collection's use of it is the programmer's obligation"* (`:7469`). Accepted: this is the ordinary residual every borrowing collection carries, with checked disposal mode as the named backstop, and it is *the correct residual* — under the status quo the same store is a statically-certain dangle onto a dead temporary. Trading a certain dangle for a documented obligation is the improvement, not a new cost.

**R3 — `ToArray()` is lost at value `T`** (D5). Zero in-tree uses, zero external users. Recorded as a stdlib-symmetry piggyback pin, not proposed here.

---

## 8. Reopening bars, collected

Three declines, each with its bar stated where it is made. `DECLINED.md`'s decline index needs one row per decline, per process §9, **at graduation** (this proposal is a `draft`; nothing here is settled, and per `DECLINED.md`'s own vocabulary these are `recommended — awaiting owner ruling`, which *"no reader may cite as settled"*).

| Topic | Verdict class | Record | Reopening bar |
|---|---|---|---|
| Restricting §15.4.1's borrowing form at class-typed `T` | `recommended — awaiting owner ruling` → `declined-at-proposal` | this proposal D6 / §1.4 | Evidence that §28.11's element-slot-policy table is wrong to place borrow slots only in the class/interface row. Ergonomic re-argument does not reopen it. |
| Changing `ReadOnlySpan<T>`'s class-`T` indexer to non-nullable `T` | `recommended — awaiting owner ruling` → `declined-at-proposal` | this proposal D7 / §1.4 | A class-`T` `OwningBuffer` slot discipline with no null state that satisfies stdlib §4.14's construction and the §28.8.1 restoration rule, **plus** an account of `#1157`'s accept-trap under the new typing. |
| A new fixed-extent borrowing span type for the protocol | `recommended — awaiting owner ruling` → `declined-at-proposal` | this proposal §3.2 | A **second** consumer outside the collection-literal protocol, *together with* a design discharging §28.11's stated deferral of a user-expressible borrow-storing primitive. |

Options C and D are declined inside their own subsections with their bars (§3.3, §3.4); they are variants of the same question rather than separately citable declines, and are indexed under the third row above if the owner prefers one row per option.

---

## 9. Questions for the owner

- **Q1 — D1: is `ReadOnlyArraySpan<T>` the carrier?** This proposal recommends yes, on family coherence (stdlib §4.15), a category-uniform indexer, zero new surface, and agreement with the shipped lowering. The honest cost is that its *source* is a resizable collection, answered by §15.5's own classification and by the backing being compiler-materialized.
- **Q2 — D4: is the fresh-producer rejection extended to user borrowing-form targets in this train?** This proposal says it **must** be, because D1 is what makes the hole reachable. The alternative — land D1 now, file D4 as a follow-on — ships a revision in which `var rb: RingBuffer<Connection> := [new Connection("a")];` compiles and leaks.
- **Q3 — D3: is retiring *"the storage for the span may be stack-allocated"* acceptable?** It is a permission no implementation can exercise once the source is an `Array<U>`, and the shipped compiler already declines it in as many words. Retaining it would be retaining a sentence known to be unimplementable.
- **Q4 — sequencing (D9).** Confirm this lands before #3247's promotion. If #3247 is already in flight, that lane needs to know today, not at its landing.
- **Q5 — R3: does the owner want `ReadOnlyArraySpan<T>.ToArray()` added** to close the one surface asymmetry the migration exposes? This proposal deliberately does not propose it (a stdlib addition riding a signature fix), but the gap is real and a stdlib pass could take it.

---

## 10. Revision record

- **2026-08-21 — drafting-time tree note.** HEAD was `fbe22213` (v0.270) throughout and every quotation was taken against it. The *working tree* acquired uncommitted v0.271 edits from a concurrent lane during drafting (spec §22.8's `readonly` single-assignment rule, §22.9, and the §15.4 nested-literal clause; plus the four version headers). This proposal touched none of them. Every `OLD` fragment in §4 was re-checked against the working tree afterwards and each is still verbatim and unique; the one overlap is recorded in D4's composition note. The pin stays **v0.270** — re-verify at graduation as process §6 requires.
- **2026-08-21 — drafted** against v0.270 (`fbe22213`) for #3310, following the v0.270 investigation's routing comment. Every quotation re-verified at that tree; every count in §5 re-derived rather than carried from the issue. Two findings are this draft's own and are not in #3310: **D4** (§15.4's borrowing-family list is a closed enumeration of built-ins, so D1 would otherwise convert an ill-typed program into a silent leak) and **Option D** (`Array<T>` as the carrier — declined on the loss of borrow-bearing-record retention diagnosis). The `ToArray()` migration cost (R3) and the stale `tests/conformance/README.md:146` line are also first recorded here.


---

## 11. Landing record — v0.272 (ruled 2026-08-21 under the owner's delegation)

**D1 — accepted.** `ReadOnlyArraySpan<T>` is the carrier. The one serious objection — that it views a *resizable* collection while a literal has fixed extent — is refuted by the spec's own §15.5 table, which lists `ReadOnlyArraySpan<T>` in the **fixed-extent** buffer/span family by name. What is resizable is the `Array<T>` source; the span's `(Start, Length)` never move, and at a literal site the backing array is compiler-materialized and named by no binding. Decided further by the category-uniform indexer, zero new stdlib surface, agreement with the shipped lowering, and the fact that this type landed at **v0.107 — four revisions before** v0.111 chose the owning view by analogy to a C# type that, unlike Bezel's, does not own its elements.

**D4 — accepted, and mandatory in the same train.** §15.4's borrowing-family list is a closed enumeration of built-ins; a user FCL type is not on it, and §15.4.1 states no element rule of its own. That hole is unreachable today *only because the class-`T` form does not type-check*. D1 makes it reachable, so D1 alone would convert an ill-typed program into a **well-typed leak** — strictly worse than the defect being fixed. D1 does not land without D4.

**D3 — accepted.** The *"the storage for the span may be stack-allocated"* permission retires. No conforming implementation can exercise it once the source is an `Array<U>`, and the shipped compiler already declines it in as many words in its own comment. Retiring a permission nobody can take is a subtraction, not a restriction; no program's acceptance changes.

**D9 — confirmed.** This sequences before [#3247](https://github.com/s8lap/bezel-lang/issues/3247)'s first promotion, which was recorded on that issue at v0.271 — it had previously said *"any order among the three."* §11.8's zero-source-change promise is a property of the **current** signature, so promoting first would freeze the wrong carrier into `ICollectionLiteralTarget` and turn a signature correction into an interface-breaking change.

**R3 — declined.** `ReadOnlyArraySpan<T>.ToArray()` is a real symmetry gap: `ReadOnlySpan<T>` declares it at value `T` and this type does not, so a factory body spelled `elements.ToArray()` would stop compiling. In-tree uses are **zero** and external users are zero. It is nonetheless declined here, because a stdlib addition riding a signature fix is the *"while we're at it"* expansion the project's operating rules forbid, and surface is debt. Recorded as a pin for a stdlib symmetry pass, with its row in `DECLINED.md`.

**Options B, C and D remain declined** with their stated bars. B's decisive argument is the one #3310 never considered: a fixed-extent *borrowing* view needs fixed-extent *borrowing storage*, and §28.11 explicitly defers the user-expressible borrow-storing primitive — so B is a new storage primitive **plus** a view, not just a view.

**Composition check performed before landing.** D4 and v0.271's §15.4 edit amend the same sentence from opposite directions. The composed sentence was read whole rather than trusted: D4's clause qualifies a **target** (*"declares the borrowing form"*), v0.271's qualifies a **producer** (*"whose `FromCollectionLiteral` result is `owned`"*). Both cite §15.4.1, with visibly different predicates and no contradiction.
