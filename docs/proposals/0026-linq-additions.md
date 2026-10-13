# Proposal 0026 — LINQ additions: indexed `Select`/`Where`, `Chunk`, `MinBy`/`MaxBy`

| | |
|---|---|
| **Status** | under-review — draft |
| **Tracking issue** | [#1457](https://github.com/s8lap/bezel-lang/issues/1457) |
| **Target module** | `System.Collections.Linq` (§5; **auto-imported since v0.278** — spec §2.3 carries the module row under the invariant *where the stdlib declares an extension over an always-available type, its module is auto-imported*, so no `import System.Collections.Linq` line is needed and the spec's own LINQ examples carry none) |
| **Written against** | spec v0.177 / stdlib v0.165 |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); refreshed 2026-09-17 to v0.306: the Target-module row's "requires `import System.Collections.Linq` — not auto-imported" claim corrected — §5 has been **auto-imported since v0.278** and the spec now carries zero `import System.Collections.Linq` lines. The addendum's `Take`/`Skip` clamping citations, the §29.11 `ToArray()` re-enumeration remedy, and `TakeLast`/`SkipLast(count: int)`'s parameter name (matching live `Take`/`Skip`) re-verified. No tier-2 flags. Written-against pin unchanged. |
| **Gap-review item** | #18 (P1) in [stdlib-gap-review-2026-07.md](papers/stdlib-gap-review-2026-07.md) |
| **Wave** | D3 — library breadth, per [stdlib-gap-design-plan-2026-07.md](papers/stdlib-gap-design-plan-2026-07.md) |

Three additive fills to the `System.Collections.Linq` extension surface.  Every addition is on `IEnumerable<T>`; none touches `IOwningEnumerable<T>` (gap #29, deferred).

---

## 1. Design decisions

### D1 — Indexed `Select` / `Where` overloads

**Decision.** Add two overloads that expose a zero-based element index to the lambda:

```bezel
public method Select<U>(selector: (T, int) -> U): IEnumerable<U>;
public method Where(predicate: (T, int) -> bool): IEnumerable<T>;
```

These are **lazy** and **single-consumption** (spec §29.11), exactly as the existing non-indexed forms (stdlib §5). The index is zero-based and increments by one per element in source order. Both overloads are additions alongside the existing `Select<U>((T) -> U)` and `Where((T) -> bool)`; they do not replace them. Bezel's overload resolution distinguishes the two by lambda arity — a one-argument lambda selects the existing form; a two-argument lambda selects the indexed form. The `owned` overload-ban (spec §22.6, v0.169) does not apply here because the parameter types differ structurally (`(T) -> U` vs `(T, int) -> U`).

No alternative lambda-type conventions were considered; the `(T, int) -> U` form exactly matches the convention used by all other two-argument lambdas in the stdlib (e.g. `Aggregate`'s `(TAccum, T) -> TAccum`).

### D2 — `MinBy` / `MaxBy` and their `OrNull` forms

**Decision.** Add four methods:

```bezel
public method MinBy<TKey>(key: (T) -> TKey): T where TKey: IComparable<TKey>;
public method MinByOrNull<TKey>(key: (T) -> TKey): T? where TKey: IComparable<TKey>;
public method MaxBy<TKey>(key: (T) -> TKey): T where TKey: IComparable<TKey>;
public method MaxByOrNull<TKey>(key: (T) -> TKey): T? where TKey: IComparable<TKey>;
```

These return the **element** `T` whose key is minimal or maximal — not the key itself. The existing `Min<U>(selector: (T) -> U): U` and `Max<U>(selector: (T) -> U): U` return the projected **value**; the naming matches the established .NET distinction and is documented explicitly.

Empty-source behavior follows the `First`/`FirstOrNull` and `Min`/`MinOrNull` precedents (stdlib §5.1):
- `MinBy`/`MaxBy` throw `InvalidOperationException` when the source is empty.
- `MinByOrNull`/`MaxByOrNull` return `null` when the source is empty.

When the element type is itself nullable (`T?`), the `OrNull` form cannot distinguish "empty source" from "a present `null` element" — the same caveat documented for `FirstOrNull` in §5 applies equally here. Callers who need that distinction use `Count()` or `Any()` before calling `MinByOrNull`.

Tie-breaking: when two elements project to equal keys (per `IComparable<TKey>.CompareTo`), the **first** such element in source order is returned, consistent with the stable-first convention of `OrderBy`.

### D3 — `Chunk`

**Decision.** `Chunk` is **eager** and returns `owned OwningArray<Array<T>>`:

```bezel
public method Chunk(size: int): owned OwningArray<Array<T>>;
```

**Why eager, not lazy.** A lazy `Chunk` returning `IEnumerable<Array<T>>` would yield borrows of chunk arrays from the enumerator's internal state. Those borrows would become dangling the moment the enumerator advanced past each chunk. Composing the lazy form with `ToArray()` (the idiomatic materialization) would then populate an `owned Array<Array<T>>` whose elements point into an already-disposed enumerator — a use-after-dispose. The correct lazy variant would require yielding *owned* sub-arrays through a lazy interface, which is the owning-enumerable pattern of gap #29 (`IOwningEnumerable<T>` LINQ). Eager Chunk is sound today without that machinery.

**Return type.** `owned OwningArray<Array<T>>` — the outer `OwningArray` owns each chunk, disposing each `Array<T>` when the outer container is disposed. `OwningArray<T>` requires `T: class`; `Array<T>` is always a class (a heap reference type) for any element T, so the constraint is satisfied unconditionally. The inner `Array<T>` chunks hold borrowed element references (for class-typed T) or value copies (for value-typed T), exactly as any `Array<T>` does — the same semantics as the result of `ToArray()`.

**Precondition.** `size` must be positive; `size <= 0` is a defined fault (raises `ArgumentException`). The last chunk may contain fewer than `size` elements if the source does not divide evenly.

**Interaction with the #1334 axis.** These additions operate entirely on the borrow-element (non-owning) path. `OwningArray<T>` exposes `IEnumerable<T>` for borrowing iteration, so `Chunk` — like all §5 operators — is available on owning collections via their `IEnumerable<T>` conformance. The result `OwningArray<Array<T>>` is non-owning at the element level of the *inner* arrays: they hold borrows/values from the *source* collection, not from the outer OwningArray. The outer OwningArray owns the chunk containers, not their contents.

---

## 2. Proposed stdlib-section text (new §5.2)

Add the following subsection to stdlib §5 after §5.1:

---

<a id="sec-linq-additions-0026"></a>
### 5.2 Indexed projection and filtering; MinBy/MaxBy; Chunk

```bezel
public extension IEnumerable<T>
  // Indexed overloads (element + zero-based position)
  public method Select<U>(selector: (T, int) -> U): IEnumerable<U>;
  public method Where(predicate: (T, int) -> bool): IEnumerable<T>;

  // Return the element whose key is minimal/maximal
  public method MinBy<TKey>(key: (T) -> TKey): T where TKey: IComparable<TKey>;         // InvalidOperationException if empty
  public method MinByOrNull<TKey>(key: (T) -> TKey): T? where TKey: IComparable<TKey>;  // null if empty
  public method MaxBy<TKey>(key: (T) -> TKey): T where TKey: IComparable<TKey>;         // InvalidOperationException if empty
  public method MaxByOrNull<TKey>(key: (T) -> TKey): T? where TKey: IComparable<TKey>;  // null if empty

  // Partition into fixed-size sub-arrays (eager)
  public method Chunk(size: int): owned OwningArray<Array<T>>;                           // ArgumentException if size <= 0
end;
```

**Indexed `Select` and `Where`.** The two-argument lambda receives `(element, index)` where `index` is zero-based and increments by one per element from the source. Both operators are lazy and single-consumption (spec §29.11), identical to the non-indexed forms. One-argument and two-argument overloads coexist; lambda arity determines which is called.

```bezel
// Number lines of a string array.
var numbered := lines.Select(lambda(line, i) => $"{i + 1}: {line}");

// Keep only even-positioned elements.
var evens := items.Where(lambda(_, i) => i mod 2 = 0);
```

**`MinBy` / `MaxBy` / `MinByOrNull` / `MaxByOrNull`.** Return the **element** (type `T`) whose projection under the key selector is minimal or maximal, evaluated with `TKey.CompareTo`. Contrast with the existing `Min<U>(selector)` / `Max<U>(selector)` (stdlib §5), which return the **key value** `U`. When multiple elements project to the equal minimum or maximum key, the first in source order is returned (stable-first). On an empty source, the non-`OrNull` forms throw `InvalidOperationException`; the `OrNull` forms return `null`.

```bezel
var earliest := events.MinBy(lambda(e) => e.Timestamp);          // first event by time
var longestOrNone := words.MaxByOrNull(lambda(w) => w.Length);   // null if words is empty
```

**`Chunk`.** Partitions the source into consecutive sub-arrays each of `size` elements; the final chunk contains the remainder elements when the source length does not divide evenly. Eager: all chunks are allocated before the method returns. The result is `owned OwningArray<Array<T>>` — the outer `OwningArray` owns and will dispose each inner `Array<T>` chunk container; the inner arrays hold borrowed (for class T) or copied (for value T) elements from the source, identically to `ToArray()`.

```bezel
using var chunks := [1, 2, 3, 4, 5, 6, 7, 8, 9, 10].Chunk(3);   // an Array<int> literal (§15.4) is IEnumerable<int>
// chunks contains [1,2,3], [4,5,6], [7,8,9], [10]
for var chunk in chunks do
  ProcessBatch(chunk);
end;
```

A lazy `Chunk` variant (producing `IEnumerable<Array<T>>`) is deferred: it requires yielding owned sub-arrays through a lazy interface, which is the `IOwningEnumerable<T>` LINQ pattern (gap #29).

---

## 3. Declined alternatives

- **`SelectMany` with index** — not included; the use case is covered by `Select(...).SelectMany(...)` and does not appear in the gap review.
- **`MinBy`/`MaxBy` on `IOwningEnumerable<T>`** — deferred to gap #29 along with all owning-LINQ operators.
- **Lazy `Chunk`** — blocked by ownership soundness (see D3 above); deferred to gap #29.
- **`Chunk` returning `owned Array<Array<T>>`** — rejected: the outer borrowing array (`Array<Array<T>>`) would hold borrows of inner arrays with no owner for those inner arrays; `OwningArray<Array<T>>` is the correct outer type.

---

## Addendum (2026-07-28): TakeLast / SkipLast rider

*Source: the 2026-07-28 stdlib API review, adversarially critiqued (the review confirmed the gap below against current text). Appended without modifying any of the proposal text above. Written against spec/stdlib **v0.228** — the base proposal is written against v0.177/v0.165, so this addendum cites the current §5 semantics pins (the v0.195 clamping rule, the v0.196 callback-retention classification) where the base text predates them.*

### A1 — The confirmed gap: the tail idiom is unspellable on a lazy query

On a **lazy query**, the .NET tail idiom is a compile error under the affine-query rule (spec §29.11 single-consumption):

```bezel
var tail := q.Skip(q.Count() - n);   // ✗ Count() is a terminal operator and CONSUMES q
                                     //   (§29.11 consumption point 1); the Skip call is a
                                     //   second use of the consumed binding — a static
                                     //   error where visible, InvalidOperationException
                                     //   where only dynamically reachable
```

The only current spelling materializes the entire source first:

```bezel
using var all := q.ToArray();          // owned Array<T>; re-enumerable (§29.11 remedy)
var tail := all.Skip(all.Count - n);   // legal — a collection is not affine
```

On a **collection**, by contrast, the idiom is already legal, total, and fine: `arr.Skip(arr.Count - n)` — `Count` is the §4.4 property read, and `Take`/`Skip` **clamp** (stdlib §5, v0.195: "a negative count behaves as `0`, and a count beyond the source takes everything / skips everything — they are range operations, not magnitude constructors"). So the win is deliberately scoped, and stated honestly: **collections already have a one-line spelling; the operators exist for the affine streaming case — where they are a MEMORY win (buffer at most `count` elements instead of materializing the whole source) as much as a spelling win — and for not writing the arithmetic.**

### A2 — Proposed operators

```bezel
public method TakeLast(count: int): IEnumerable<T>;   // the final count elements, source order
public method SkipLast(count: int): IEnumerable<T>;   // all but the final count elements
```

Both on `IEnumerable<T>` only, matching this proposal's scoping ("none touches `IOwningEnumerable<T>` (gap #29, deferred)").

**Clamping** matches `Take`/`Skip` exactly (stdlib §5, v0.195): a negative `count` behaves as `0` (`TakeLast` yields nothing; `SkipLast` yields everything); `count` at or beyond the source length yields everything (`TakeLast`) / nothing (`SkipLast`). They are range operations, so stdlib §1's negative-magnitude fault rule deliberately does not apply — no `ArgumentException` anywhere; both operators are total over all `int` counts.

**Buffering** (documented in the style of D3's eagerness note):

- **`TakeLast`** — lazy until first enumeration, then **exhausts the source before yielding its first element**: no suffix is identifiable before end-of-source, so the operator runs the source to completion while retaining a sliding window of **at most `count` elements**, then yields the window. This is the lazy-then-buffer shape `GroupBy` already has (stdlib §5, v0.195), with the buffer bounded by the argument rather than by the source length. Honestly stated: the source is still fully consumed — the saving over `ToArray()` is *space* (at most `count` live at once), not *work*.
- **`SkipLast`** — genuinely streaming: each element is yielded delayed by `count` positions through an internal ring of **at most `count` elements**; there is no point of full materialization.

**Ownership (consistent with the operators above — the borrow path).** Both are **callback-free lazy operators**: they join `Take`/`Skip`/`Reverse` in stdlib §5's callback-retention classification (v0.196) — no callback is retained, and the returned query value is the ordinary deferred-activation frame carrying the receiver as its source (spec §28.14 / §28.3), with the unmarked `IEnumerable<T>` return being the stdlib §1 deferred-activation transfer announcement, exactly as `Take`/`Skip` are declared. At class-typed `T`, the internally buffered and finally yielded elements are borrows sourced from the query's source, identically to elements yielded by `Take`/`Skip`; no disposal obligation arises at any point and no `owned` marker appears anywhere. Both results are single-consumption (spec §29.11), and applying either operator to a bound query consumes that binding (§29.11 consumption point 2) — the affine discipline is preserved, not weakened.

```bezel
// The tail of an affine query without materializing its source.
// ReadRecords() is a user-written iterator method (spec §29.3) — a
// single-pass streaming source, the case A1 scopes these operators to:
var q := ReadRecords().Where(lambda(r) => r.Level = "ERROR");
var lastErrors := q.TakeLast(50);      // consumes q (§29.11 point 2); buffers at most 50

// Drop a trailer element without knowing the length up front:
var body := ReadRecords().SkipLast(1); // streams; holds at most 1 element back
```

### A3 — Member summary

The §2 proposed-section block above is left untouched per this addendum's append-only discipline; at graduation, the proposed §5.2 extension block additionally gains these two lines (slicing group):

```bezel
  // Tail slicing (2026-07-28 addendum): clamp as Take/Skip; buffer at most count
  public method TakeLast(count: int): IEnumerable<T>;
  public method SkipLast(count: int): IEnumerable<T>;
```

### A4 — Declined (addendum scope)

- **Eager owned-array forms** (`TakeLastArray(count): owned Array<T>`) — `.TakeLast(n).ToArray()` composes in one step; the operators exist precisely for the non-materializing case.
- **`IOwningEnumerable<T>` counterparts** — deferred to gap #29 with every other owning-LINQ operator, exactly as the base proposal's scoping states.
- **A backward-from-index `TakeLast(count, fromEnd)` family** — no workload named it; index-from-end slicing is a range-syntax question, not a LINQ one.
