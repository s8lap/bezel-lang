# 0119 — Stdlib symmetry batch: dictionary `TryAdd`, the window-parameter convention, and the comparer-injection ruling

**Status:** graduated-v0.282 — superseded — graduated v0.282 (owner-directed 2026-08-25). Landed with one gate correction to this proposal's own §1: the restored `SortedDictionary` insert tier mirrors `Dictionary` (§4.8) — throwing `Add`/`TryAdd`/indexer upsert — not `SortedSet`, whose `Add` discards silently; the v0.282 changelog records it.
**Tracking issue:** [#3369](https://github.com/s8lap/bezel-lang/issues/3369)
**Target module(s):** stdlib [§1](../Bezel_Standard_Library.md#sec-conventions) (one convention paragraph), [§4.8](../Bezel_Standard_Library.md#sec-dictionaryk-v)/[§4.9](../Bezel_Standard_Library.md#sec-owningdictionaryk-v)/[§4.17](../Bezel_Standard_Library.md#sec-sorted-dictionary) (members), [§2.7](../Bezel_Standard_Library.md#sec-string)/[§3.2](../Bezel_Standard_Library.md#sec-encoding) (parameter renames), rationale (one ruling record). **No language-spec rule changes. Compiler catch-up issues filed at graduation.**
**Written against:** v0.279

All three items come from the 2026-08-25 elegance review, each verified before proposing: the review's factual claims were checked against the documents, and its "unrecorded" statuses were checked against `DECLINED.md`, the rationale, the changelog, and every proposal in this folder. What follows survived that check.

## 1. `TryAdd` for the dictionaries (additive API)

**The asymmetry.** `Set<T>.TryAdd(item: T): bool` (§4.10), `OwningSet<T>.TryAdd(owned item: T): bool` (§4.10), and `SortedSet<T>.TryAdd(item: T): bool` (§4.18) all exist — the sanctioned §1 shape-(d) bool-only attempt. No dictionary has `TryAdd`. The absence is recorded nowhere (swept: `DECLINED.md`, rationale, changelog, proposals 0001–0118); the code-generation reference documents it in its not-exists list without a reason — the only entry there for which no principled ground exists.

**Why it costs.** `OwningDictionary<K, V>.Add(key: K, owned value: V)` raises `ArgumentException` on a present key **and disposes the passed value on that path** (§4.9); the documented remedy is pre-checking `ContainsKey` — two hash lookups, and the caller keeps the value only by not calling `Add`. This is exactly the situation `OwningSet<T>.TryAdd` exists to solve, per that member's own stated reasoning ("for callers that want to know whether the add succeeded"). Note the sets' *other* stated ground — §11.2's exact-return-type conformance on an inherited interface `Add` — does **not** transfer: `Dictionary.Add(key, value)` is not an interface override. `TryAdd` here stands on shape (d), the insert-only-and-report stance the doc already distinguishes from the indexer's insert-or-overwrite, and the set precedent's naming.

**Proposed members** (duplicate key ⇒ `false`, no mutation; shape (d), no out, no nullability postcondition):

```
Dictionary<K, V>:        public method TryAdd(key: K, value: V): bool;
OwningDictionary<K, V>:  public method TryAdd(key: K, owned value: V): bool;   // on false, the passed value is disposed — the OwningSet<T>.TryAdd contract verbatim
SortedDictionary<K, V>:  public method Add(key: K, value: V);                  // ArgumentException if the key is present
                         public method TryAdd(key: K, value: V): bool;
```

`SortedDictionary` today has **no** `Add` at all — insert is indexer-only — so the *duplicate-is-a-bug* tier does not exist there while `SortedSet` keeps all three insert stances. This batch restores the tier so the sorted family is internally consistent; `Keys`/`Values` stay absent with the reason finally recorded in §4.17 (iteration yields entries in key order; a one-line loop substitutes), per the review's alternative.

**Migration:** zero — all additive, no name taken. **Compiler:** catch-up issue filed at graduation (the dictionary builtins gain members).

## 2. The window-parameter convention (parameter renames, recorded rule)

**The drift.** Three spellings of "a sub-range" coexist: `(start, length)` on every span's `Slice`/`AsSpan` and on `Substring`/`StringBuilder.Remove`; `(offset, count)`/`(index, count)` on `Stream.Read`/`Write` and `Encoding.GetString`; and `String.From(buffer: Array<char>, start: int, count: int)` mixing the two families in one signature. Within `String` itself: `Substring(startIndex, length)` beside `Remove(startIndex, count)`. Parameter names are API surface (named arguments); no recorded convention governs them.

**The rule, recorded in §1:** a window is **`(start, length)`** — `startIndex` stands as the string family's established spelling of the start — except the **`Stream` byte-window family**, which keeps `(offset, count)` deliberately (the familiarity doctrine: `Stream.Read(buffer, offset, count)` is the one signature C# muscle memory types unprompted).

**The renames** (three, all rename-only; positional call sites unaffected, named-argument call sites exist only in this repo's own tests — compiler catch-up issue filed at graduation):

```
String.From(buffer: Array<char>, start: int, count: int)      →  (buffer: Array<char>, start: int, length: int)
String.Remove(startIndex: int, count: int)                    →  (startIndex: int, length: int)
Encoding.GetString(bytes: Array<byte>, index: int, count: int) →  (bytes: Array<byte>, start: int, length: int)
```

`RawBytes.CopyTo(destination, destOffset)` is untouched: `destOffset` names a write *position*, not a window. **Cost of not doing this now:** the renames are free before external users exist and breaking after; this is the entire argument for doing it at v0.282 rather than "eventually".

## 3. The comparer-injection ruling (recorded, no API change)

**The tension, verified:** the hash containers take `constructor(comparer: IEqualityComparer<K>)` (§4.8, §4.9, §4.10) while the sorted containers refuse ordering comparers on the recorded ground that "the single-order rule keeps equality, ordering, and hashing in one place, on the type" (§4.17; `DECLINED.md`, *Comparer parameter on the sorted collections*). Neither text cites the other, and the sorted family's stated principle, read alone, indicts the hash family's constructors.

**The ruling:** both stand, because they are not the same operation. An `IEqualityComparer<K>` carries **both** facets a hash container consults — `Equals` and `GetHashCode` — as one object, so injection relocates the container's identity *whole*: the two functions that must agree travel together, and the type's `CompareTo` is never consulted, so no observable facet can disagree with another. An injected *ordering* comparer would do the opposite: the container would find by the instance's order while the element's own equality remains the type's, **splitting identity between the instance and the type** — `SortedSet` identity *is* `CompareTo`-equality (§4.18), so the split is directly observable in `Contains`/`TryAdd`. The single-order rule's real content is "identity must not fork", and the equality-comparer constructors satisfy it; the wrapper-record workaround remains the sorted family's answer. **Landing shape:** one rationale record stating this; §4.8 and §4.17 each gain a one-line cross-reference to it. EDITORIAL.

## 4. Out of scope (checked, already recorded elsewhere)

`MinBy`/`MaxBy` (proposal 0026, under review); `LastIndexOf(item, startIndex)` and `OwningArray` query parity (declined/deferred, proposal 0067 §4 + v0.228 changelog); `Environment.CurrentDirectory` (proposal 0013, accepted); `PriorityQueue` (proposal 0024); streaming/append text I/O (proposals 0006/0018 — the sync-append seam between them is worth one sentence in 0006 at its implementation run, not a new proposal); `TryPopAt` and the `DangerousPointer` subtraction (routed: the §14.2 register's own exits, the latter at the v0.283 subtraction review).
