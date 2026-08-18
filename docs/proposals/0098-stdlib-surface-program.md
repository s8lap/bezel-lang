# Proposal 0098 — Stdlib surface program: `ElementAtOrNull`, the `OwningSet` egress rename, `INumber<T>` aggregation, the exception-safety post-state band, and three truth repairs

| | |
|---|---|
| **Status** | graduated-v0.259 — superseded — graduated v0.259 (ruled under the owner's 2026-08-18 delegation; charter ruling C7 — `ElementAtOrNull` declared with its §29.11 warning-set membership stated explicitly, the `OwningSet` egress renamed `Pop`/`TryPop` with `PopValue` staying reasoned and the `TryPopAt` gap recorded, `Sum` migrated to the two `INumber<U>` generics with `Average` recorded blocked (§9 Q1 resolved as recommended; §22.6's worked overload example re-keyed to `Average` at the train), the stdlib §1 exception-safety band and block-by-name note landed in one pass (§9 Q2 resolved as recommended), the three truth repairs applied, `CountWhere` declined evidence-gated in the §1 note; the rename's non-mechanical src/test re-keying recorded as outstanding compiler work in the v0.259 changelog) |
| **Tracking issue** | coordinator files |
| **Target module(s)** | **stdlib** — §1 (the exception-safety post-state convention band, new; the block-by-name shadowing note, new; the `*OrNull` tier bullet and the Try-convention bullet, one name each), §5 (`ElementAtOrNull` declared; the `Sum`/`Average` selector overloads; the §26.4→§26.5 citation repair), §5.1 (the no-selector `Sum`/`Average` extension blocks), §9.7 (the LINQ-applicability claim narrowed), §4.10 (`OwningSet<T>.Take`/`TryTake` → `Pop`/`TryPop`), §4.9 (the `PopValue` note gains its reason). **spec** — §29.11 (the terminal-accessor warning set gains `ElementAtOrNull`), §11.8 (the `Sum<T>` normative example, if the migration changes its shape). **guides** — 02 and 05 (the transfer-name teaching lists `Take` on the owning family). |
| **Written against** | spec/stdlib **v0.255** (`git show 5fa5a95b:docs/*.md`) |
| **Currency** | **v0.256** (HEAD `8f41e99d` — the defect train `4130d2de` plus its gate fixes `e8f5387a`): verified to touch **none** of this proposal's quoted sections — the stdlib delta is the version header only, and the spec delta (§10.4/§13.3/§19.3/§22.9/§27.x/§28.3-producers/§28.4/A.4/C.x) leaves §29.11's warning set, §26.5, §29.2's span iteration, and §11.8 unchanged. Every quotation and count below stands as written. |
| **Origin** | Charter C7, from the stdlib-surface review. |

*Reference convention: unprefixed §-references are language-spec references; stdlib references are marked `stdlib §x`. Every quotation and every count is from the v0.255 snapshot (re-verified unchanged at v0.256) or from `tests/` at HEAD. Bold inside a quotation is the source's own unless marked* (emphasis added).

---

## 1. `ElementAtOrNull` — declared (adoption i)

**The defect.** Four normative texts speak of `ElementAt`'s `*OrNull` form as if it existed. It does not: stdlib §5's element-accessor block declares `FirstOrNull` (both forms), `LastOrNull` (both), `SingleOrNull` (both), and then `public method ElementAt(index: int): T;` — full stop. Verified against the snapshot, the four texts are:

1. **spec §29.11**, the terminal-accessor provenance warning's normative member set: *"`First`/`Last`/`Single`/`ElementAt` with the `*OrNull` and predicate forms, `Min()`/`Max()`, and `Aggregate(seed, fold)`."*
2. **stdlib §5**, *Ownership at class-typed substitutions*: *"`First`/`Last`/`Single`/`ElementAt` (with the `*OrNull` and predicate forms)"*.
3. **stdlib §5**, the element-accessor failure prose: *"`First`/`Last`/`Single`/`ElementAt` throw `InvalidOperationException` (or `IndexOutOfRangeException` for `ElementAt`) on missing-element; **the `*OrNull` variants return `null` instead**."*
4. **Guide 05 §**(eager operators): *"the element accessors (`First`/`Last`/`Single`/`ElementAt` and their `*OrNull` and predicate forms)"*.

