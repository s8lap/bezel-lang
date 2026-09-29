# Proposal 0029 — checksums: CRC-32 and XxHash64

| | |
|---|---|
| **Status** | under-review — draft |
| **Tracking issue** | [#1454](https://github.com/s8lap/bezel-lang/issues/1454) |
| **Target module(s)** | `System.IO.Hashing` (new module; requires `import System.IO.Hashing`) |
| **Written against** | Language spec **v0.177**; stdlib companion v0.165 |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); re-verified clean at v0.306 (2026-09-17 sweep) — §4.2's §8.6 span/`IEnumerable` quotation still matches the landed text, `IReadOnlySpan<byte>` (§4.16), `Encoding.ASCII.GetBytes`, `Array<T>(size, fill)`, `new FileStream(path, mode)` (the v0.290 `access := FileAccess.ReadWrite` default leaves the two-argument call unchanged) and `Stream.Read(buffer, offset, count)` (the one recorded `(offset, count)` window exception, stdlib §1) all read as cited. No tier-1 respells, no tier-2 flags. Written-against pin unchanged. |
| **Origin** | Gap review [#1403](https://github.com/s8lap/bezel-lang/issues/1403) item #27 (P1), wave D3 (the design plan lists #27 under wave D3); depends on proposal 0008 (integer toolkit — [#1437](https://github.com/s8lap/bezel-lang/issues/1437)) |

---

## 1. Problem and motivation

CRC-32 and non-cryptographic hash functions are needed for file-integrity checks, network protocol framing (Ethernet FCS, ZIP, PNG, GZIP all use CRC-32), fast content fingerprinting (deduplication, change detection), and hash-table algorithms that need determinism across process restarts (distributed caches, on-disk hash tables). None are provided by the current stdlib.

The characteristic that separates this item from proposal 0008's `HashCode` is **stability across process boundaries**. `HashCode` is seeded per-process (§5.1 of proposal 0008) and is useless for any on-disk or on-wire purpose. CRC-32 and XxHash64 compute **the same value every time for the same input**, everywhere, forever — that is the property that makes them useful for wire protocols and file formats. The two tools are cleanly separated: `HashCode` for in-process hash tables; the algorithms here for deterministic cross-process digests.

**Why wrapping arithmetic is the prerequisite.** CRC-32's inner loop performs polynomial mod-2 mixing (`xor + shift`), which never faults under §6.1 because `^` and `>>` are already non-faulting bit ops (spec §6.5). XxHash64's inner loop mixes with `UInt64.WrappingMul` and `UInt64.WrappingAdd` — modular arithmetic that §6.1's checked default would fault on every iteration. Proposal 0008 supplies exactly these primitives. This proposal therefore depends on 0008 being accepted.

---

## 2. Algorithm selection — v1 cut

Two algorithms are shipped in v1: **CRC-32 (IEEE)** and **XxHash64**. The choice covers two distinct use-case classes:

| Algorithm | Width | Speed | Use-case class |
|-----------|-------|-------|----------------|
| CRC-32 (IEEE) | 32 bits | Moderate | Wire protocols, file format integrity (ZIP, PNG, GZIP, Ethernet FCS) |
| XxHash64 | 64 bits | Very fast | Content fingerprinting, change detection, fast non-crypto hashing |

**Why these two and not others:**

- **CRC-32 (IEEE polynomial `0xEDB88320`)** is the standard; no other polynomial is interoperable with ZIP, PNG, GZIP, or Ethernet frames. It is the only CRC-32 variant a stdlib needs; any other CRC polynomial would be a specialty item for the consumer to implement.
- **XxHash64** is the fastest well-regarded non-cryptographic hash at 64-bit width. It is a deterministic algorithm with a published reference implementation and a stable specification. XxHash32 is omitted from v1 (narrowly additive — a later proposal if a 32-bit fingerprint use case arises).
- **CRC-32C (Castagnoli)** — used by iSCSI, SCTP, NVMe — is additive and deferred; it requires a different polynomial and different test vectors.
- **MurmurHash3, FNV-1a, Adler-32** — all deferred as not covering the primary use cases as well as CRC-32 + XxHash64. MurmurHash3 is unspecified (no standard); Adler-32 is weaker than CRC-32 for file formats.

---

## 3. Determinism guarantee — the sharp contrast with `HashCode`

**CRC-32 and XxHash64 are fully deterministic.** The same byte sequence produces the same digest on every run, every platform, every process. There is no seed, no per-process randomization, no platform-specific behavior. This is the defining property of a wire/file checksum.

This is the **opposite** of `HashCode` (proposal 0008 §5.1), which uses a per-process randomized seed as a hash-flood defense. The two tools serve different contracts:

- `HashCode`: randomized seed, not stable across runs, **never persist or send over a wire**.
- Checksums here: fixed algorithm, stable across all contexts, **specifically intended for persistence and wire protocols**.

The distinction is stated loudly because the confusion — using an in-process hash as a stable digest — is a recurring security and correctness bug. The module-level documentation repeats it.

**XxHash64 seed parameter.** The XxHash64 specification supports an optional seed. This proposal exposes seed=0 only (the default, and the value all standard test vectors assume). A seeded constructor is a potential v2 addition; shipping seed=0 covers all interoperability use cases and leaves no ambiguity about what "the standard test vector" means.

---

## 4. Surface design — two shapes per algorithm

Each algorithm exposes two shapes: a **static one-shot method** for the common case, and an **incremental accumulator record** for streaming or segmented data.

### 4.1 Static one-shot

The one-shot form follows proposal 0009's input/output conventions exactly:

- **Input**: `IReadOnlySpan<byte>` (stdlib §4.16) — accepts `ReadOnlySpan<byte>`, `ReadOnlyArraySpan<byte>`, and any other `IReadOnlySpan<byte>` implementor with one overload.
- **Output**: `uint` (CRC-32) or `ulong` (XxHash64) — plain value types, no ownership, no allocation.

```bezel
public static class Crc32
  // Compute the CRC-32 (IEEE polynomial 0xEDB88320) of `bytes`.
  // Always deterministic: same bytes → same result, every run, every platform.
  public static method Compute(bytes: IReadOnlySpan<byte>): uint;
end Crc32;

public static class XxHash64
  // Compute the XxHash64 (seed=0) of `bytes`.
  // Always deterministic: same bytes → same result, every run, every platform.
  public static method Compute(bytes: IReadOnlySpan<byte>): ulong;
end XxHash64;
```

### 4.2 Incremental accumulator — the chaining-record shape

The incremental form follows the `HashCode` immutable-chaining value record shape from proposal 0008 §5.3 exactly. This is deliberate: the structural analogy helps readers recognize the pattern, and the shape (a small fixed state carried in the value, copy-free, no heap allocation) applies here for the same reasons it applied to `HashCode`. The state size is **not** always one word: `Crc32Hasher` is a single `uint`, but `XxHash64Hasher` carries **four `u64` accumulators plus an up-to-31-byte tail buffer** (the reference algorithm's block remainder) — still a fixed-size value expressible as a record with an inline-array field (`[32]byte` tail + a length), so it remains a stack value with no allocation; it is simply not one word.

Key shape decisions (all identical to `HashCode`'s reasoning in 0008 §5.4):

- **Immutable-chaining value record**: `Add(bytes)` returns a **new** record value; it does not mutate the receiver. This avoids the value-receiver-mutation question (spec §9.4 — see 0008 §5.4), is allocation-free (records are stack values), and is sendable (§30.14 — a record of value-typed fields is a sendable value type).
- **`Start()`** factory: the blessed entry point. `default(Crc32Hasher)` is the all-zero unseeded state and must **not** be used directly — document as a footgun, same note as 0008.
- **`Value` property**: idempotent finalizer (re-readable; no "consume" semantics).
- **No `AddRange(IEnumerable<IReadOnlySpan<byte>>)`**: a sequence-of-spans convenience is *inexpressible*. `IReadOnlySpan<byte>` is a borrow-bearing record (§9.7), and iterating a sequence of them requires the `IEnumerable<T>` enumerator path that spec §8.6 explicitly forbids for spans ("the contrasting *forbidden* case is the `IEnumerable<T>` enumerator path … which is why spans do not implement `IEnumerable<T?>`"). No caller could even construct such a sequence. Streaming is covered by per-chunk `h := h.Add(span)` in an ordinary read-into-buffer loop (§4.2 examples).

```bezel
/// Incremental CRC-32 accumulator. Immutable-chaining value record.
/// Entry point: Crc32Hasher.Start(). Do NOT use default(Crc32Hasher) — it is the
/// all-zero state, not the correct initial CRC state (0xFFFFFFFF). Use Start() only.
public record Crc32Hasher
  // Fresh accumulator at the correct initial state (0xFFFFFFFF internally).
  public static method Start(): Crc32Hasher;

  // Fold bytes into the running CRC; returns a NEW Crc32Hasher value.
  // Equivalent to Crc32Hasher with all prior bytes followed by these bytes.
  // Stream by calling Add once per buffer in a read loop (no span-sequence AddRange — §4.2).
  public method Add(bytes: IReadOnlySpan<byte>): Crc32Hasher;

  // Finalize: the CRC-32 of all bytes added so far. Idempotent; re-readable.
  // Note: this performs the final XOR with 0xFFFFFFFF per the CRC-32 spec;
  // the stored internal state is the pre-XOR value.
  public readonly property Value: uint;
end Crc32Hasher;

/// Incremental XxHash64 accumulator. Immutable-chaining value record.
/// Entry point: XxHash64Hasher.Start(). Seed is fixed at 0.
public record XxHash64Hasher
  // Fresh accumulator for seed=0.
  public static method Start(): XxHash64Hasher;

  // Fold bytes into the running hash; returns a NEW XxHash64Hasher value.
  // Stream by calling Add once per buffer in a read loop (no span-sequence AddRange — §4.2).
  public method Add(bytes: IReadOnlySpan<byte>): XxHash64Hasher;

  // Finalize: the XxHash64 of all bytes added so far. Idempotent; re-readable.
  public readonly property Value: ulong;
end XxHash64Hasher;
```

**Usage examples:**

```bezel
import System.IO;              // FileStream, FileMode
import System.IO.Hashing;

// One-shot checksum of an in-memory buffer.
var crc := Crc32.Compute(fileBytes.AsReadOnlySpan());      // fileBytes: Array<byte>

// Incremental checksum across buffered reads — the §7.1 canonical read loop.
// Add is called once per filled buffer prefix; there is no span-sequence AddRange.
using var stream := new FileStream(path, FileMode.Open);
var buffer := new Array<byte>(64 * 1024, 0);               // sized buffer (§4.4)
var h := Crc32Hasher.Start();
var n := stream.Read(buffer, 0, buffer.Count);            // 0 at EOF
while n > 0 do
  h := h.Add(buffer.AsReadOnlySpan(0, n));                 // §4.15 view over the filled prefix; value reassignment (§25.3 Counter idiom)
  n := stream.Read(buffer, 0, buffer.Count);
end;
var checksum := h.Value;

// One-shot XxHash64 content fingerprint.
var fp: ulong := XxHash64.Compute(payload.AsReadOnlySpan());

// Incremental XxHash64 over the same read-loop shape.
using var xstream := new FileStream(path, FileMode.Open);
var xbuffer := new Array<byte>(64 * 1024, 0);
var hx := XxHash64Hasher.Start();
var m := xstream.Read(xbuffer, 0, xbuffer.Count);
while m > 0 do
  hx := hx.Add(xbuffer.AsReadOnlySpan(0, m));
  m := xstream.Read(xbuffer, 0, xbuffer.Count);
end;
var digest := hx.Value;
```

---

## 5. Known-answer test vectors

These are the standard reference values for verifying a correct implementation. Any implementation of this proposal must produce these values.

### CRC-32 (IEEE polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF)

| Input (bytes) | Expected `Compute(...)` |
|---|---|
| `[]` (empty, 0 bytes) | `0x00000000` |
| `"123456789"` (9 ASCII bytes: 0x31–0x39) | `0xCBF43926` |

These vectors are from the CRC-32 reference check value defined in ITU-T V.42 and universally reproduced in ZIP, PNG, and GZIP specifications.

### XxHash64 (seed = 0)

| Input (bytes) | Expected `Compute(...)` |
|---|---|
| `[]` (empty, 0 bytes) | `0xEF46DB3751D8E999` |
| `"a"` (1 byte: 0x61) | `0xD24EC4F1A98C6E5B` |
| `"abc"` (3 bytes: 0x61 0x62 0x63) | `0x44BC2CF5AD770999` |

These vectors are from the xxHash reference implementation test suite (Cyan4973/xxHash on GitHub; the algorithm version is xxHash64 as specified by the reference).

---

## 6. Module placement and import

These types live in a new `System.IO.Hashing` module — matching the .NET `System.IO.Hashing` namespace, which is the established home for non-cryptographic hashing. They are **not** auto-imported: callers must `import System.IO.Hashing`. Reasons:

1. Checksums are not universally used; auto-importing them adds names (`Crc32`, `XxHash64`, `Crc32Hasher`, `XxHash64Hasher`) to every compilation unit's namespace, which is noisy for code that has no use for them.
2. The `System.IO.Hashing` module name signals the semantic category (IO-adjacent data integrity, not general arithmetic) without touching `System` or `System.Text`.
3. The `Math`/`BitConverter`/`HashCode` utilities in `System` are auto-imported because they are general-purpose low-ceremony tools used everywhere; checksum algorithms are specialized and import-gated.

---

## 7. Relationship to proposal 0008 (`HashCode`)

The two are the two halves of a complete hashing story:

| Concern | Tool | Seeded? | Cross-process stable? |
|---------|------|---------|----------------------|
| In-process hash tables (`GetHashCode`) | `HashCode` (0008) | Yes — per-process randomized | No — must not persist/send |
| Wire/file checksums | `Crc32` / `XxHash64` (this) | No | Yes — the whole point |

Both share the immutable-chaining value record accumulator shape (`Start()` / `Add(bytes): Self` / `Value`). This parallel makes the pattern recognizable even as the semantic contracts diverge sharply.

---

## 8. Declined alternatives

1. **Mutable class accumulator** — the same shape the std's `StringBuilder` uses. Declined for the same reasons `HashCode` used the immutable-chaining record: allocation-free, sendable, no `Dispose`, and avoids the §9.4 value-receiver-mutation question. A one-word state is exactly the case where a chaining-record is preferable.
2. **Auto-imported (`System` module)** — declined; see §6.
3. **XxHash32 in v1** — declined as additive, no P0 consumer identified; deferred.
4. **CRC-32C (Castagnoli)** — declined for v1 (different polynomial, different use cases than ZIP/PNG/Ethernet); deferred.
5. **Exposing XxHash64 seed** — declined for v1; seed=0 is all standard interoperability requires; a seeded constructor is additive and trivially composable later.
6. **Naming `Crc32Hasher` as `Crc32Builder` or `Crc32State`** — declined; the "Hasher" suffix is the established term for an incremental hash accumulator (Rust's `Hasher`, Java's `Checksum` is the exception). "Hasher" reads naturally: `Crc32Hasher.Start().Add(...).Value`.
7. **Placing algorithms in `System.Security.Cryptography`** — declined; these are non-cryptographic algorithms and that placement would wrongly imply security properties. `System.IO.Hashing` is the established module for non-crypto hashing.

---

## 9. What v1 deliberately leaves out

- XxHash32 (additive, no P0 consumer — deferred).
- CRC-32C / Castagnoli (different polynomial, deferred).
- Any HMAC or cryptographic hash (SHA-256, SHA-3, BLAKE3) — belong in a future `System.Security.Cryptography` proposal.
- Streaming encode/decode forms operating on `Stream` (deferred; rides gap review #1 concrete stream family — proposal 0006).
- A seeded XxHash64 constructor (additive, deferred).

---

## 10. Proposed stdlib additions (ready-to-land text)

The following is written to be inserted into `Bezel_Standard_Library.md` as a new section. Section number is assigned at integration time.

```
### N.1  System.IO.Hashing — CRC-32 and XxHash64

Non-cryptographic checksum and content-fingerprint algorithms. All algorithms are
fully deterministic: the same byte sequence always produces the same output, on every
run and every platform. This is the defining contrast with `HashCode` (§9 of this
document), which uses a per-process randomized seed and must never be persisted or
sent across a process boundary.

Requires `import System.IO.Hashing`.

---

#### N.1.1 `Crc32`

CRC-32 using the IEEE polynomial (0xEDB88320), initial value 0xFFFFFFFF, final XOR
0xFFFFFFFF. This is the CRC-32 variant used by ZIP, PNG, GZIP, and Ethernet FCS.

    public static class Crc32
      // Compute the CRC-32 of `bytes`.  Never faults on any input.
      public static method Compute(bytes: IReadOnlySpan<byte>): uint;
    end Crc32;

Known-answer test vectors:
  Crc32.Compute([]) = 0x00000000
  Crc32.Compute(Encoding.ASCII.GetBytes("123456789")) = 0xCBF43926

---

#### N.1.2 `Crc32Hasher`

Incremental CRC-32 accumulator. An immutable-chaining value record: `Add` returns a
new `Crc32Hasher` value rather than mutating the receiver, so no heap allocation
occurs and the value is sendable (§30.14 of the language spec — a record of value-typed
fields is a sendable value type). Stream by calling `Add` once per buffer in a read
loop; there is no `AddRange(IEnumerable<IReadOnlySpan<byte>>)` — spans cannot be
enumerated through `IEnumerable<T>` (spec §8.6).

Entry point: `Crc32Hasher.Start()`.  Do NOT use `default(Crc32Hasher)` — it is the
all-zero state, not the CRC-32 initial value; use `Start()` exclusively.

    public record Crc32Hasher
      public static method Start(): Crc32Hasher;
      public method Add(bytes: IReadOnlySpan<byte>): Crc32Hasher;
      public readonly property Value: uint;   // idempotent finalizer; re-readable
    end Crc32Hasher;

`Value` finalizes (applies the CRC-32 final XOR) on each read; the stored state
is the pre-XOR running value.

---

#### N.1.3 `XxHash64`

XxHash64 with seed=0.  A fast, non-cryptographic hash suitable for content
fingerprinting and change detection.  Algorithm is the xxHash64 standard as
described in the Cyan4973/xxHash reference specification.

    public static class XxHash64
      // Compute the XxHash64 (seed=0) of `bytes`.  Never faults on any input.
      public static method Compute(bytes: IReadOnlySpan<byte>): ulong;
    end XxHash64;

Known-answer test vectors (seed=0):
  XxHash64.Compute([]) = 0xEF46DB3751D8E999
  XxHash64.Compute([0x61]) = 0xD24EC4F1A98C6E5B           // "a"
  XxHash64.Compute([0x61, 0x62, 0x63]) = 0x44BC2CF5AD770999  // "abc"

---

#### N.1.4 `XxHash64Hasher`

Incremental XxHash64 accumulator.  Same immutable-chaining value record shape as
`Crc32Hasher`.  Seed is fixed at 0.

    public record XxHash64Hasher
      public static method Start(): XxHash64Hasher;
      public method Add(bytes: IReadOnlySpan<byte>): XxHash64Hasher;
      public readonly property Value: ulong;  // idempotent finalizer; re-readable
    end XxHash64Hasher;
```
