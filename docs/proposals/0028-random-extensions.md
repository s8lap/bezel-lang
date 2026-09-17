# Proposal 0028 — `Random` extensions: `NextFloat`, `NextInt64` range overloads, `NextBytes(Span<byte>)`, `Shuffle`

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Tracking issue** | [#1459](https://github.com/s8lap/bezel-lang/issues/1459) |
| **Target module** | `System` (§2.7 `Random`, auto-imported) |
| **Written against** | spec v0.177 / stdlib v0.165 |
| **Currency note** | Re-verified clean at v0.306 (2026-09-17 sweep): §1's transcribed §2.7 `Random` surface still matches the landed member list (no `:=` defaults there, so v0.290's overload-pair rule takes no edit), and `Span<byte>`/`ArraySpan<byte>`/`ReadOnlySpan<byte>` are all live type names. No tier-1 respells, no tier-2 flags. Written-against pin unchanged. |
| **Gap-review item** | #30 (P2) in [stdlib-gap-review-2026-07.md](stdlib-gap-review-2026-07.md) |
| **Wave** | D3 — library breadth, per [stdlib-gap-design-plan-2026-07.md](stdlib-gap-design-plan-2026-07.md) |

Four additive fills to `Random` (stdlib §2.7). Nothing here changes the seeding contract or the underlying generator; the additions are new output shapes and a shuffle utility.

---

## 1. Verifying the gap against §2.7

The current `Random` surface (stdlib §2.7):

```bezel
public class Random
  public constructor();
  public constructor(seed: long);

  public method Next(): int;                        // [0, int.MaxValue]
  public method Next(maxValue: int): int;           // [0, maxValue)
  public method Next(minValue: int, maxValue: int): int;
  public method NextInt64(): long;                  // full long range
  public method NextDouble(): double;               // [0.0, 1.0)
  public method NextBytes(buffer: Array<byte>);     // fills buffer in-place
  public method NextBoolean(): bool;
end Random;
```

**Gap items confirmed missing:**

| Item | Status |
|---|---|
| `NextFloat(): float` | absent — no `float` analogue of `NextDouble` |
| `NextInt64(maxValue: long): long` | absent — `NextInt64()` exists but has no range overloads |
| `NextInt64(minValue: long, maxValue: long): long` | absent |
| `NextBytes(buffer: Span<byte>)` | absent — only the `Array<byte>` form exists |
| `Shuffle<T>(array: Array<T>)` | absent — no in-place shuffle anywhere in the stdlib |

---

## 2. Design decisions

### D1 — `NextFloat(): float`

**Decision.** Add `NextFloat(): float` returning a value in `[0.0f, 1.0f)`.

The naming follows the Bezel type name (`float`, not `single`). The existing `NextDouble(): double` establishes the naming pattern; `NextFloat` is the float-width complement. The range `[0.0f, 1.0f)` mirrors `NextDouble`'s `[0.0, 1.0)` exactly; no lower-closed / upper-open variation is introduced.

### D2 — `NextInt64` range overloads

**Decision.** Add two overloads to complete the `NextInt64` family, mirroring the three-overload `Next` family:

| `int` form (existing) | `long` form (existing / proposed) |
|---|---|
| `Next(): int` — `[0, int.MaxValue]` | `NextInt64(): long` — full range (existing) |
| `Next(maxValue: int): int` — `[0, maxValue)` | **`NextInt64(maxValue: long): long`** — proposed |
| `Next(minValue: int, maxValue: int): int` — `[minValue, maxValue)` | **`NextInt64(minValue: long, maxValue: long): long`** — proposed |

Both new overloads use half-open ranges `[min, max)`, consistent with the existing `Next(maxValue)` and `Next(minValue, maxValue)`. Fault conditions:

- `NextInt64(maxValue)`: `maxValue < 0` raises `ArgumentException` (mirrors `Next(maxValue)` which requires `maxValue >= 0`).
- `NextInt64(minValue, maxValue)`: `maxValue < minValue` raises `ArgumentException`.

### D3 — `NextBytes(buffer: Span<byte>)`

**Decision.** Add `NextBytes(buffer: Span<byte>)` alongside the existing `NextBytes(buffer: Array<byte>)`.

`Span<T>` is a borrow-bearing record (stdlib §4.14). Sync methods may accept span parameters because the span's lifetime is bounded by the call frame — the borrow's source outlives the synchronous call. Issue #1438 flags async + span combinations as unsound; `NextBytes` is synchronous, so the span form is sound here. The OwningBuffer-backed `Span<byte>` is the preferred target for callers using pre-allocated byte buffers (avoids the array allocation that `NextBytes(Array<byte>)` requires when the caller already has buffer-backed storage).

`ReadOnlySpan<byte>` is not admitted (write-only destination; `ReadOnlySpan` has no writable indexer). `ArraySpan<byte>` (stdlib §4.15) follows the same reasoning: it is the Array-backed writable span analogue. Should a future gap review request `NextBytes(ArraySpan<byte>)`, that is a distinct addition and not blocked by this proposal.

### D4 — `Shuffle<T>(array: Array<T>)` on `Random`

**Decision.** Instance method on `Random`, taking `Array<T>` by value (borrow).

**Why on `Random`, not on `Array<T>`.** `Random` is the entropy source; the shuffle is logically a random operation parameterized by the RNG, not an operation the collection performs on itself. `rng.Shuffle(arr)` reads as "use this RNG to shuffle this array," which is the natural mental model. Adding `arr.Shuffle(rng)` as an `Array<T>` extension would be equally discoverable but inverts the actor/subject relationship.

