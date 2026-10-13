# Proposal 0067 — Small-stdlib ergonomics batch II (symmetry sweep, 2026-07-28)

| | |
|---|---|
| **Status** | graduated-v0.228 — **superseded — graduated v0.228** (owner-accepted 2026-07-28; Q1–Q5 resolutions recorded in the v0.228 changelog entry) |
| **Tracking issue** | [#2242](https://github.com/s8lap/bezel-lang/issues/2242) (implementation) |
| **Target module(s)** | stdlib only — `System` ([§2.2](../Bezel_Standard_Library.md#sec-math) `Math`, [§2.5](../Bezel_Standard_Library.md#sec-datetime-timespan-dateonly-timeonly) `TimeSpan`), `System.Text` ([§3.2](../Bezel_Standard_Library.md#sec-encoding) `Encoding`), `System.Collections.Generic` ([§4.4](../Bezel_Standard_Library.md#sec-arrayt) `Array<T>`), and the [§9.1](../Bezel_Standard_Library.md#sec-integer-types) integer primitives. **No spec change, no new types, no new grammar.** |
| **Written against** | stdlib/spec v0.226 |
| **Origin** | A 2026-07-28 API-symmetry review of the v0.226 stdlib surface (run alongside the [gap review](papers/stdlib-gap-review-2026-07.md)'s map, which tracks capability gaps, not member-level symmetry), adversarially verified: every member below was re-located in the v0.226 stdlib text and confirmed absent; none is pre-declined in 0062's examined-and-deferred register or in [`DECLINED.md`](DECLINED.md). |

*Reference convention: unprefixed §-references are stdlib references; spec references are marked.*

---

## 0. Shape of the batch

Five items, each independently landable, in the mold of proposal 0062 (graduated v0.215). All five are **symmetry completions**: each adds the missing member of a pair the library already opened — a span twin the v0.222 train gave the decode direction but not the encode direction, a `decimal` twin `Abs` has but `Sign` lacks, a `Try` twin that every other `Parse` in the document carries, a from-index twin v0.215 gave `string.IndexOf` but not `Array<T>.IndexOf`, and the scalar-division twin of `TimeSpan`'s existing scalar multiplication. Nothing here introduces a new capability class; the considered-and-declined register (§7) records the neighbors deliberately left out.

**Ownership and sendability, stated once for the value-typed items (2, 3, 5).** `decimal`, the integer primitives, and `TimeSpan` are value types (their member surfaces: §9.3, §9.1, §2.5); every proposed member takes and returns values (or an `out` value slot), so no `owned` annotation, no borrow, and no disposal obligation appears anywhere in items 2, 3, and 5, and sendability (spec [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)) is unaffected. Items 1 and 4 state their borrow stories inline.

---

## 1. Span-source encode: `Encoding.GetByteCount`/`GetBytes` over `ReadOnlyStringSpan`

**Today (§3.2, v0.226).** The encode surface is string-only:

```bezel
public abstract method GetBytes(s: string): owned Array<byte>;
public abstract method GetByteCount(s: string): int;                       // v0.165 (#1355)
public abstract method GetBytes(s: string, destination: Span<byte>): int;  // v0.165
```

The v0.222 (#1809) span additions were **decode-only** (`GetString(bytes: ReadOnlySpan<byte>)`, `GetString(bytes: ReadOnlyArraySpan<byte>)`); §3.2's own closing sentence names the asymmetry — they "complete the v0.165 span-destination encode pair (`GetByteCount`/`GetBytes(s, destination)`) **in the decode direction**." The encode *source* side was never mirrored: no `GetBytes`/`GetByteCount` overload anywhere in the document takes a `ReadOnlyStringSpan`.

**Workload.** A service that parses a request line or a config token into a §3.3 span and then writes that token to a socket or file must call `.ToString()` first — materializing exactly the intermediate string `ReadOnlyStringSpan` exists to avoid (§3.3: "zero-allocation substring operations — tokenization, parsing … without materializing intermediate string instances"). §9.6's own guidance designates `GetByteCount(s)` + `GetBytes(s, destination)` as "the allocation-conscious form — repeated encoding in a loop, I/O paths reusing a buffer"; today that form is unreachable from a span without paying the allocation it is advertised to remove. This is the direct twin of the landed v0.222 `StringBuilder.Append(ReadOnlyStringSpan)` (§3.1), which closed the same seam for accumulation.

**Proposed** (new overloads on `Encoding`, §3.2):

```bezel
// PROPOSED — span-source encode, mirroring the v0.222 decode direction
public abstract method GetByteCount(s: ReadOnlyStringSpan): int;
public abstract method GetBytes(s: ReadOnlyStringSpan, destination: Span<byte>): int;
                                     // returns bytes written; IndexOutOfRangeException if
                                     //   destination is too short (as the string form)
public abstract method GetBytes(s: ReadOnlyStringSpan): owned Array<byte>;
                                     // one-shot allocating form
```

**Contract.** Identical to the string forms over the span's code-point range: the §3.2 strict-fault model applies unchanged (`FormatException` on a code point the encoding cannot represent — possible only for `ASCII`); `GetByteCount` and `GetBytes` agree; the empty span encodes to zero bytes. Equivalent to calling the string overload on `s.ToString()`, minus the materialization.

**Why the allocating span form too.** `enc.GetBytes(span.ToString())` allocates twice (string, then array); the span overload allocates once. Its return carries `owned` per the §1 convention for freshly-allocated class instances.

**Ownership.** `ReadOnlyStringSpan` is an ordinary record, **not** borrow-bearing (§3.3, v0.195 classification: its only reference-typed field is a runtime-managed `string`), so the parameter imposes no lifetime restriction. `destination: Span<byte>` is a call-bounded borrow, exactly as in the existing v0.165 overload. The returned `string`-independence property of the decode direction has its encode mirror: the produced bytes retain nothing of the source.

**Design note (abstract members and user subclasses).** §3.2 admits user subclasses of `Encoding`; adding `abstract` members is a source-break for them. With no external users, the clean surface wins. The alternative — `virtual` defaults on the base that delegate through `s.ToString()` — was considered and declined: a silently-allocating default defeats the overloads' whole purpose and hides the miss from exactly the callers who chose the span path for allocation reasons.

---

## 2. `Math` decimal symmetry: `Sign`, `Floor`, `Ceiling`, `Truncate`

**Today (§2.2, v0.226).** `Abs` has four overloads ending `Abs(x: decimal): decimal`, but `Sign` stops at `int`/`long`/`double`, and `Floor`/`Ceiling`/`Round`/`Truncate` exist only as `(x: double): double`. `Decimal` (§9.3) offers only the instance `Round(digits: int)`.

**Workload.** §2.18 designates `decimal` with `F2` as the money idiom ("format money explicitly (`$"{amount:F2} EUR"`)"). Money and quantity code needs sign tests and floor/ceiling logic constantly — ledger direction, whole-unit truncation, tax rounding-up — and today must round-trip through `double`, surrendering the exactness `decimal` exists to provide (§2.19/§9.3: exact representation of finite decimal fractions). `Abs(decimal)` already concedes that `Math` is the home for these; the rest of the family simply never followed.

**Proposed** (new overloads in `Math`, §2.2, beside their `double` forms):

```bezel
// PROPOSED
public static pure method Sign(x: decimal): int;            // -1, 0, or 1
public static pure method Floor(x: decimal): decimal;
public static pure method Ceiling(x: decimal): decimal;
public static pure method Truncate(x: decimal): decimal;
```

**Contract.** `Floor` rounds toward negative infinity, `Ceiling` toward positive infinity, `Truncate` toward zero — each to an integral value. All results are exactly representable — the endpoints of `decimal`'s range are integral (§9.3's exact-representation contract), and rounding a non-integral value toward either infinity moves it by less than one — so no overload can overflow and none faults. `Sign` matches the integer overloads' `-1, 0, or 1`. All four are `pure` (value-in/value-out, like the `double` forms; nothing here is `out`-shaped, so spec [§22.15](../Bezel_Language_Spec.md#sec-pure-members) poses no obstacle).

**Considered and declined: static `Math.Round(x: decimal, digits)`.** The instance `Decimal.Round(digits)` (§9.3) already provides banker's rounding with the pinned `0..28` digits contract "matching Math.Round (§2.2)"; duplicating it as a static would create two names for one operation. `Floor`/`Ceiling`/`Truncate` have **no** instance forms, so no duplication arises for them.

---

## 3. Radix `TryParse` on the integer types

**Today (§9.1, v0.226).** The only radix members are:

```bezel
public static pure method Parse(s: string, radix: int): int;        // 2, 8, 10, 16
public pure method ToString(radix: int): string;
```

There is no `Try` twin — the sole `Parse` in the document without one (`TryParse` plain and span forms exist on every §9 numeric type, `Convert.TryTo*`, the §2.5/§2.6 temporal and `Guid` forms), leaving radix parsing the one place expected-failure parsing forces `try`/`catch`.

**Workload.** Parsing user-supplied hex is expected-failure territory: config values, `#RRGGBB` color codes, request IDs, permission masks from a TOML/env string. Today the only non-throwing route is wrapping `Parse(s, 16)` in `try`/`catch` — precisely the ceremony the Try shapes exist to remove.

**Proposed** (on each of the eight §9.1 integer types; `Int32` shown, the others analogous per §9.1's stated pattern):

```bezel
// PROPOSED — shape (c) of the §1 Try convention
public static method TryParse(s: string, radix: int, out result: int): bool;
```

**Contract.** Accepts exactly the inputs `Parse(s, radix)` accepts, per radix in {2, 8, 10, 16}; returns `false` (with `default(T)` assigned to the out, spec [§22.3](../Bezel_Language_Spec.md#sec-parameter-modifiers)) in every case where `Parse` would raise `FormatException` or `OverflowException`. An **invalid radix** (anything outside 2/8/10/16) is a caller bug, not input failure, and raises `ArgumentException` from `TryParse` and `Parse` alike — the Try shapes shield malformed *input*, never malformed *calls* (the §1 convention's outcome-signaling reading). Landing this item should pin that invalid-radix fault on the existing `Parse(s, radix)` too, whose listing currently states only the accepted set (the v0.215 pin-the-previously-unstated-contract precedent).

**Not `pure`**: a pure member declares no `out` (spec [§22.15](../Bezel_Language_Spec.md#sec-pure-members), as recorded at §2.2's `DivRem`) — matching every existing `TryParse` in §9, which are all plain `static method`.

**Considered and deferred: span radix forms.** `Parse(s: ReadOnlyStringSpan, radix)` / `TryParse(s: ReadOnlyStringSpan, radix, out)` would complete the v0.222 (#1810) span-parse family in the radix direction. Deferred to keep this item minimal; if the owner prefers the family closed in one motion, the contract is identical over the span's range (open question Q2).

---

## 4. `Array<T>.IndexOf(item, startIndex)` and `Array<T>.LastIndexOf(item)`

**Today (§4.4, v0.226).** The sole occurrence-search member is:

```bezel
public method IndexOf(item: T): int;          // -1 if not found
```

No from-index form; no `LastIndexOf` at all. `String` (§9.6) has had `IndexOf(value, startIndex)` since v0.215 and both `LastIndexOf` forms; `String` is the only type in the document with `LastIndexOf` (§3.3's `ReadOnlyStringSpan` lacks it too).

**Workload.** The scan-for-next-occurrence loop — find each occurrence of a sentinel element, process the segment, resume after it — is the same "first N fields" shape that justified the v0.215 string addition (0062 item 2); without a start index, each iteration either re-scans from zero (quadratic) or hand-writes the loop `IndexOf` exists to replace. `LastIndexOf` is the natural query for "most recent matching entry" over an append-ordered array (a log tail, an undo stack scan).

**Proposed** (in `Array<T>`, §4.4, beside `IndexOf`):

```bezel
// PROPOSED
public method IndexOf(item: T, startIndex: int): int;   // first occurrence at or after
                                                        //   startIndex, or -1;
                                                        //   0 <= startIndex <= Count,
                                                        //   IndexOutOfRangeException otherwise
public method LastIndexOf(item: T): int;                // last occurrence, or -1
```

**Contract.** Equality is `Equals` per the standard convention, exactly as the existing single-argument `IndexOf` (and the §4.15 span searches, whose text states the `Equals` rule). The bounds pin mirrors the v0.215 string contract: `startIndex = Count` is legal and returns `-1` (empty suffix). Both are plain `method`, not `pure` — they run user equality (the v0.215 graduation precedent recorded at 0062: the `Contains` family stays impure for this reason).

**Ownership.** `item` is a borrow-taking query parameter (the same position as the existing `IndexOf`/`Contains`/`Remove(item)`); the return is an `int`. No ownership motion anywhere.

**Considered and declined.** (a) `LastIndexOf(item, startIndex)` — `String` has no backward-from-index form either; add both nowhere or both everywhere, and no workload has asked. (b) `OwningArray<T>` counterparts — its `IndexOf(item: T)` (§4.5) is the same borrow-taking query and could take the same two additions, but the confirmed pain is on the borrowing type; recorded here so a follow-up can cite it rather than rediscover it (open question Q3). (c) A predicate overload `IndexOf(match: (T) -> bool)` — LINQ's `Where`+indexed forms (0026) are that axis; not this batch.

---

## 5. `TimeSpan` division and negation, plus the `Zero`/`MinValue`/`MaxValue` anchors

**Today (§2.5, v0.226).** The operator group is exactly three lines:

```bezel
public static method operator +(a: TimeSpan, b: TimeSpan): TimeSpan;
public static method operator -(a: TimeSpan, b: TimeSpan): TimeSpan;
public static method operator *(a: TimeSpan, factor: double): TimeSpan;
```

No `/`, no unary `-`, and no `Zero`/`MinValue`/`MaxValue` members — so even the `TimeSpan.Zero - t` negation workaround is unspellable without `FromTicks(0)`.

**Workload.** Average latency — `total / count` — is a bread-and-butter service metric; scaling a timeout down (`t / 2.0`) is its configuration twin. Today both take the `TimeSpan.FromTicks(total.Ticks / n)` detour, dropping to the tick representation that the arithmetic operators exist to encapsulate. Negation arises whenever a signed duration flips direction (deadline slack, clock-drift correction); `Zero` is the identity every accumulation loop and comparison threshold wants (`if drift < TimeSpan.Zero then …`).

**Proposed** (in `TimeSpan`, §2.5):

```bezel
// PROPOSED — completes the scalar pair `*` opened, plus the negation `-` lacks
public static method operator /(a: TimeSpan, divisor: double): TimeSpan;
public static method operator -(t: TimeSpan): TimeSpan;     // unary negation

// PROPOSED — anchors, on the DateTime §2.5 precedent (static readonly properties)
public static readonly property Zero: TimeSpan;
public static readonly property MinValue: TimeSpan;
public static readonly property MaxValue: TimeSpan;
```

Both operator forms are admissible as declared: unary `-` is overloadable (spec [§25.1](../Bezel_Language_Spec.md#sec-overloadable-operators)), records may declare operator overloads with the declaring type in either operand position (spec [§25.2](../Bezel_Language_Spec.md#sec-operator-overloading-restrictions)), and unary operators carry no complete-group obligation.

**Contract.** `MinValue`/`MaxValue` are the full tick range (`FromTicks(long.MinValue)` / `FromTicks(long.MaxValue)`); `Zero` is `FromTicks(0)`. Division computes over `Ticks` in `double`, as `*` does. `TimeSpan`, like `decimal`, has no `Infinity` or `NaN`, so the spec [§6.4](../Bezel_Language_Spec.md#sec-decimal-arithmetic) stance applies rather than the [§6.3](../Bezel_Language_Spec.md#sec-floating-point-arithmetic) produce-infinity stance: `divisor = 0.0` raises `DivideByZeroException`; a `NaN` divisor or a quotient outside the tick range raises `OverflowException`. Unary `-` of `MinValue` raises `OverflowException` (the two's-complement edge, the spec [§6.1](../Bezel_Language_Spec.md#sec-integer-arithmetic) checked stance). Landing this item should pin the same out-of-range fault on the **existing** `*` operator, whose overflow behavior is currently unstated (the v0.215 pin precedent again).

**Ownership.** Value record over value fields throughout; sendable, no disposal, operands received as copies (operator parameters admit no modifiers, spec §25.2).

**Coordination.** Two live drafts also amend §2.5 `TimeSpan`: 0016 (monotonic time — `FromNanoseconds`) and 0012 (DateTimeOffset — arithmetic against `TimeSpan`). Whichever graduates first, the later landings re-verify this item's operator/anchor set against the then-current §2.5 text (the proposals-currency discipline).

**Considered and deferred: the ratio form `operator /(a: TimeSpan, b: TimeSpan): double`.** Genuinely useful (progress = elapsed/total) and unambiguous beside the scalar form under spec §25.2's in-order matching, but it is a second capability (duration-ratio, not scalar-scale) the confirmed workload did not name; recorded as open question Q4 rather than proposed.

---

## 6. Open questions

- **Q1 (item 1):** abstract members on `Encoding` break hypothetical user subclasses — confirm the no-external-users premise at landing, or downgrade the one-shot allocating span form to the two abstract core members only.
- **Q2 (item 3):** close the radix family over `ReadOnlyStringSpan` in the same motion (`Parse`/`TryParse` span+radix), or leave the span direction to a later sweep?
- **Q3 (item 4):** extend `IndexOf`-from/`LastIndexOf` to `OwningArray<T>` (§4.5) for surface parity, or wait for a workload on the owning type?
- **Q4 (item 5):** admit the duration-ratio `operator /(a: TimeSpan, b: TimeSpan): double` alongside the scalar division?
- **Q5 (items 3, 5):** the two piggyback pins (invalid-radix fault on the existing `Parse(s, radix)`; overflow fault on the existing `TimeSpan.operator *`) — land with their items, or split into an independent contract-pinning editorial pass?

## 7. Batch-level considered-and-declined

- **`ReadOnlyStringSpan.LastIndexOf`** — would make `String` no longer the sole `LastIndexOf` holder; no tokenization workload scans backward today. Not proposed.
- **`Math.Round(x: decimal)` statics** — duplicate of the pinned instance `Decimal.Round(digits)` (§9.3); declined in item 2.
- **Anything from 0062's examined-and-deferred register** (`Lazy<T>`, raw strings, as-patterns, flags-enum niceties, `TaskScope` ctor sugar, `LinkedList` relink, `Partial<T>`, `Swap<T>`) — those entries stand; nothing here reopens them.
