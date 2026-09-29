# Proposal 0008 — the integer toolkit: wrapping/saturating arithmetic, `BitOperations`, `HashCode`

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Tracking issue** | [#1437](https://github.com/s8lap/bezel-lang/issues/1437) |
| **Target module(s)** | `System` (auto-imported): the primitive-wrapper records (`Int32`/`UInt32`/`Int64`/`UInt64`, stdlib §9.1), a new `BitOperations` static class (sibling to `Math` §2.2 and `BitConverter` §2.12), a new `HashCode` value record |
| **Written against** | Language spec **v0.177**; stdlib companion v0.165 |
| **Origin** | Gap review [#1403](https://github.com/s8lap/bezel-lang/issues/1403) item #9 (2×P0+P1), wave D1 of the [stdlib gap DESIGN plan](stdlib-gap-design-plan-2026-07.md) |
| **Unblocks** | Gap-review #27 (CRC-32/xxHash checksums, `System.IO.Hashing`); every custom `IEqualityComparer` / manual `GetHashCode` that needs a combiner better than XOR |
| **Currency note** | Refreshed 2026-08-11 to v0.246: `hashable` vocabulary (v0.242; formerly *equality-bearing*). The 2026-08-11 currency paper's flag 1 — the §2/§3/§4.2 arguments leaning on §6.5's repealed silent shift-count masking (the paper cites the rotation contract as "§5.2"; its home here is §4.2/§4.4) — is **resolved**: §2/§3.2/§4.2/§4.4 re-derived against v0.242's count-domain fault under the owner's 2026-08-12 direction on the recorded flags; see "Delegated ruling 2026-08-12" below. Refreshed 2026-09-17 to v0.306: the three citations of spec §25.3's deleted `Counter`/`operator ++` example re-pointed at §25.3's live `Vector2` `operator +` (v0.289 retired `operator ++`/`--` declarations; §17.6 now spells advancement `x := x + 1`); §6.1's and §6.5's quoted sentences re-checked verbatim against the landed text. One tier-2 flag recorded in `proposals-currency-2026-09-17.md` §3 (§5.4/§5.3/§7's "§9.4 leaves value-receiver mutation unspecified" premise). Written-against pin unchanged. |

## 1. The problem, stated against §6

Bezel checks integer overflow *everywhere*. **§6.1 (v0.177):** "Operators `+`, `-`, `*`, `/`, `mod`, unary `-`, `++`, and `--` on integer types are checked for overflow. An operation whose mathematical result is not representable in the result type raises `OverflowException`." The compound forms (`+=`, `-=`, `*=`, `/=`; §6.6) inherit this, and there is **no `unchecked` block** in the language — the only "unchecked" in the spec is the *disposal* mode of §28.2, an unrelated concept. There is also no `Int128`/`UInt128` in the type tower (§4.2 stops at 64 bits).

This is the right default: silent wrap-around is a classic source of security bugs, and Bezel's whole posture is "no hidden control flow, no silent surprises." But three real tasks need to step *outside* that default, deliberately and legibly:

1. **Modular (wrap-around) arithmetic** is the *definition* of the non-cryptographic hash and checksum family — xxHash64, MurmurHash3, FNV-1a, CRC all specify their mixing steps as arithmetic mod 2³² or mod 2⁶⁴. Under §6.1 the very first `h * prime` faults.
2. **Saturating arithmetic** — clamp to the type's range instead of faulting — is the natural model for bounded counters, audio/DSP sample math, and "add but never exceed capacity" accumulators.
3. **Non-throwing overflow detection** — "give me the sum, or tell me it overflowed, without an exception" — is wanted at validation boundaries (parsing a length field, budgeting an allocation) where an exception is heavier than the situation warrants.

Additionally, bit-level queries (population count, leading/trailing zeros, `IsPow2`, `Log2`) and a real hash combiner have no home in the stdlib today. This proposal supplies all four as pure, value-in/value-out surfaces with no ownership or domain interaction.

## 2. Verifying the gap review's "unwritable" claim (design question 4)

The gap review states a 64-bit wrapping multiply is "*unwritable* in Bezel." **The claim holds in substance, with one precise qualification worth recording**, because it strengthens the case for the primitive rather than weakening it.

**The 32-bit case has a clumsy but real workaround.** A `uint`×`uint` product fits in `ulong` — `(2³²−1)² = 2⁶⁴ − 2³³ + 1 < 2⁶⁴` — so widening first avoids the fault, and the low half is recovered by masking:

```bezel
// 32-bit wrapping multiply, writable today.
public method WrappingMul32(a: uint, b: uint): uint
  var wide: ulong := ulong(a) * b;        // uint→ulong promotion (§5.1/§6.2); product < 2^64, no overflow
  return uint(wide & 0xFFFFFFFFUL);       // low 32 bits; the narrowing cast fits, so no OverflowException
end WrappingMul32;
```

**The 64-bit case has no such workaround, because there is no 128-bit integer to widen into.** A schoolbook decomposition into 32-bit limbs needs to sum the cross terms `aLo*bHi + aHi*bLo`, each up to ≈2⁶⁴, whose sum is ≈2⁶⁵ — and that `+` is a checked `ulong` add, which faults. So a 64-bit wrapping multiply reduces to needing a 64-bit **wrapping add**, which §6.1 also withholds.

**Strictly, a `ulong` wrapping add *is* expressible** — via a bitwise ripple-carry adder built only from §6.5 operations, every step of which is fault-free under the landed rules: `&` and `^` are not among §6.1's overflow-checked operators, and the adder's only shift is by the constant `1` — in-domain for every width — where §6.5 (v0.242) states that "bits shifted beyond the left operand's width are discarded silently — that is the operation, exactly as `&` discards bits — and the result never faults." *(Through v0.241 the exemption was blanket — "a shift never raises `OverflowException`" — v0.242's count-domain fault, proposal 0079, narrowed it to in-domain counts, which is all this construction ever uses; re-derived per the delegated ruling below.)*

```bezel
// (a + b) mod 2^64, using only §6.5 bit ops — never the checked §6.1 `+`.
public method WrappingAdd64(a: ulong, b: ulong): ulong
  var x := a;
  var y := b;
  while y <> 0 do
    var carry := (x & y) << 1;            // §6.5: count 1 is in-domain; the bit past the width is discarded — no fault
    x := x ^ y;                           // §6.5: `^` never faults
    y := carry;
  end;
  return x;                               // terminates in ≤ 64 iterations (carry marches left)
end WrappingAdd64;
```

A `ulong` wrapping multiply then follows by shift-and-add over this adder. **So "unwritable" is, to the letter, "not *practically* writable":** the only path is a bit-serial software adder at roughly O(width²) bit operations per multiply (~4000 operations to replace one hardware `imul`), utterly illegible, and three-to-four orders of magnitude too slow for a hash inner loop that runs per 8 bytes of input. No competent implementation of xxHash64 can be written this way. The gap review's conclusion stands; the honest phrasing is "unwritable in practice," and the 64-bit-specifically-has-no-workaround asymmetry (§4.2 has no `Int128`) is the sharpest single argument for adding the primitive.

## 3. Wrapping / saturating / checked arithmetic (design question 1)

### 3.1 Spelling: named instance methods, not operators, not an `unchecked` block

The overflow behavior of `+` is a **contract at the use site**, not an implementation detail. Changing it must be visible exactly where the arithmetic happens. Three spellings were considered:

- **New operators** (e.g. a `*%` wrapping-multiply token) — *declined*. It grows the lexer and precedence table, and — worse — makes the safety-relevant switch from "checked" to "wrapping" as visually quiet as a one-character typo. Bezel already refuses operator proliferation of this kind: `~` and `not` are kept distinct and non-interchangeable (§6.5), and `=`/`<>` are not overloadable on classes (§8.4). A wrapping multiply that reads almost like an ordinary multiply is precisely the footgun the checked default exists to prevent.
- **An `unchecked { … }` block** (C#'s answer) — *declined*. A block silently reclassifies *every* operation inside it, including ones the author did not mean to make wrapping; the deviation is scoped to a region, not attached to an operation. It is also a language change (new statement form) rather than a stdlib addition, so it would go through the spec workflow, not a proposal.
- **Named methods** — *chosen*. `a.WrappingMul(b)` states the policy at the operation, reads left-to-right as a pipeline (`h.WrappingMul(prime).WrappingAdd(block)`), and matches the precedent the gap review cites — Rust's `wrapping_mul` / `saturating_add` / `checked_add`. This is the same "make the unusual thing legible" discipline as `move`, `owned`, and the `Ieee`/`Bitwise` equality variants (§4.3.1).

**Instance, not static.** The gap review sketched a static form (`UInt64.WrappingMul(a, b)`); this proposal uses instance methods (`a.WrappingMul(b)`) instead. Rationale: the primitive wrappers already carry their operations as instance members (`x.CompareTo(y)`, `x.ToString()`), instance form composes in hash pipelines without repeating the type name, and — because the receiver's type is authoritative — it forces both operands to the *same* width with no silent §6.2 promotion sneaking a wider result in. The static form is noted as the declined alternative.

### 3.2 The full matrix, and the v1 cut

Policies × operations, with the v1 decision in each cell:

| | `Add` | `Sub` | `Mul` | `Neg` | `Div` | `Shl` |
|---|---|---|---|---|---|---|
| **Wrapping** | ✅ v1 | ✅ v1 | ✅ v1 | ✅ v1 | ❌ | ❌ (declined) |
| **Saturating** | ✅ v1 | ✅ v1 | ✅ v1 | ⏳ later | ❌ | ❌ |
| **Checked (→ `T?`)** | ✅ v1 | ✅ v1 | ✅ v1 | ⏳ later | ❌ | — |

Cuts, with reasons:

- **`Shl` is excluded from every row — the exclusion stands, its reason replaced at v0.242 (delegated ruling below).** Through v0.241 the ground was §6.5's silent count masking ("a `WrappingShl` would duplicate the built-in operator"); v0.242 (proposal 0079) repealed the masking — `<<` now faults on an out-of-domain count — so that ground is gone. The exclusion is re-reasoned, not reopened: (i) the `Wrapping*` family wraps *results* mod 2^w where §6.1 would fault, but an in-domain shift's result never faults (discarding the high bits *is* the operation, §6.5) — there is no checked-result behavior for a wrapping shift to step around, so a count-masking `WrappingShl` would be a false member of the family, policy-switching the count operand rather than the result; (ii) the deliberately-masked count already has its sanctioned spelling in the spec itself — §6.5 (v0.242) designates `v << (k & 31)` (`& 63` for 64-bit left operands) as the modular-count form, the mask in the source, visible and in-domain by construction — and a `WrappingShl(k)` would be a second, less legible spelling of the same thing; (iii) the consumers this proposal serves (§1's hash/checksum kernels) shift by in-domain constants and need neither.
- **`Div` is excluded from v1.** Integer division overflows in exactly one case — `MinValue / -1` on a signed type — and faults on `/ 0` (`DivideByZeroException`, §6.1), which is a genuine error no policy should paper over. The single overflowing case is niche; defer `WrappingDiv`/`CheckedDiv` until a consumer needs it.
- **`Neg`:** `WrappingNeg` is in v1 (it completes the modular set cheaply — `WrappingNeg(MinValue) = MinValue`). `SaturatingNeg`/`CheckedNeg` (only `MinValue` negation is interesting) are deferred as trivially-additive.
- **Saturating and checked stay `Add`/`Sub`/`Mul`** — the three operations that actually overflow in practice.

**Which types.** v1 covers **`int`, `uint`, `long`, `ulong`** — the 32- and 64-bit signed and unsigned types. Reasons: hashing and checksums live in the `uint`/`ulong` domain; general modular counters and ring indices live in `int`/`long`; and the narrow types (`byte`/`sbyte`/`short`/`ushort`) are rarely the intended site of a deliberate wrap and mostly compute in int-space anyway. Adding the four narrow types later is mechanical and additive. `float`/`double` are excluded (overflow already yields `±Infinity`, not a fault — §6.3 — so "wrapping"/"saturating" have no meaning); `decimal` and `char` are excluded (no modular reading).

### 3.3 Checked = the Try convention, shape (b)

The plain `+` is *already* the throwing checked add (it raises `OverflowException`). The value a "checked" method adds is therefore the **non-throwing** form: compute the result, or signal overflow without an exception. That is exactly a nullable return — the stdlib Try convention **shape (b)**, `TryX(...): T?` "when null alone is the signal" (stdlib §1, v0.171). For a non-nullable value type like `int`, a `null` result is unambiguous: it *means* overflow.

The methods are named `CheckedAdd`/`CheckedSub`/`CheckedMul` (returning `int?`) rather than `TryAdd`/… — chosen for parallelism with `WrappingAdd`/`SaturatingAdd` (three overflow *policies*, uniformly prefixed) and to match the cross-language term "checked arithmetic." They *are* the shape-(b) Try convention; only the prefix differs from the literal `Try`. **Open question for the owner:** keep `Checked*` for the parallel family, or rename to `Try*` for strict convention adherence? (§8.)

### 3.4 Proposed surface

On `Int32` (aliased `int`); `UInt32`, `Int64`, `UInt64` are analogous with their own widths. All are compiler-intrinsic (no source body), consistent with the existing members of §9.1.

```bezel
public record Int32: IComparable<int>, IEquatable<int>
  // ... existing members (MinValue/MaxValue, Parse, ToString, CompareTo) unchanged ...

  // Wrapping (two's-complement modular; never faults). WrappingAdd(MaxValue, 1) = MinValue.
  public method WrappingAdd(other: int): int;
  public method WrappingSub(other: int): int;
  public method WrappingMul(other: int): int;
  public method WrappingNeg(): int;             // WrappingNeg(MinValue) = MinValue

  // Saturating (clamp to [MinValue, MaxValue]; never faults).
  public method SaturatingAdd(other: int): int; // SaturatingAdd(MaxValue, 1) = MaxValue
  public method SaturatingSub(other: int): int; // SaturatingSub(MinValue, 1) = MinValue
  public method SaturatingMul(other: int): int;

  // Checked (Try convention, shape (b): null iff the exact result is out of range).
  public method CheckedAdd(other: int): int?;
  public method CheckedSub(other: int): int?;
  public method CheckedMul(other: int): int?;
end Int32;
```

`UInt64.WrappingMul` — the operation the gap review called unwritable — becomes a single intrinsic call. A 64-bit FNV-1a step reads:

```bezel
// FNV-1a over bytes; hash and prime are ulong, all steps modular.
var hash: ulong := 14695981039346656037UL;
const prime: ulong := 1099511628211UL;
for var b in data do
  hash := (hash ^ ulong(b)).WrappingMul(prime);
end;
```

The `^` stays the ordinary §6.5 operator (`&`/`|`/`^`/`~` never fault); only the multiply — the operation §6.1 would fault on — takes the explicit `WrappingMul`.

### 3.5 Composition with constraint-generic numeric code (§11.8)

§11.8 / stdlib §2.19 give `INumber<T>` with *checked* static operators (`+`, `*`, …), and the primitive tower conforms intrinsically. Generic numeric code written against `INumber<T>` today gets the checked semantics — correct for general arithmetic. Wrapping/saturating are deliberately **not** added to `INumber<T>`: they are a different contract (a wrapping `+` in a `Sum<T>` would silently change that function's overflow behavior for every caller). The clean way to write *generic* wrapping code is a future dedicated static interface — an `IBinaryInteger<T>`-style abstraction exposing `WrappingMul` etc. as static members — which is exactly the kind of promotion §11.8's "recorded follow-ups" anticipate. That is **deferred**: v1 ships the concrete per-type methods (which unblock the concrete consumers, #27), and the generic surface waits for a consumer that is itself generic. Recorded so the eventual static interface is designed against, not bolted onto, `INumber<T>`.

## 4. `BitOperations` (design question 2)

### 4.1 Home and shape: a static class in `System`

A static class, sibling to `Math` (§2.2) and `BitConverter` (§2.12) in auto-imported `System` — matching where `BitConverter`, its closest relative, already lives (Bezel has no `System.Numerics` module, .NET's home for it). Primitive-member and `INumber`-style-interface alternatives are declined (§4.3).

### 4.2 v1 surface — `uint`/`ulong` only

```bezel
public static class BitOperations
  // Population / zero counts. LeadingZeroCount(0) = width; TrailingZeroCount(0) = width.
  public static method PopCount(value: uint): int;
  public static method PopCount(value: ulong): int;
  public static method LeadingZeroCount(value: uint): int;    // 0..32
  public static method LeadingZeroCount(value: ulong): int;   // 0..64
  public static method TrailingZeroCount(value: uint): int;   // 0..32
  public static method TrailingZeroCount(value: ulong): int;  // 0..64

  // Rotations. A rotation of a w-bit lane is periodic in w — rotate-by-width is the identity —
  //   so `offset` is reduced modulo the width as the operation's own definition; never faults.
  public static method RotateLeft(value: uint, offset: int): uint;
  public static method RotateLeft(value: ulong, offset: int): ulong;
  public static method RotateRight(value: uint, offset: int): uint;
  public static method RotateRight(value: ulong, offset: int): ulong;

  // Power-of-two predicates and helpers.
  public static method IsPow2(value: uint): bool;             // IsPow2(0) = false
  public static method IsPow2(value: ulong): bool;
  public static method Log2(value: uint): int;                // floor(log2); ArgumentOutOfRangeException on 0
  public static method Log2(value: ulong): int;
  public static method RoundUpToPowerOf2(value: uint): uint;  // OverflowException if the result exceeds the type
  public static method RoundUpToPowerOf2(value: ulong): ulong;
end BitOperations;
```

**Why `uint`/`ulong` only in v1.** Bit-twiddling is an unsigned-domain activity: leading-zero-count, `Log2`, and rotation all become footguns on a signed value whose sign bit is set (`LeadingZeroCount(-1)` = 0; `Log2` of a negative is undefined; rotating drags the sign bit). Restricting v1 to the two unsigned widths keeps every operation unambiguous. A caller holding an `int`/`long` bit pattern views it under the unsigned type with the no-fault, no-copy `BitConverter.Reinterpret<uint>(x)` (§2.12), or the checked `uint(x)` cast when a *value* conversion is intended. Narrow (`byte`/`ushort`) and signed (`int`/`long`) overloads for the sign-safe subset (`PopCount`, `IsPow2`) are a justified later addition, kept out of v1 to hold the surface small.

### 4.3 Declined homes

- **Members on the primitive wrappers** (`x.PopCount()`) — declined. These are queries *over a bit pattern*, not per-number behavior; grouping them in one discoverable class mirrors `Math` and reads better (`BitOperations.Log2(cap)`), and it keeps the width-overload set in one place instead of scattered across eight records.
- **An `INumber`-style static interface** (`IBinaryInteger<T>.PopCount`) — declined *for v1*, same reasoning as §3.5: it is the right *eventual* generic surface (and pairs with the deferred generic wrapping interface), but there is no generic consumer yet, and static-interface authoring is heavier than a static class. The static-class methods can become the witnesses for such an interface later without a call-site change.

### 4.4 Edge-value faults, stated exactly (design question 2)

Bezel's philosophy is defined faults, not silent sentinels (§6.1's `OverflowException`/`DivideByZeroException` set the pattern; .NET's `BitOperations` instead returns 0 for these, which this proposal deliberately does not follow):

- `Log2(0)` — `floor(log2 0)` does not exist. **Raises `ArgumentOutOfRangeException`** (the argument is outside the operation's domain, as with the invalid-component `DateTime` constructor, §2.5). It never returns a sentinel.
- `RoundUpToPowerOf2(value)` when the next power of two is not representable — for `uint`, any `value` in `(2³¹, 2³²)` rounds up to `2³²`, which does not fit; for `ulong`, any `value` in `(2⁶³, 2⁶⁴)`. **Raises `OverflowException`**, consistent with §6.1's treatment of a non-representable result.
- `RoundUpToPowerOf2(0)` = `1` and `RoundUpToPowerOf2(1)` = `1` (the least power of two ≥ the input; defined, no fault).
- `LeadingZeroCount(0)` / `TrailingZeroCount(0)` = the full width (32 or 64) — defined, total.
- `PopCount`, `IsPow2`, `RotateLeft`, `RotateRight` — total on all inputs; rotation reduces `offset` modulo the width **on its own periodicity ground** (rotate-by-width is the identity) and never faults — deliberately *not* re-aligned to §6.5's v0.242 count-fault domain, which governs shifts, where an out-of-domain count is a programming error rather than a periodic offset (delegated ruling below).

## 5. `HashCode` (design question 3)

### 5.1 What §8.3/§9.2 promise today, and matching it

§8.3 requires only the hash–equality invariant ("if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`") and states that **"the choice of combining algorithm is implementation-defined."** It makes **no cross-process-stability promise** for `GetHashCode` — nothing requires the same value to hash identically across two runs of the program. Auto-generated `GetHashCode` (positional records §9.1, auto-equality body records §9.2) inherits this: the combine is implementation-defined and free to be seeded.

`HashCode` **matches that contract exactly**: it uses a **per-process randomized seed**. Within a single process run every equal value hashes identically (so the §8.3 invariant holds and it is safe for `Dictionary`/`Set` keys); across runs the values differ, which is the standard hash-flood defense (an attacker cannot precompute colliding keys — the same reason .NET seeds `System.HashCode` and string hashing per process). This is a *decision to record*: because §8.3 already permits a seeded auto-hash, `HashCode` and the auto-generated members can share one implementation, and a manual `GetHashCode` built on `HashCode` is indistinguishable from the auto one.

**Consequence, stated loudly:** `HashCode` output is **not** a stable digest. It must never be persisted, sent across a process/domain boundary, or used as a wire identifier. When a *stable* fingerprint is needed, that is a **checksum** — CRC-32/xxHash (gap-review #27), which is deterministic *because* it is built on §3's **wrapping** arithmetic, not on `HashCode`. The two tools are cleanly separated: `HashCode` = randomized, for in-process hash tables; the wrapping ops = deterministic, for on-the-wire checksums.

### 5.2 It is the documented tool for manual `GetHashCode`

A type that overrides `Equals` **must** override `GetHashCode` (§8.3, compiler-enforced), and any body record that declares a manual `Equals`/`GetHashCode` opts *out* of auto-equality (§9.2). `HashCode` is the sanctioned way to write that manual `GetHashCode` — a combiner strictly better than the XOR-fold people reach for (XOR loses information and makes `(a, b)` collide with `(b, a)`):

```bezel
public class Money: IEquatable<Money>
  public readonly property Amount: decimal;
  public readonly property Currency: string;

  public override method Equals(other: Object?): bool => /* ... */;
  public override method GetHashCode(): int =>
    HashCode.Combine(Amount, Currency);        // order-dependent, allocation-free
end Money;
```

### 5.3 Surface — an immutable-chaining value record

`HashCode` is a **compiler-intrinsic value record** wrapping one word of fold state — the same shape as `Int32` (`public record Int32`, §9.1) and the generic `Range<T>` (§3.4). It is **immutable-chaining**: each `Add` returns a **new** `HashCode` value (§25.3's `Vector2` operator idiom — value in, fresh value out), so there is no heap allocation, no `Dispose`/`using`, and — decisively — **no value-receiver mutation** (the exact thing §9.4 leaves unspecified). One word of primitive state means it is not borrow-bearing (§9.7), not move-only (§15.3), and sendable (§29): copies are one register, free.

```bezel
public record HashCode
  // Fresh accumulator carrying the per-process randomized seed (§5.1).
  public static method Start(): HashCode;

  // Fold one value's hash in (order-dependent); returns a NEW HashCode value.
  public method Add<T>(value: T): HashCode;
  public method AddRange<T>(values: IEnumerable<T>): HashCode;

  // Finalize. A pure projection of the folded state — idempotent and re-readable.
  public readonly property Value: int;

  // Allocation-free convenience for a fixed, known set of fields (the common case).
  public static method Combine<T1>(v1: T1): int;
  public static method Combine<T1, T2>(v1: T1, v2: T2): int;
  public static method Combine<T1, T2, T3>(v1: T1, v2: T2, v3: T3): int;
  // ... through arity 8, same shape (matching the field counts real records reach).
end HashCode;
```

Usage — chained fixed + streaming in one expression, zero heap:

```bezel
public override method GetHashCode(): int
  return HashCode.Start().Add(_id).AddRange(_tags).Value;
end GetHashCode;
```

When `AddRange` is not enough, the loop form folds with plain value reassignment (`h := h.Add(e)` is a value copy — legal by §15.3/§28.6, since a value record with no owned fields has no ownership state):

```bezel
var h := HashCode.Start();
for var e in elems do
  h := h.Add(e);                // reassign the running value
end;
return h.Value;
```

**`default(HashCode)` footgun (documented).** As a value record, `default(HashCode)` (§17.11) yields the all-zero *unseeded* state, **not** the seeded `Start()` state. `Start()` is the blessed entry point and the only supported way to begin an accumulation; `HashCode` deliberately exposes no meaningful `default` and no public constructor. (This is the one wrinkle the class shape would not have had — a small, documentable cost against the record's allocation-freedom, sendability, and no-ceremony copies.) `HashCode` is also not itself hashable — its hidden state is not an exposed auto-property (§9.2) — so `.Equals`/`.GetHashCode` on an accumulator is a §9.2 use-site error; that is correct and harmless, as accumulators are never compared.

### 5.4 Why the immutable-chaining value record — and the eligibility of `T`

**An immutable-chaining value record — no receiver mutation needed.** The tempting reading is "a builder is a class" (every mutable builder the stdlib ships — `StringBuilder`, `Random`, `AtomicInteger` — is one). But those wrap *growable/multi-word/heap-backed* mutable state where copies are expensive or wrong; a one-word hash fold is the opposite case, where copies are free and immutable-chaining is natural. The only shape that would have forced a class is a **mutable value record** whose `Add` writes `this._state` and depends on that write being visible to the caller — and §9.4 is the relevant fact here: it (i) permits mutable body-record *properties*, (ii) defines **no** `mutating`-method receiver model (a grep of spec and rationale finds no value-receiver-mutation semantics at all), and (iii) explicitly calls mutable records **"inadvisable."** The immutable-chaining record sidesteps (i)/(ii) entirely — `Add` never writes a receiver field; it reads its receiver's folded state, computes a new state, and **returns a new value**, exactly §25.3's `Vector2` (`public static method operator +(left: Vector2, right: Vector2): Vector2 => new Vector2(…)`), the value-record operator shape §17.6 now names as the way a type spells advancement — "a type spells advancement as `x := x + 1` under its own `operator +`" (`operator ++`/`--` declarations were retired at v0.289) — and *aligns* with (iii). So the earlier conclusion flips: **no mutation is needed at all**, and no value-receiver-mutation ruling is a prerequisite. The record needs no heap allocation on any path (`Combine` and the streaming fold both allocate nothing), no `Dispose`, and is sendable. Precedent is direct: `Int32` is itself a compiler-intrinsic value record wrapping one word (§9.1), and `Range<T>` a compiler-intrinsic *generic* value record (§3.4) — `HashCode` with a generic value-in/value-out `Add<T>` is the same shape. The class shape survives only as a fallback if an owner ruling ever forbids value-record generic intrinsics.

**Eligibility of `T` (why no `where` clause).** `Add<T>`/`Combine<…>` are **compiler-intrinsic generics** with a compiler-enforced eligibility rule, in the manner of `Range<T>`'s "integer-only" restriction (spec §16.3) and `Atomic<T>`'s "cell-sized primitive" restriction (§6.4) — restrictions the type system cannot phrase as a `where` clause. The rule: **`T` must be a type hashable under §8.3** — the compiler hashes each argument exactly as it would inside an auto-generated `GetHashCode`, so every admissible auto-equality component type (primitive, enum, `string`, `T?`, admissible inline array, and any auto-hashable record — the §9.2 set) is accepted, and classes contribute their `GetHashCode` (identity or override). Passing a value whose static type is a **non-auto-hashable record** is the *same compile error* §9.2 already raises for `.GetHashCode()` on such a type, pointing to the same fix (declare custom equality, or simplify to an auto-equality form). This is deliberately *not* a `where T: IEquatable<T>` bound: that would wrongly exclude enums (whose `IEquatable` conformance is not established by §2.9) and the nullable/inline-array components §9.2 admits, while adding nothing the intrinsic rule doesn't already give. The declined `IEquatable<T>` constraint is recorded in §7.

## 6. What v1 deliberately leaves out

- Narrow-type (`byte`/`sbyte`/`short`/`ushort`) and signed-type overloads of `BitOperations`.
- `WrappingDiv`/`CheckedDiv`, `SaturatingNeg`/`CheckedNeg`, and any wrapping/saturating on the narrow types.
- A generic static interface for wrapping ops / bit ops (`IBinaryInteger<T>`-style) — deferred to a generic consumer (§3.5, §4.3).
- The checksums themselves (CRC-32/xxHash, #27) — they are the *consumer* of this toolkit, a separate item.

*(The v1 `HashCode` **is** a value record — the immutable-chaining form of §5.3/§5.4 — so it is no longer on this "leaves out" list. No value-receiver-mutation ruling was needed.)*

## 7. Declined alternatives (register)

1. **New wrapping/saturating operators** (`*%`, etc.) — hides a safety-relevant policy change behind quiet syntax; against §6.5/§8.4's anti-operator-proliferation posture (§3.1).
2. **A C#-style `unchecked { }` block** — reclassifies *every* operation in a region, not the one you meant; and it is a language change, not a stdlib addition (§3.1).
3. **Static `WrappingMul(a, b)` form** (as the gap review sketched) — declined for the instance form, which composes in pipelines and forces same-width operands (§3.1).
4. **Adding wrapping/saturating to `INumber<T>`** — a wrapping `+` in a generic `Sum<T>` would silently change overflow behavior for all callers; the generic surface is a *separate* static interface, deferred (§3.5).
5. **.NET `BitOperations` sentinel semantics** (`Log2(0) = 0`, `RoundUpToPowerOf2` overflow → 0) — declined for defined faults (`ArgumentOutOfRangeException`/`OverflowException`), per Bezel's no-silent-sentinel philosophy (§4.4).
6. **`BitOperations` as primitive members or as a static interface** — declined for a discoverable static class mirroring `Math`/`BitConverter` (§4.3).
7. **A *mutable* value-record `HashCode`** (in-place `Add` mutating `this`) — declined: it is the one shape that depends on the unspecified value-receiver-mutation semantics (§9.4 permits mutable properties but defines no mutating-method receiver model), and §9.4 calls mutable records "inadvisable." Note: the **immutable-chaining** value record (each `Add` returns a new value, §25.3's `Vector2` operator idiom) is a *distinct* option that needs no such ruling — it is the **chosen v1 shape** (§5.3/§5.4), not a declined one.
8. **`where T: IEquatable<T>` on `Add`/`Combine`** — excludes enums and nullable/inline-array components that §9.2 admits; the compiler-intrinsic "hashable under §8.3" rule is broader and reuses §9.2's footgun protection (§5.4).
9. **A stable/deterministic `HashCode`** — declined; determinism belongs to checksums (#27), and a stable in-process hash forgoes hash-flood defense that §8.3 permits us to have (§5.1).
10. **Typed modular arithmetic** (an Ada-style modular *type* whose ordinary `+`/`*` wrap by declaration, rather than wrapping *methods* on the ordinary types) — declined (v0.263, recorded from proposal 0106 §1.4, which found this form covered only by adjacency to items 1 and 2 and asked that it be made explicit). It answers item 1's objection better than item 1's own candidate did: the safety-relevant switch is stated once, at the type, and is not a one-character-typo away from checked arithmetic — so it is declined on **cost**, not on legibility. A wrapping type is a new arithmetic type kind, and Bezel's integer surface is fixed: it would need its own conversion rules against every existing integer type (spec §5.1/§5.2 admit no user-defined conversions to extend, §25.4), its own `INumber<T>` conformance question (§3.5's obstruction, at a type that cannot honestly witness the checked contract), its own literal and constant-folding behavior, and its own FFI classification — the full new-kind treatment, for a capability the `Wrapping*` methods already deliver at the call sites that need it (hashing, checksums, PRNGs), where the wrap is *local* to the operation rather than a property of every value that flows through the type. **Reopening bar:** evidence that real code needs whole *regions* of wrapping arithmetic — a hash or cipher implementation where the per-operation spelling is measured as the dominant noise — at which point the question is a type kind and takes its own proposal, not an addition here.

## 8. Open questions for the owner — RESOLVED (2026-07-04)

All four are resolved as follows (owner batch confirmations, 2026-07-04):

1. **Checked-op naming — `Checked*` KEPT.** The v0.171 Try convention constrains **Try-named** methods (their `bool`/nullable-return shape); it does **not** reserve nullable-return for that prefix. `CheckedAdd`/`CheckedSub`/`CheckedMul` name the *semantics* (checked arithmetic), stay parallel with `Wrapping*`/`Saturating*`, avoid false kinship with collection `TryAdd`, and match the cross-language term (Rust's `checked_add`). They remain shape-(b) Try methods in behavior; only the prefix differs.
2. **`HashCode` shape — DISSOLVED (value record, no ruling needed).** The former dichotomy (mutable-value-record-needs-a-ruling vs class) had a false gap: the **immutable-chaining** value record is a third option available *today* that touches neither the mutation-semantics problem nor §9.4's inadvisability. It is the chosen v1 shape (§5.3/§5.4). No value-type-receiver-mutation ruling is required. (The residual, narrower confirm — `Value` property vs a `ToHashCode()` finalizer, and `Start()` factory vs a public constructor — is a §5.3 surface detail, settled there as `Value` + `Start()`.)
3. **v1 type coverage — CONFIRMED `int`/`uint`/`long`/`ulong` only.** `byte`/`short` arithmetic *does* exist (§6.2/§6.5), so restricting v1 to the 32/64-bit types is **scoping, not declining** — the narrow-type overloads are mechanical, additive, and deferred, not forbidden (§3.2).
4. **`HashCode` seeding — CONFIRMED per-process randomization.** §8.3's auto-hash combining is implementation-defined and makes no cross-process stability promise, so a per-process randomized seed violates no stability contract while giving the standard hash-flood defense (§5.1). A stable seed is deliberately *not* adopted; determinism belongs to checksums (#27).

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

All four owner questions were **already resolved in the owner batch of 2026-07-04** and are recorded verbatim in §8; acceptance carries them unchanged. In summary: (1) checked-op naming keeps the `Checked*` prefix (it names the semantics and stays parallel with `Wrapping*`/`Saturating*`; still shape-(b) Try behavior); (2) `HashCode` is the immutable-chaining value record of §5.3/§5.4 — the dichotomy dissolved, no value-receiver-mutation ruling is required; (3) v1 type coverage is `int`/`uint`/`long`/`ulong` only (scoping, not declining — the narrow-type overloads are mechanical and deferred); (4) `HashCode` uses per-process randomized seeding (§8.3 permits it, gives hash-flood defense; determinism belongs to checksums, #27). Nothing in §8 remained open at acceptance.

## Delegated ruling 2026-08-12 (currency flag 1 — re-grounding on the v0.242 shift-count fault)

Recorded under the owner's 2026-08-12 direction on the recorded flags ([2026-08-11 currency paper](proposals-currency-2026-08-11.md) §3 flag 1; option (i) as recommended — re-ground, no surface change). v0.242 (proposal 0079) repealed §6.5's silent shift-count masking: `<<`/`>>` now fault on an out-of-domain count (compile error for constants, `OverflowException` at runtime), in-domain shifts are bit-for-bit unchanged, and the deliberately-masked count has exactly one sanctioned spelling — **`v << (k & 31)`** (`& 63` for 64-bit left operands), the mask in the source, visible and in-domain by construction (§6.5, v0.242). Three of this proposal's arguments cited the repealed rule; each is re-derived in place above, and none of the proposal's decisions changes:

1. **Rotation offsets (§4.2/§4.4) stay modular — on the operation's own ground, not by analogy.** The old contract comment justified `offset mod width` "as shift counts are (§6.5)"; that precedent now points the other way. The re-grounding: a rotation of a w-bit lane is inherently periodic — rotate-by-width is the identity, rotate-by-k *is* rotate-by-(k mod w) — so the modular reduction is the operation's own definition, not a policy import. Re-aligning rotation offsets to §6.5's `0..width−1` fault domain (the flag's option (ii)) is **declined**: for a shift, an out-of-domain count has no in-domain meaning and the fault catches a programming error; for a rotation, every offset has one canonical meaning. No behavior change to the proposed contract.
2. **The §3 `WrappingShl` exclusion stands — with its reason replaced** (the flag's option (iii), adding a `Wrapping` shift row on the changed ground, is **declined**). The re-worked reasoning is in §3.2: (i) `Wrapping*` wraps *results* where §6.1 would fault, and an in-domain shift's result never faults — so a `WrappingShl` would policy-switch the *count operand*, a false member of the family; (ii) §6.5 (v0.242) itself designates `v << (k & 31)` as the deliberate modular-count spelling, and a method form would be a second, less legible spelling of it; (iii) §1's consumers shift by in-domain constants. Recorded honestly: the exclusion's *reason* changed even though the exclusion stands; if a real consumer ever surfaces with variable, deliberately-modular shift counts, the mask spelling is the answer before any new row is.
3. **The §2 "strictly expressible" claim survives, re-derived.** The ripple-carry adder's steps are `&`/`^` (never overflow-checked) and `<< 1` (a constant, in-domain at every width; in-domain shifts never fault) — so the construction, and the "unwritable in practice" verdict it qualifies, stands under v0.242 exactly as at the v0.177 pin. §2's quotation is re-derived from the landed §6.5 text; the pre-v0.242 blanket wording is retained only as marked history.

## 9. Relationship to existing work

§6.1/§6.5 (the checked-arithmetic default this steps around, and the bit ops it builds on — `&`/`|`/`^`/`~` never fault; shifts fault only on an out-of-domain count since v0.242) · §8.3/§9.2 (the auto-hash contract `HashCode` matches) · §11.8 + stdlib §2.19 (`INumber<T>`, the generic-numeric surface the deferred static interfaces would extend) · stdlib §2.12 `BitConverter` (the sibling `System` bit-surface and the `Reinterpret` bridge for signed inputs) · gap review #1403 item #9 (this proposal) and item #27 (CRC/xxHash — the downstream consumer of §3's wrapping ops) · wave D1 of the [gap DESIGN plan](stdlib-gap-design-plan-2026-07.md).