So the member set of a **normative compiler warning** names a member the stdlib does not declare. That is the repair's urgency — stated precisely: the warning set is implementable as written (an entry for a member that cannot occur is simply **vacuous**), but a normative enumeration carrying a phantom member is a standing trap — four texts teach an API that does not resolve, and the first user to write `xs.ElementAtOrNull(i)` from the spec's own warning set gets a resolution error the documents say cannot happen. Not unimplementable; false in the way that misleads.

**The declaration** (stdlib §5, immediately after `ElementAt`):

```bezel
  public method ElementAt(index: int): T;
  public method ElementAtOrNull(index: int): T?;      // null when index is out of range
```

No predicate form: `ElementAt` has none (an index has no predicate analogue), so the four texts' *"and predicate forms"* continues to distribute over `First`/`Last`/`Single` only — the graduation train should not read the phrase as licensing an `ElementAtOrNull(predicate)`.

**§29.11's warning set** gains it explicitly. The set is normative and enumerated; after this item it reads `First`/`FirstOrNull`/`Last`/`LastOrNull`/`Single`/`SingleOrNull`/`ElementAt`/`ElementAtOrNull` (with the predicate forms where they exist), `Min()`/`Max()`, `Aggregate(seed, fold)` — and the classification is the same for the new member as for `ElementAt`: at a class or interface element type the result is a **borrow of the element sourced from the receiver** under §28.3's bare-type-parameter rule, so it is a warning-set member for exactly the reason `ElementAt` is.

**The nullable-conflation caveat applies unchanged.** stdlib §5 already records it for the whole `*OrNull` tier: *"When the element type is itself nullable, `?` is idempotent under substitution — spec §7.10 — so an `*OrNull` result cannot distinguish 'empty' from 'a present `null`'; use `Count` or the `Try` shape when the distinction matters."* `ElementAtOrNull` inherits that sentence; no new caveat is written.

**Migration:** newly defined, nothing rejected, nothing renamed. Zero call sites exist (the member does not exist), so the only corpus work is the new member's own tests.

## 2. `OwningSet<T>.Take`/`TryTake` → `Pop`/`TryPop` (adoption ii)

**The current surface** (stdlib §4.10):

```bezel
  public method Take(query: T): owned T;               // borrow-taking query;
  public method TryTake(query: T, out owned removed: T?): bool;
```

with the prose *"`Take(query: T)` finds the equal element in the set, removes it, and returns ownership. … `TryTake` is the no-throw variant."*

