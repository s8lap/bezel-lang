# Proposal 0027 — `SplitSpans`: zero-alloc string tokenization

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Tracking issue** | [#1458](https://github.com/s8lap/bezel-lang/issues/1458) |
| **Target modules** | `System.Text` (§3, auto-imported): new `StringSplitCursor` class; `string` (§9.6): three new `SplitSpans` overloads |
| **Written against** | spec v0.177 / stdlib v0.165 |
| **Currency note** | Re-verified clean at v0.306 (2026-09-17 sweep): zero scan hits across all 28 retired-form patterns; `string.AsSpan(r: Range<int>)`, `Range<int>.IsEmpty` (now `pure`, v0.263 — a marker addition, no respell), §29.2's span-enumerator bar and the free §3.5 slot all still hold. No tier-1 respells, no tier-2 flags. Written-against pin unchanged. |
| **Gap-review item** | #20 (P1) in [stdlib-gap-review-2026-07.md](papers/stdlib-gap-review-2026-07.md) |
| **Wave** | D3 — library breadth, per [stdlib-gap-design-plan-2026-07.md](papers/stdlib-gap-design-plan-2026-07.md) |

Zero-allocation string tokenization for hot paths where materializing a `string[]` via `Split` is too expensive.

---

## 1. The constraint that shapes everything (spec §29.2)

`ReadOnlyStringSpan` does not implement `IEnumerable<char>` — spec §29.2 explains that an enumerator returned by `IEnumerable<T>.GetEnumerator()` is an ordinary heap class that would hold the span (or its borrow of the source string) across its own, unbounded lifetime, and the borrow tracker cannot bound that lifetime through the enumerator interface. The same reasoning bars `IEnumerable<ReadOnlyStringSpan>` as a return type for `SplitSpans`: a `StringSplitEnumerator` heap class would hold the source string borrow for an unbounded lifetime, defeating the lifetime-safety guarantees that make span types safe.

**Consequence:** `SplitSpans` cannot return `IEnumerable<ReadOnlyStringSpan>`. An alternative protocol is needed.

---

## 2. Design decisions

### D1 — Cursor form: Range-emitting class

Two cursor shapes were weighed:

**Option A: cursor emitting `ReadOnlyStringSpan`** — `Current` returns a `ReadOnlyStringSpan` directly. The cursor would be a class holding the source `string` plus a current-token span. `ReadOnlyStringSpan` is described as a borrow-bearing record in stdlib §3.3; a cursor class returning it from `Current` would propagate the borrow, making the cursor itself borrow-bearing in effect. Storing such a cursor in a collection, returning it from a method, or composing it with other APIs would require the borrow-lifetime rules to flow through the cursor — the same coupling that blocks `IEnumerable<ReadOnlyStringSpan>`, just delayed one layer.

**Option B: cursor emitting `Range<int>`** — `Current` returns a `Range<int>` (a compiler-intrinsic value record, start inclusive and end exclusive, with no borrow coupling whatsoever). The caller converts to a `ReadOnlyStringSpan` on demand via the existing `string.AsSpan(r: Range<int>): ReadOnlyStringSpan` (stdlib §9.6). The cursor itself holds only a `string` reference (runtime-managed, unbounded lifetime per spec §9.7) and primitive position state; it is **not** borrow-bearing under the strict §9.7 definition.

**Decision: Option B.** The Range-emitting form decouples cursor lifetime from span lifetime entirely. The cursor can be stored, passed across method boundaries, and used as a value without borrow restrictions. The span is constructed only when the caller asks for it, and its lifetime is governed by the enclosing scope, not by the cursor's state.

### D2 — Cursor type: class, not record

Iteration state (current scan position, separator, exhaustion flag) must be mutated on each `MoveNext()` call. Records in Bezel are value types; for a mutable cursor the natural home is a **class**. The class holds the source `string` and integer scan state. `Dispose()` clears the string reference (a no-op in terms of resource release, since strings are runtime-managed; included for consistency with the class-disposal contract).

### D3 — Separator forms

Three overloads mirror the existing `string.Split` (stdlib §9.6) signature family exactly:

| New `SplitSpans` | Matching `Split` |
|---|---|
| `SplitSpans(separator: char)` | `Split(separator: char)` |
| `SplitSpans(separator: string)` | `Split(separator: string)` |
| `SplitSpans(separators: Array<char>)` | `Split(separators: Array<char>)` |

This keeps the split API family uniform: callers who know `Split` know `SplitSpans`.

### D4 — Empty-token policy

Include empty tokens (consecutive separators yield a `Range<int>` where `Start = End`, i.e. `IsEmpty = true`). This matches `string.Split`'s default behavior, preserving field positions in CSV-like formats. Callers who want to skip empty tokens write `if not cursor.Current.IsEmpty then ... end` or equivalent.

### D5 — Relationship to `string.Split`

`SplitSpans` is the **allocation-free complement** of `string.Split`, not a replacement. `Split` returns `owned Array<string>` — fully materialized, each token a separate heap string — and is the right choice when individual tokens need to be stored, passed, or compared against non-literal strings. `SplitSpans` allocates only the `StringSplitCursor` object; each `ReadOnlyStringSpan` from `AsSpan(range)` is a stack-allocated value record pointing into the source string. Use `SplitSpans` in hot tokenization loops (HTTP header parsing, CSV ingestion, log line splitting) and `Split` everywhere else.

---

## 3. Proposed stdlib-section text

### New §3.5 `StringSplitCursor` (System.Text, auto-imported)

A stateful cursor for zero-allocation tokenization of a `string` by separator. Obtained exclusively from the `SplitSpans` family on `string` (§9.6); not constructible directly.

```bezel
public class StringSplitCursor
  // No public constructor; obtained from string.SplitSpans overloads.

  // The current token as a half-open index range [Start, End) into the source string.
  // Valid only after MoveNext() has returned true; undefined before the first MoveNext() call.
  public readonly property Current: Range<int>;

  // Advance to the next token. Returns true if a token is available; false when exhausted.
  // Calling MoveNext() after it has returned false is a no-op and returns false.
  public method MoveNext(): bool;

  destructor;   // clears the string reference; no resource release required
end StringSplitCursor;
```

`StringSplitCursor` is a class (not a record) because iteration requires mutable scan-position state. Its `Dispose()` is effectively a no-op (the string field is runtime-managed), but the class participates in the standard ownership model — `SplitSpans` returns an `owned StringSplitCursor` and callers should dispose it, typically via `using var`.

**Why `Current` is `Range<int>`, not `ReadOnlyStringSpan`.**  A cursor whose `Current` returned `ReadOnlyStringSpan` would propagate the span's borrow-bearing character (stdlib §3.3) through the cursor interface, constraining where the cursor can be stored and how long it can live.  A `Range<int>` is a plain value record with no borrow coupling; the caller obtains a `ReadOnlyStringSpan` from it on demand via `s.AsSpan(cursor.Current)` (§9.6, existing), keeping each span's lifetime explicit and independent of the cursor.

**Usage pattern:**

```bezel
var csv := "alpha,beta,,delta";
using var cursor := csv.SplitSpans(',');
while cursor.MoveNext() do
  var range := cursor.Current;                // Range<int>, e.g. 0..<5 for "alpha"
  var token := csv.AsSpan(range);            // ReadOnlyStringSpan — zero alloc
  if not range.IsEmpty then
    Process(token);
  end;
end;
```

**`Range<int>.IsEmpty`** is the idiomatic test for a zero-length token: `range.IsEmpty` is `true` when `Start = End` (spec §16.3, stdlib §9.8).

---

### Additions to §9.6 `String`

Add the following three methods to the `string` section (alongside the existing `Split` family):

```bezel
// Zero-alloc tokenization — returns a cursor emitting Range<int> tokens.
// Mirrors the Split overload family; see System.Text.StringSplitCursor (§3.5).
public method SplitSpans(separator: char): owned StringSplitCursor;
public method SplitSpans(separator: string): owned StringSplitCursor;
public method SplitSpans(separators: Array<char>): owned StringSplitCursor;
```

`SplitSpans(separator: string)` with an empty separator string is a defined fault (`ArgumentException`), consistent with `Split(separator: string)`.

The cursor emits the same sequence of token ranges that `Split` would produce as strings, including empty tokens from consecutive separators. Calling `s.AsSpan(range)` on each emitted `Range<int>` produces the equivalent zero-allocation `ReadOnlyStringSpan`.

---

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

No open owner questions; promoted as-is. The design is fully settled and spec-verified: the §29.2 span-enumerator bar forces a non-`IEnumerable` protocol; the `Range<int>`-emitting `StringSplitCursor` class (D1/D2) decouples cursor lifetime from span lifetime, so the cursor is not borrow-bearing (§9.7) and the caller materializes a `ReadOnlyStringSpan` on demand via the existing `string.AsSpan(r: Range<int>)` (§9.6); the three `SplitSpans` overloads mirror the `Split` family; empty tokens are included (`Range<int>.IsEmpty`, §16.3/stdlib §9.8). Lands in the d3-small-utilities batch (new §3.5 `StringSplitCursor` + three §9.6 `String` methods) through the normal docs workflow.

## 4. Declined alternatives

- **`IEnumerable<ReadOnlyStringSpan>`** — blocked by spec §29.2: an enumerator class cannot safely hold a span borrow across its unbounded lifetime; see §1 above.
- **`ReadOnlyStringSpan`-emitting cursor** — rejected in favor of the Range form; see D1.  The span form would propagate borrow-bearing character through the cursor, reintroducing the lifetime coupling that §29.2 is designed to prevent.
- **Value-record cursor with functional `MoveNext`** — a record cursor returning `(bool, StringSplitCursor)` from `MoveNext` would be allocation-free but requires the loop pattern `var (ok, cursor) := cursor.MoveNext()`, which is less idiomatic than `while cursor.MoveNext() do`. The class form matches the `IEnumerator<T>` usage pattern that Bezel programmers already know.
- **Compiler `for`-loop desugaring for `StringSplitCursor`** — would need a spec amendment; deferred as a possible ergonomic follow-on once the API stabilizes.
