# Proposal 0025 — `BitArray`

| | |
|---|---|
| **Status** | under-review — draft |
| **Tracking issue** | [#1452](https://github.com/s8lap/bezel-lang/issues/1452) |
| **Target module(s)** | `System.Collections.Generic` (stdlib §4) — new §4.20 |
| **Written against** | spec **v0.177**; stdlib companion v0.165 |
| **Currency note** | Refreshed 2026-09-17 to v0.306: the two "`System.Collections.Generic` is not auto-imported" claims (§D1, §6 item 2) corrected — §4 has been auto-imported since **v0.289** (0122 L3), and the quoted stdlib §4 sentence no longer exists. §D2/§D3/§D4's citations re-verified (`OwningBuffer<T>.Length`, `Array<T>.Sort`/`Reverse` — restored on `Array<T>` itself at v0.275 — and the `Span<T>` indexer bounds-check all stand). No tier-2 flags. Written-against pin unchanged. |
| **Origin** | Gap review [#1403](https://github.com/s8lap/bezel-lang/issues/1403) item #28 (P1), wave D3 of the [stdlib gap DESIGN plan](papers/stdlib-gap-design-plan-2026-07.md) |
| **Depends on** | [Proposal 0008](0008-integer-toolkit.md) / [#1437](https://github.com/s8lap/bezel-lang/issues/1437) — `BitOperations.PopCount` is used internally for `PopCount`. `BitArray` without `PopCount` is independent; `PopCount` requires 0008 to land first. |

---

## 1. The problem

There is no bit-packed boolean collection in the stdlib today. The natural alternative — `Array<bool>` — stores one byte (or more) per bit, wasting up to 8× memory and making word-at-a-time bulk operations (AND, OR, XOR, NOT) impractical. Applications needing dense boolean flags (protocol bitfields, Sieve of Eratosthenes, graph reachability bitmaps, permission sets, Bloom filters) either pay the full byte-per-bit cost or implement their own bit manipulation over `Array<uint>`.

`BitArray` fills this gap: a fixed-length, bit-packed sequence of boolean values with O(1) indexed access and O(n/w) bulk bitwise operations (where w is the machine word width).

---

## 2. Design decisions

### D1 — Class over internal packed storage; home in §4

**Decision.** `BitArray` is a **class** living in `System.Collections.Generic` (stdlib §4), numbered §4.20.

*Why a class, not a record.* `BitArray` is mutable (indexed set, in-place And/Or/Xor/Not, SetAll) and owns its backing storage — the same combination that motivates `Array<T>`, `Queue<T>`, and `OwningBuffer<T>` being classes. A mutable record would require value-receiver mutation (§9.4 explicitly calls mutable records "inadvisable"). The class shape provides the expected `using var`/`owned` ownership discipline and a proper destructor for the backing store.

*Why `sealed`.* `BitArray` is declared `sealed`. Beyond the usual "no subclass needs to extend it" reasoning, `sealed` is what makes it **sendable** under §30.14 rule 1 (an open class is never sendable). With its single backing field an `owned Array<uint>`, the sendability derivation goes through — see §D9.

*Why §4, not a new module.* `BitArray` is a collection of booleans; it belongs beside `Array<T>` and `Queue<T>` in `System.Collections.Generic`. Note that `System.Collections.Generic` **is** auto-imported (v0.289, graduation batch 0122 L1–L18 item L3) — spec §2.3 carries the module row and stdlib §4 states "**Auto-imported:** this module is on language spec §2.3's auto-import list, so every name below resolves in every file with no `import`". A program using `BitArray` therefore needs no import line at all, exactly as for `Dictionary`/`Queue`; the bare name `BitArray` joins §2.3's stated cost, which is the price the row records. *(Written against v0.177, when §4 required an explicit `import System.Collections.Generic;`; the home-module argument is unchanged either way.)*

*Internal representation.* The backing store is an internal `owned Array<uint>` field (the `owned` marker matters for the §D9 sendability derivation) with `(Length + 31) / 32` elements; bit `i` occupies bit position `i mod 32` of word `i / 32`. This is an implementation detail (the implementation may use `ulong` for performance or other packing strategies); it is not part of the contract. The spec says *class over an internal packed storage*; what the storage is remains internal.

### D2 — `Length` property for the bit count

**Decision.** The bit count is exposed as `readonly property Length: int`, matching `OwningBuffer<T>` (§4.14), which also uses `Length` for its fixed-size count. This distinguishes fixed-size containers (`Length`) from resizable collections (`Count` per §4.4 `Array<T>`). `BitArray` v1 is **fixed-length**: once created, the number of bits cannot change. No resize in v1.

*Alternative: `Count` as in `IReadOnlyCollection<T>.Count`.* Declined. `Count` signals "resizable collection semantics" to a Bezel reader. `Length` signals a fixed-size buffer view and aligns with `OwningBuffer<T>`.

### D3 — Bulk bitwise operations: in-place mutating; `Clone` for non-destructive copy

**Decision.** `And`, `Or`, `Xor`, `Not` are **in-place mutating** methods that modify `this`. A separate `Clone(): owned BitArray` provides a deep copy when the caller needs a non-destructive variant.

*Why in-place.* The primary use case — updating a live bitmap — wants mutation without an allocation per operation. In-place also keeps the API consistent with the spec's treatment of `Array<T>.Sort` and `Array<T>.Reverse` (§4.4), which are in-place. Returning a *new* `BitArray` per operation is a valid alternative (the functional style), but it conflicts with Bezel's usual choice for mutable collections and would make chained operations (`ba.And(mask).Or(extension)`) require `using var` management for every intermediate.

*Why `Clone` rather than a copy constructor.* `Clone()` is a method that returns an `owned BitArray`, following the `OwningBuffer<T>` convention for producing a fresh copy. It is explicit at the call site without requiring language-level copy semantics for a mutable class.

`And(other)` and `Or(other)` and `Xor(other)` require `other.Length = this.Length`; `ArgumentException` if the lengths differ. `Not` is always defined (no other operand).

### D4 — Indexer: bounds fault per §4 convention

**Decision.** `property [i: int]: bool` — get and set are both bounds-checked. Out-of-range access raises `IndexOutOfRangeException`, matching §4.4 (`Array<T>` indexer) and §4.14 (`Span<T>` indexer bounds-check, v0.105).

### D5 — `SetAll(value: bool)`

**Decision.** `SetAll(value: bool)` sets every bit to `value` in O(n/w) time. This is the direct equivalent of `Span<T>.Fill` (§4.14) for the bit-packed case; word-level operations (fill with `0xFFFFFFFF` or `0`) make it substantially faster than individual indexed sets.

### D6 — `CopyTo(dest: Array<bool>, destIndex: int)`

**Decision.** `CopyTo(dest: Array<bool>, destIndex: int)` copies all bits into an existing `Array<bool>` starting at `destIndex`. `destIndex + this.Length > dest.Count` raises `ArgumentException`. This mirrors `.NET`'s `BitArray.CopyTo(Array, int)` (restricted to the `bool[]` form — the integer-array form is a footgun in a typed language and is not provided).

### D7 — `PopCount` depends on proposal 0008 `BitOperations`

**Decision.** `PopCount(): int` returns the number of bits set to `true`. Its implementation uses `BitOperations.PopCount` (proposed §2: `BitOperations.PopCount(value: uint): int`) word-by-word over the internal backing store. The method signature is part of this proposal; its dependency on 0008 is recorded explicitly. The method may be deferred to a follow-up if 0008 has not landed before `BitArray` is integrated; this is noted in the "what v1 may defer" section (§5).

### D8 — Iteration: `IEnumerable<bool>`

**Decision.** `BitArray` directly implements `IEnumerable<bool>`. `for var bit in ba do ... end` yields each bit as a `bool` in index order (index 0 first).

*Basis for the decision.* The §4.14 note states that `OwningBuffer<T>` with value-typed T does **not** implement `IEnumerable<T>` due to a generic conditional-interface limitation: `OwningBuffer<T>` is generic and the compiler lacks a mechanism to implement an interface conditionally on T's category. `BitArray` is not generic; it is a concrete class with a concrete element type (`bool`). The conditional-interface limitation does not apply; direct `IEnumerable<bool>` conformance is straightforward. The `for`-loop desugaring additionally emits a direct indexed-loop optimization (§29.2: "direct-indexed-loop optimization for concretely-typed sources") so no heap-allocated enumerator is needed in the common `for` case.

*Alternative: indexed-only access, no `IEnumerable<bool>`.* Declined. `IEnumerable<bool>` conformance unlocks LINQ (§5) and the `for`-loop without extra conversion cost. The precedent from §4.14 (OwningBuffer's class-T form implementing `IEnumerable<T?>`) confirms that direct iteration conformance is the expected style.

### D9 — Sendable: a `sealed` bitmap with an `owned` backing store is transfer-closed

**Decision.** `BitArray` is **sendable** across domains (§30.14). This corrects an earlier reading that treated "mutable in-place state" as disqualifying — §30.14 sendability governs *transfer* (a move across a domain boundary), not *sharing*, so mutability is irrelevant.

*Derivation.* §30.14's class rule: rule 1 — the class is `sealed` (§D1); rule 2 — every field is sendable. `BitArray`'s single field is an `owned Array<uint>`, and `Array<uint>` is a borrowing collection whose element type (`uint`) is a sendable value type, which the §30.14 compiler-known list declares sendable. No function-typed, `Object`-typed, interface-typed, or non-`owned` class field appears (rule 3). Therefore `BitArray` is **transfer-closed** and moves whole across a domain boundary.

*Why this is useful, not incidental.* A worker domain can compute a bitmap — graph reachability, a sieve, a Bloom filter — and **return it as an owned spawn result** to the parent, or receive one moved in as a spawn argument. The bitmap crosses the boundary by ownership transfer with no copy and no synchronization; the receiving domain is its sole owner thereafter. (Mutating a `BitArray` still requires the owning domain hold it exclusively — which transfer guarantees — and concurrent mutation within a domain still needs external synchronization, §3 semantics notes.)

---

## 3. Proposed stdlib addition (ready-to-land text)

### New §4.20 — `BitArray`

*(Numbered §4.20 after §4.19 `PriorityQueue`; adjust at integration time.)*

```bezel
// A fixed-length, bit-packed sequence of boolean values.
// Length is immutable after construction.
// Owned: caller is responsible for disposal (releasing backing storage).
// Sendable: `sealed` (§30.14 rule 1) with a single `owned Array<uint>` backing
// field (rule 2 — a borrowing collection of sendable value elements is sendable),
// so a BitArray is transfer-closed and moves across a domain boundary. See §D9.
public sealed class BitArray: IEnumerable<bool>
  // ── Construction ─────────────────────────────────────────────────────────
  // Create a BitArray of `length` bits, all initialised to false.
  // ArgumentOutOfRangeException if length < 0.
  public constructor(length: int);

  // Create a BitArray of `length` bits, all initialised to `fill`.
  // ArgumentOutOfRangeException if length < 0.
  public constructor(length: int, fill: bool);

  // ── Basic properties ─────────────────────────────────────────────────────
  // Number of bits.  Immutable after construction.
  public readonly property Length: int;

  // ── Indexed access ────────────────────────────────────────────────────────
  // Get or set bit i.  IndexOutOfRangeException if i < 0 or i >= Length.
  public property [i: int]: bool
    get;
    set;
  end;

  // ── Bulk mutation: in-place bitwise operations ────────────────────────────
  // All require other.Length = this.Length; ArgumentException otherwise.
  public method And(other: BitArray);      // this[i] := this[i] and other[i]
  public method Or(other: BitArray);       // this[i] := this[i] or  other[i]
  public method Xor(other: BitArray);      // this[i] := this[i] xor other[i]
  public method Not();                     // this[i] := not this[i]

  // Set every bit to `value`.  O(n/w).
  public method SetAll(value: bool);

  // ── Copy ─────────────────────────────────────────────────────────────────
  // Produce an independent deep copy.
  public method Clone(): owned BitArray;

  // Copy all bits to `dest` starting at `destIndex`.
  // ArgumentException if destIndex + this.Length > dest.Count.
  public method CopyTo(dest: Array<bool>, destIndex: int);

  // ── Population count ─────────────────────────────────────────────────────
  // Number of bits set to true.  O(n/w).
  // Requires stdlib §2 `BitOperations.PopCount` (proposal 0008 / #1437).
  public method PopCount(): int;

  // ── Iteration ─────────────────────────────────────────────────────────────
  // IEnumerable<bool>: for var bit in ba do ... end  visits bits in index order.
  // The for-loop desugaring uses the §29.2 direct-indexed-loop optimisation;
  // no heap enumerator is allocated in the typical loop case.

  destructor;    // releases the internal packed storage
end BitArray;
```

**Semantics notes:**

- **Thread safety.** `BitArray` is not synchronized. Concurrent reads are safe; concurrent mutation (any combination of set, And, Or, Xor, Not, SetAll) requires external synchronization (e.g., a `Mutex` per §6.2).
- **Iteration invalidation.** Mutating a `BitArray` while iterating it via `IEnumerable<bool>` raises `InvalidOperationException` on the next `MoveNext()`, matching the standard collection invalidation rule (§29.12).
- **`Clone`** returns an `owned BitArray` with the same `Length` and bit contents. The two arrays are independent after the call; mutations to one do not affect the other.
- **`PopCount` dependency.** If proposal 0008 has not landed, `PopCount` may be deferred to a follow-up commit. The rest of `BitArray` has no dependency on 0008.

---

## 4. Usage examples

```bezel
// Sieve of Eratosthenes up to N.
public method SievePrimes(n: int): Array<int>
  using var sieve := new BitArray(n + 1, true);
  sieve[0] := false;
  sieve[1] := false;
  var i := 2;
  while i * i <= n do
    if sieve[i] then
      var j := i * i;
      while j <= n do
        sieve[j] := false;
        j := j + i;
      end;
    end;
    i := i + 1;
  end;
  var primes := new Array<int>();
  for var k := 0; k < n + 1; k := k + 1 do
    if sieve[k] then primes.Add(k); end;
  end;
  return primes;
end SievePrimes;
```

```bezel
// Bulk membership: intersect two permission sets.
public method IntersectPermissions(
    userPerms: BitArray, rolePerms: BitArray): owned BitArray
  var result := userPerms.Clone();
  result.And(rolePerms);     // in-place; result holds the intersection
  return result;
end IntersectPermissions;
```

```bezel
// Iterate set bits.
for var i := 0; i < ba.Length; i := i + 1 do
  if ba[i] then
    Console.WriteLine($"Bit {i} is set");
  end;
end;
```

---

## 5. What v1 deliberately leaves out

- **Resize** (`Length` setter) — fixed-length is the right first shape; resizing adds mutable-size semantics that belong to a separate design pass.
- **`OwningBitArray` / bit-packed owning collection** — ownership over boolean values is not meaningful in the same sense as `OwningArray<T where T: class>`; this is a borrowing collection by nature.
- **Shared (multi-domain concurrent) access** — `BitArray` is **sendable** (movable across a domain boundary by ownership transfer, §D9), but it is not a *shared* concurrent structure: two domains cannot hold and mutate one instance simultaneously. Cross-domain hand-off is by transfer (spawn argument or owned spawn result), not by sharing.
- **`CopyFrom(Array<bool>)` constructor overload** — converting an `Array<bool>` into a `BitArray` is additive and deferred.
- **`GetHashCode` / equality** — `BitArray` is a mutable class; reference equality is the default (§8.3). Structural equality is deferred (it requires iterating all bits and adds hash instability for mutable instances).

---

## 6. Declined alternatives (register)

1. **Value record instead of class** — `BitArray` is mutable and owns backing storage; value records with in-place mutation are "inadvisable" (§9.4). (§D1)
2. **New module (e.g., `System.Collections`)** — `BitArray` is a collection; §4 (`System.Collections.Generic`) is the natural home, beside `Array<T>`/`Queue<T>`. No new module needed. (Like the rest of §4 it is auto-imported — v0.289; no `import System.Collections.Generic` line is needed.) (§D1)
3. **Count instead of Length** — `Count` signals resizable semantics; `Length` matches `OwningBuffer<T>` and the fixed-size convention. (§D2)
4. **Return-new (non-mutating) And/Or/Xor** — allocates an intermediate per operation; in-place matches the collection mutation conventions of §4 (`Array<T>.Sort`, `Array<T>.Reverse`). `Clone` covers the non-destructive need. (§D3)
5. **No `IEnumerable<bool>` (indexed-only)** — `BitArray` is concrete, not generic; the §4.14 conditional-interface limitation does not apply; direct conformance unlocks `for`-loops and LINQ at no extra cost. (§D8)
6. **`CopyTo(Array<uint>, int)` integer-form** — bit-layout is an implementation detail; exposing `uint` words as a contract locks in the internal representation. Declined in favor of the typed `Array<bool>` form. (§D6)