**The ground for the rename is measured shadowing, not taste.** Spec §26.5 rule 1: *"If `x`'s declared type (or any base type) **declares** any instance member named `Foo` — of any arity, applicability, or accessibility — instance resolution is committed and extension blocks are **never consulted** for that name on that receiver"* *(the bold on "declares" is added; "never consulted" is the source's own)*. LINQ's `Take(count: int): IEnumerable<T>` is an extension on `IEnumerable<T>`. So on an `OwningSet<T>` receiver, `Take` is the owning-egress member and **LINQ's `Take` is unreachable, by name, forever**. The rename to `Pop`/`TryPop` un-shadows it and puts the member in the family the guides already teach: Guide 2's *"`Get`/`Peek` borrow; `Pop`/`Drain`/`Take` transfer"*, and `OwningStack<T>`/`OwningQueue<T>`'s existing `Pop()`/`TryPop(out owned item: T?)`.

**Re-verified call-site count — the charter's "0 call sites" is wrong.** Measured at HEAD across `tests/`:

| Site | Count | Files |
|---|---|---|
| `s.Take(…)` / `s.TryTake(…)` on an `OwningSet` receiver, in Bezel source | **10** | `tests/codegen/owned_set_take_1476_test.cpp` (9: six `Take`, three `TryTake`), `tests/sema/builtin_collection_nullable_arg_2582_test.cpp` (1) |
| `src/` occurrences of the string literals `"Take"` / `"TryTake"` (name-keyed recognition) | **27** | 8 files: `sema/ExprTypecheck_Calls.cpp`, `sema/ExprTypecheck.cpp`, `sema/ExprTypecheck_Exprs.cpp`, `common/OwnershipFacts.cpp`, `ownership/Ownership_Checks.cpp`, `codegen/LlvmBackend_Calls.cpp`, `LlvmBackend_Calls2.cpp`, `LlvmBackend_Calls3.cpp` |
| LINQ `Take` call sites in the **same** test files that must **not** change | 3 | `arr.Take(2)` (2582), `a.Take(3).ToArray()` and `a.Take(2)` as a `for` source (1476) |
| The null-conditional LINQ control, in a **third** file | 1 | `arr?.Take(2)` — `tests/sema/builtin_collection_nullcond_arg_2594_test.cpp`, the #2594 null-conditional-argument pin; outside the two rename files but keyed on the same name split, so the rename lane verifies it unchanged too |

So the rename is **not free**, and the `src/` half is **not mechanical**: the compiler distinguishes the two `Take`s by receiver family today — the #2582 family gate is a *negative* exclusion inside the lazy-LINQ typing path, quoted from `src/sema/ExprTypecheck_Calls.cpp`:

```cpp
if (m == "Where" || m == "Skip" ||
    (m == "Take" && colName != "OwningSet") ||
    m == "Distinct") {
```

— LINQ's `Take` is *every* `Take` **except** the `OwningSet` one, so after the rename the exclusion clause becomes dead (no instance `Take` remains to exclude) and the `Pop` recognition must distinguish `OwningSet`'s `Pop(query: T)` from `OwningStack`/`OwningQueue`'s `Pop()` by **arity** where today the name plus family sufficed. The graduation train's implementation issue must say so; a regex rename would silently mis-key the gate. *(This is the same shape as proposal 0091's `WhenAll` re-keying: name-keyed recognition becomes name-plus-signature recognition.)*

**`OwningDictionary<K, V>.PopValue`/`TryPopValue` stay, with their reason stated.** stdlib §4.9's own inline comment already records it (*"forms remain PopValue/TryPopValue"*): the `Value` suffix **carries information** — the dictionary's egress transfers the *value* and consumes the *key* as a query, so `PopValue(key)` says which of the two the caller gets, where a bare `Pop(key)` would not. That is a naming-carries-information ground, not an exception to the rename; `OwningSet<T>` has no such ambiguity (its element and its query are the same type and the same thing), which is exactly why its member can lose its qualifier.

**The `TryPopAt` gap, recorded with band reasoning.** `OwningArray<T>` declares `PopAt(index): owned T` (raising), `PopFirst()`, `PopLast()`, `TryPopFirst(out owned item: T?)`, and `TryPopLast(out owned item: T?)` — but **no `TryPopAt`**. Read against stdlib §1's three access tiers, the surface is asymmetric: the positional egress has a throwing form and no Try form, while its two convenience specializations (first/last) have both. Under §1's failure-signal rule an out-of-range index is arguably the *assert-shaped* tier (a bug, like the throwing indexer) rather than the branch-shaped tier — which is a coherent reason for the absence, and is presumably why v0.171 stopped where it did. But the same argument applies to `PopFirst`/`PopLast` on an empty collection, and those *did* get Try forms. **Recorded as a gap with the reasoning on both sides, not closed here**: closing it means either declaring `TryPopAt(index: int, out owned item: T?): bool` (shape (a), the `when true` postcondition riding it per §1) or writing the band sentence that says positional egress is deliberately assert-only. This proposal does neither — it records that the surface currently asserts neither, which is the defect worth naming.

**Migration:** breaking rename, zero external users, hard flip with no alias (the 0090 Q3 / 0091 precedent). Old names leave the stdlib in the same revision. Guides 02 and 05 lose `Take` from their transfer-name lists and gain nothing (`Pop` is already there).

## 3. `Sum`/`Average` migrate to `INumber<T>` (adoption iii) — with an honest split

**The recorded follow-up, quoted.** stdlib §5: *"`Sum` and `Average` are provided as per-type overloads rather than as a single generic constrained on a numeric interface. These overloads predate the `INumber<T>` abstraction (§2.19); migrating them to a single constrained generic is **recorded follow-up work**."* And stdlib §5.1, of the no-selector forms: *"The per-type pattern follows the selector-based overloads and exists for the same reason: **no generic numeric constraint in this version**."*

That stated reason is **false at v0.255**. `INumber<T>` has existed since v0.176 (stdlib §2.19), the whole primitive numeric tower conforms via compiler-provided conformances, and spec §11.8's own normative example is `public static method Sum<T>(items: Array<T>): owned T where T: INumber<T>`. The stdlib is carrying an overload set whose recorded justification the language removed three dozen revisions ago. This is the pre-1.0 cheap moment to collect it.

**The overloads at stake, counted:**

| Family | Declared in the snapshot | Stated-but-not-declared | Total |
|---|---|---|---|
| `Sum(selector)` (§5) | 7 — `int`, `long`, `uint`, `ulong`, `float`, `double`, `decimal` | — | **7** |
| `Average(selector)` (§5) | 7 — same set | — | **7** |
| `Sum()` no-selector (§5.1) | 4 blocks shown (`int`, `long`, `double`, `decimal`) | 3 more stated in prose (`uint`, `ulong`, `float`) | **7** |
| `Average()` no-selector (§5.1) | 4 blocks shown | 3 more stated in prose | **7** |
| | | | **28** |

**`Sum` migrates cleanly.** `INumber<T>` supplies `Zero()`, `One()`, `+`, `-`, `*`, `/`, and `<` — an accumulator over `Zero()` and `+` is exactly §11.8's own example, and spec §28.3 has already verified that shape's ownership at every substitution (*"the accumulator shape of §11.8's `Sum<T>` … is well-formed at every substitution: stdlib §2.19's producer members and `Sum`'s own return are spelled `owned T`, admissible over the mixed numeric domain under §28.1's per-instantiation erasure"*). The 14 `Sum` overloads collapse to two:

```bezel
public extension IEnumerable<T>
  public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
end;

public extension IEnumerable<T> where T: INumber<T>
  public method Sum(): owned T;
end;
```

**`Average` does not migrate cleanly, and the graduation train must not pretend otherwise.** Two obstructions, both verified against the snapshot:

1. **The result type is not the element type.** The declared overloads are `Average(selector: (T) -> int): double`, `(long) -> double`, `(uint) -> double`, `(ulong) -> double`, `(float) -> float`, `(double) -> double`, `(decimal) -> decimal`. Five of the seven **widen**: the integer families average to `double`. A single `Average<U>(): U where U: INumber<U>` would return `int` for an integer source — a silently truncating average, the worst possible outcome. A single `Average<U>(): double` would lose `decimal`'s exactness and `float`'s type, both of which the current surface deliberately preserves (v0.195 added the `float`→`float` and `ulong`→`double` forms precisely to complete this mapping).
2. **Dividing by the count is expressible — at the price of a second constraint.** Averaging requires `total / n` where `n: int`. `INumber<T>` declares `/` over `(T, T)` only and offers no `int`→`T` conversion, so within `INumber<U>` alone the operation is unwritable. But the conversion **does** exist one interface over: `ISteppable<T>` (stdlib §2.19) declares `static method Step(value: T, by: long): owned T` — *advance in T's space* — so `U.Step(U.Zero(), count)` produces the count as a `U` in O(1), and the whole primitive numeric tower conforms to both interfaces. A generic average is therefore expressible at `where U: INumber<U>, ISteppable<U>`. The objection moves from **impossibility to signature shape**: `Average` would carry a second constraint that `Sum` does not, its meaning stretched (`Step` is an ordinal-advance operation drafted for ranges, here pressed into service as a from-integer conversion), and every user conformer wanting `Average` must implement `ISteppable<U>` — an iteration vocabulary — to unlock an arithmetic aggregation.

**Obstruction 1 alone still blocks.** Whatever the constraint vocabulary, a single `Average<U>(): U` truncates integer averages and a single `(): double` loses `decimal` and `float` — the result-type mapping is the surface's deliberate content, and no constraint expresses it. **Disposition unchanged:** adopt the migration **for `Sum` (14 overloads → 2)**; **record `Average` as blocked** — primarily on the result-type mapping (obstruction 1), secondarily on the constraint-shape cost of the now-expressible division (obstruction 2) — and leave its 14 overloads in place. That is the truthful version of C7(iii): the ruling's "pre-1.0 cheap moment" is real for `Sum` and is not available for `Average` at an acceptable signature. The stdlib §5 sentence that today says the migration is "recorded follow-up work" is rewritten to say what actually happened: `Sum` migrated; `Average` awaits a result-type facility on `INumber<T>` (an associated result type — the same associated-types gap rationale §18 already records for traits), the division half being expressible via `ISteppable<U>` but not worth the second constraint on its own. *(§9 Q1 puts the alternative to the graduation train.)*

## 4. The exception-safety post-state convention band (adoption iv)

**The problem.** Several stdlib mutators re-enter user code mid-operation: a comparator (`Sort`, `Sorted`, the sorted collections' insertion path), `Equals`/`GetHashCode` (every hashed container's `Add`/`Remove`/`Contains`/indexer-set), a key selector or predicate (the eager LINQ operators over a mutating receiver). If that user code **throws**, the container is caught mid-mutation. What state is it in? The stdlib says nothing, per-member or otherwise — and saying it per-member would be dozens of near-identical sentences.

Every band in stdlib §1 exists for exactly that shape (*the Try convention*, *Negative magnitudes fault*, *The `Async` suffix*, *Borrowing/owning duals are overloads*): a promise stated once, applying document-wide, elided from the per-type listings.

**The drafted band** (stdlib §1, appended to the bullet list, in the register of its siblings):

> - **Exception safety when a mutator re-enters user code (v0.2xx).** Some mutating members call back into user code while the receiver is mid-mutation: a comparator (`Sort`, `Sorted`, and the sorted collections' insertion path), `Equals`/`GetHashCode` (the hashed containers' `Add`, `Remove`, `Contains`, and indexer-set), and a caller-supplied predicate, selector, or key function on any member that takes one. If that user code raises, the receiver is left in a state satisfying this convention, stated once here rather than per member. **The receiver remains valid and usable** — every invariant its own type states still holds, every element it still holds is intact, and no element is left half-transferred: on an **owning** container the dispose-exactly-once obligation is preserved for every element, either still held by the container or already transferred to the caller, never both and never neither. **The receiver's contents are otherwise unspecified**: a partially-applied ordering, a partially-completed insertion or removal, or a partially-drained sequence is admitted, and no member promises the strong (all-or-nothing) guarantee unless its own text says so. A caller that needs all-or-nothing operates on a copy and installs the result. This is the basic guarantee, not the strong guarantee, and it is a convention of this document — the language imposes it on no user type.

**Why "basic, not strong", and why stated as a band.** The strong guarantee would require every such member to build a shadow copy — for an *owning* container that means duplicating owned elements, which is the very thing stdlib §4.5 says the owning family cannot do (*"`OwningArray<T>` deliberately has **no** value copy-forms … a copy would either double-own each element or drain the source"*). So the strong guarantee is not merely expensive here, it is structurally unavailable on half the surface. The basic guarantee *is* available and is what the exactly-once invariant already forces: spec §28.4's multi-value-cleanup rule and §28.7's drop-on-overwrite already guarantee that no element is lost or doubled on an exception edge. The band therefore **states a consequence of existing language rules as a library promise** rather than adding a new obligation — which is why it costs nothing to implement and everything to omit.

## 5. Three truth repairs

**(a) stdlib §9.7's LINQ-applicability claim is false, measurably.** It reads, in full: *"In addition to the methods declared on `Array<T>` itself (§4.4), the LINQ extensions in §5 apply to `Array<T>` because it implements `IEnumerable<T>`."* Under spec §26.5 rule 1 (block by *name*), any LINQ operator whose name collides with a declared instance member of `Array<T>` is unreachable on an `Array<T>` receiver. Measured against §4.4's declared members and §5's operator set: **`Contains`, `Count`, `Reverse`, and `ToArray` are blocked** — four operators, of which `Count` is the highest-traffic member in the whole LINQ surface (`arr.Count(pred)` does not resolve to LINQ on an `Array<T>`). The repair is a narrowing clause naming the blocked set and pointing at §26.5 rule 1 and at §1's new shadowing note (b).

**(b) The block-by-name shadowing note (stdlib §1, new).** §9.7's defect is an instance of a document-wide fact, so it is stated once. Measured across the stdlib's collection and span sections, the LINQ names blocked by a same-named declared instance member:

| Receiver (stdlib §) | LINQ names blocked by name |
|---|---|
| §4.1 borrowing collection interfaces | `Contains`, `Count` |
| §4.2 owning collection interfaces | `Contains` |
| §4.4 `Array<T>` | `Contains`, `Count`, `Reverse`, `ToArray` |
| §4.5 `OwningArray<T>` | `Contains`, `Count`, `Reverse` |
| §4.6 `LinkedList<T>` | `Count`, `First`, `Last` |
| §4.7 `OwningLinkedList<T>` | `Contains`, `Count`, `First`, `Last` |
| §4.8 `Dictionary<K, V>` / §4.9 `OwningDictionary<K, V>` | `Contains`, `Count` |
| §4.10 `Set<T>` / `OwningSet<T>` | `Take` (item 2 removes this one) |
| §4.11 `Stack`/`OwningStack`/`Queue`/`OwningQueue` | `Contains` |
| §4.14 `OwningBuffer<T>` at class `T` — the section's only `IEnumerable` implementer (`IEnumerable<T?>`) | **none** — `Length`, the indexer, and the `AsSpan`/`AsReadOnlySpan` families collide with no §5 name; the row's former `Contains`/`Reverse`/`ToArray` belonged to the span types, resolved moot below |
| §4.17 `SortedDictionary<K, V>` | `Contains`, `Count` |
| §4.18 `SortedSet<T>` | `Contains`, `Count`, `First`, `Last` |
| §4.19 `FrozenArray<T>` | `ToArray` |
| §4.20 `Counter<K>` | `Count` |

*(Measured by name collision between §5's operator set and each section's declared `method`/`property` names, including `pure readonly property` forms. The `Set<T>`/`OwningSet<T>` row is section-level: `Take` is declared on `OwningSet<T>` only.)*

**The span family is resolved moot, not deferred.** `Span<T>`/`ReadOnlySpan<T>` (§4.14), `ArraySpan<T>`/`ReadOnlyArraySpan<T>` (§4.15), `IReadOnlySpan<TElement>` (§4.16), and `ReadOnlyStringSpan` (§3.3) implement no `IEnumerable<T>`: spec §29.2 iterates every one of them by a **direct indexed loop** over the span's range (the inline-array rule's parallel), and the value-`T` `OwningBuffer<T>` form is declared with *"no IEnumerable conformance"* outright. §26.5's block-by-name rule arbitrates between an instance member and an *applicable* extension — on a receiver where the LINQ extensions never apply, there is nothing to block, so those four rows (and their `Contains`/`Reverse`/`ToArray`/`Sum` name lists) drop from the table rather than await verification.

The note itself is short: an instance member blocks a same-named extension **by name**, so where a collection declares its own `Count`, `Contains`, `First`, `Last`, `Reverse`, or `ToArray`, that member — not the LINQ operator — is what a call resolves to; the semantics are usually equivalent (`Array<T>.Contains` and LINQ's `Contains` agree) and the differences, where they exist, are the member's own documented behavior. The value of stating it is that a reader who expects `arr.Count(pred)` to work now finds out why it does not.

**(c) The §26.4 → §26.5 mis-cite.** stdlib §5's `Reverse` naming note reads: *"on an `Array<T>` receiver, instance resolution commits to the in-place `Reverse()` before extensions are consulted (spec **§26.4** rule 1)."* Spec §26.4 is *Generic extensions* and has no rules 1–4; the instance-members-win rule is **§26.5 rule 1**. One-character class of repair, verified: the only two §26.x citations in the stdlib are this one and a correct §26.1.

## 6. `CountWhere` — declined, evidence-gated (the decline)

The proposal on the table was a distinctly-named predicate-count operator, `CountWhere(predicate)`, alongside LINQ's `Count(predicate)`.

**The motivation is real and is measured in §5(b): ten stdlib types declare a `Count` property, so on every one of them LINQ's `Count()` *and* `Count(predicate)` are unreachable by §26.5 rule 1.** `CountWhere` would be reachable everywhere, because no collection declares it.

**Declined for now, with the reasons recorded.**

1. **It fixes one name out of six.** The same blocking hits `Contains`, `First`, `Last`, `Reverse`, and `ToArray` (§5(b)'s table). A `CountWhere` that leaves `FirstWhere`/`LastWhere` undeclared trades one inconsistency for a worse one; a full `*Where` tier is a naming programme, not a member.
2. **The workaround is one call and is already idiomatic.** `coll.Where(pred).Count()` composes the two operators the collision separated, and `Where` is not shadowed anywhere in §5(b)'s table.
3. **No evidence of the friction.** Zero corpus uses, zero filed issues; the collision was discovered by reading §26.5 against the member lists, not by hitting it.
4. **The general remedy is recorded and better.** Rationale §18's *Extension-call disambiguation syntax* deferral is the shape that fixes all six names at once; a per-name workaround member spends the vocabulary that design would want.

**The reopening bar:** field evidence that the block-by-name collision costs real users real time on the `Count`/`Contains` names specifically — at which point the question is the `*Where` tier or the disambiguation syntax, not `CountWhere` alone.

## 7. Migration and compatibility (changelog house categories)

- **Newly defined:** `ElementAtOrNull` (stdlib §5); the two `INumber<T>`-constrained `Sum` forms; stdlib §1's exception-safety band and block-by-name note.
- **Renames (breaking; zero users; hard, no alias — the 0090 Q3 precedent):** `OwningSet<T>.Take` → `Pop`, `TryTake` → `TryPop`. **Not mechanical in `src/`** (§2).
- **Removed:** 14 `Sum` overloads (7 selector + 7 no-selector, of which 3 + 3 were prose-stated rather than declared).
- **Newly warned:** `ElementAtOrNull` joins §29.11's terminal-accessor provenance warning set.
- **Doc-only:** the §9.7 narrowing, the §26.4→§26.5 repair, the `PopValue` reason, the `TryPopAt` gap record, the `CountWhere` decline, and the `Average` blocked-migration record.
- **Unchanged:** every `Average` overload; `OwningDictionary.PopValue`/`TryPopValue`; every element accessor's ownership classification; every Try shape.

## 8. Implementation sketch

- **sema/codegen:** `ElementAtOrNull` joins the compiler-known terminal-accessor set (the same list that carries `ElementAt` for the §28.3 bare-type-parameter classification and the §29.11 warning). The `OwningSet` rename re-keys the recognition sites named in §2 from *name + collection family* to *name + arity + family*, because `Pop` now exists on three owning types with two different arities. The `Sum` migration replaces seven per-type recognitions with one constraint-checked generic; §11.8's static-interface dispatch already supports it.
- **stdlib/tests:** the 10 `OwningSet` call sites respell; the 3 LINQ `Take` sites in the same files, plus the `arr?.Take(2)` control in `builtin_collection_nullcond_arg_2594_test.cpp`, must be verified unchanged (they are the family-gate controls and would silently pass a bad rename).
- **Spec-cited regression tests** (`runAot`): `ElementAtOrNull` in range / out of range / at a nullable element type (the §7.10 conflation caveat) / drawing the §29.11 warning over a same-unit fresh-yield iterator; `OwningSet.Pop`/`TryPop` accept and the old names reject, plus `set.Take(2)` now resolving to LINQ on an `OwningSet` receiver (the un-shadowing, which is the rename's whole point and needs its own pin); `Sum()` and `Sum(selector)` at each primitive conformer and at a user `INumber<T>` conformer, plus the rejection at `string`/`bool`/`char` (stdlib §2.19's non-conformers); an exception-safety test per band clause — a throwing comparator in `Sort` and a throwing `GetHashCode` in an owning container's `Add`, each asserting the receiver is still usable and each element disposed exactly once under `--full`.

## 9. Drafting seams for the graduation train

- **Q1 — `Average`.** Recommended: leave the 14 overloads and record the blocking reasons (§3). The alternative the drafter may prefer — declare `Average<U>(selector: (T) -> U): double where U: INumber<U>` plus keep `decimal`/`float` as per-type overloads — collapses 5 of 7 selector forms but leaves a mixed surface whose rule ("integers and `double` go generic, `decimal` and `float` stay per-type") is harder to state than the current uniform per-type table. If the drafter takes it, the §5.1 sentence must say *why* the surface is mixed.
- **Q2 — where the exception-safety band lives.** Recommended: stdlib §1, as drafted, since it is a document-wide convention in the register of its siblings. The alternative is spec §31 as a language-level statement — declined in the draft above because the language imposes nothing here (a user type may offer any guarantee it likes) and §1 is where "this document's members promise X" belongs. The band's last sentence says so explicitly; if the graduation train moves it, that sentence must move with it.