**Algorithm.** Fisher-Yates (Knuth) shuffle: for `i` from `n-1` down to `1`, swap `array[i]` with `array[rng.Next(i + 1)]`. O(n) time; each permutation is equally likely given a uniform RNG.

**Why `Array<T>` only in v1, not `OwningArray<T>`.** `Array<T>` stores borrowed element references (for class T) or value copies (for value T); swapping by index is sound for both. `OwningArray<T>` stores owned elements; swapping owned slot contents is also sound (slot ownership stays in the array), but the overload adds API surface and a separate implementation. Deferred to a follow-on once a consumer appears. The restriction is noted in the stdlib text.

**Type constraint.** No constraint on `T` is needed — Fisher-Yates requires only indexed read/write access, which `Array<T>` provides for any T.

---

## 3. Proposed stdlib-section text (additions to §2.7 `Random`)

Add the following members to the `Random` class declaration in §2.7:

```bezel
public class Random
  // ... existing members unchanged ...

  // float-width analogue of NextDouble
  public method NextFloat(): float;               // [0.0f, 1.0f)

  // range overloads completing the NextInt64 family
  public method NextInt64(maxValue: long): long;                          // [0, maxValue); ArgumentException if maxValue < 0
  public method NextInt64(minValue: long, maxValue: long): long;          // [minValue, maxValue); ArgumentException if maxValue < minValue

  // span-accepting overload (sync only; see §4.14 for Span<T>)
  public method NextBytes(buffer: Span<byte>);    // fills buffer in-place

  // in-place Fisher-Yates shuffle
  public method Shuffle<T>(array: Array<T>);
end Random;
```

**`NextFloat`.** Returns a `float` in `[0.0f, 1.0f)`. Equivalent to `float(NextDouble())` for the range guarantee; implementations may use a more direct method. No `NextFloat(max)` or `NextFloat(min, max)` form is added in v1 (floating-point range semantics require care around rounding; the cast pattern `NextDouble() * range + min` suffices for now).

**`NextInt64` range overloads.** Complete the three-form family that `Next` already has for `int`. The half-open `[min, max)` convention is consistent throughout. `NextInt64(long.MinValue, long.MaxValue)` returns a value in `[long.MinValue, long.MaxValue)` — `long.MaxValue` is excluded; to include the full representable range use `NextInt64()` (no-arg form, full range).

**`NextBytes(Span<byte>)`.** Fills every element of `buffer` with a random byte. Behavior is identical to `NextBytes(Array<byte>)` restricted to the span's range. This overload is admitted because `NextBytes` is synchronous — the span's borrow is valid for the duration of the call (spec §28.3 borrow-bearing-record rule; the concern about spans and async in #1438 does not apply to sync methods).

**`Shuffle<T>`.** Shuffles `array` in place using the Fisher-Yates algorithm (Knuth, v2): for each index `i` from `array.Count - 1` down to `1`, swap `array[i]` with `array[j]` where `j := rng.Next(i + 1)`. Each permutation of the elements is equally likely given a uniform `Next`. No return value (the shuffle is in-place; the `Array<T>` borrow remains valid throughout). Does not apply to `OwningArray<T>` in v1; a follow-on may add that overload.

---

## 4. Seeding and determinism

The seeding contract is **unchanged** by this proposal. Quoting §2.7:

> "Pseudorandom number generation. Seeded; a default-constructed `Random` uses a system-time-based seed."
> `public constructor(seed: long);`

All new methods generate output via the same internal generator state as the existing methods. A `Random` constructed with a fixed `seed: long` produces a fully deterministic sequence regardless of which methods are called and in what order. No new global or static state is introduced.

For cryptographic randomness, §2.7 already defers to `System.Security.Cryptography` (a future module). This proposal does not add `SecureRandom` or any cryptographically secure surface.

---

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

No open owner questions; promoted as-is. The four additions are verified against stdlib §2.7 and carry as drafted: `NextFloat(): float` in `[0.0f, 1.0f)` (the float-width complement of `NextDouble`); the two `NextInt64` range overloads completing the three-form family (half-open `[min, max)`, `ArgumentException` on bad bounds); `NextBytes(Span<byte>)` — sound because `NextBytes` is *synchronous*, so the borrow-bearing span (§4.14) is bounded by the call frame and the #1438 async-span concern does not apply; and the Fisher-Yates `Shuffle<T>(Array<T>)` instance method on `Random` (RNG as the actor). The seeding/determinism contract is unchanged. `NextFloat` range overloads, `Shuffle<T>(OwningArray<T>)`, and `NextUInt64` are well-declined for v1. Lands in the d3-small-utilities batch as §2.7 additions through the normal docs workflow.

## 5. Declined alternatives

- **`NextFloat(max)` / `NextFloat(min, max)` range overloads** — not included in v1; floating-point range arithmetic requires rounding-mode discussion that goes beyond the gap item; the cast pattern is sufficient.
- **`Shuffle<T>(array: OwningArray<T>)`** — deferred (see D4); requires a separate overload, and no consumer is blocked today.
- **`Shuffle<T>` as an `Array<T>` extension method** — rejected in favor of the `Random` instance method; see D4.
- **`NextUInt64` / `NextUInt64(max)` / `NextUInt64(min, max)`** — not in gap item #30; `uint64` random generation is addressable today via `ulong(rng.NextInt64())` with bit reinterpretation for most use cases, and the gap review does not flag it.
