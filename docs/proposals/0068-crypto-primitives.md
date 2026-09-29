# Proposal 0068 — cryptographic primitives: SHA-2 digests, HMAC, constant-time comparison, secure random bytes

| | |
|---|---|
| **Status** | under-review — draft |
| **Tracking issue** | — (none yet) |
| **Target module(s)** | `System.Security.Cryptography` (new module; requires `import System.Security.Cryptography`) — the [stdlib §11](../Bezel_Standard_Library.md#11-modules-deferred-to-future-revisions) deferred line made concrete |
| **Written against** | spec/stdlib **v0.227** |
| **Currency note** | Refreshed 2026-09-17 to v0.306: one use-site respell in §9's worked example (*Usage examples*) — `out owned var presented` → `out var presented` (the `out owned var` use-site spelling was retired at v0.289; the inline declaration adopts the callee's announced classification, spec §28.3 / Appendix A.4). Declaration-position `out owned` slots are unchanged. Stream windowing (`Read(buffer, offset, count)`) re-verified against stdlib §7.1's recorded `(offset, count)` exception to the v0.282 window convention. No tier-2 flag. Written-against pin unchanged. |
| **Origin** | Adversarial stdlib review pass, 2026-07 (confirmed gap). **Not** among the 31 ranked gaps of the [2026-07 gap review](stdlib-gap-review-2026-07.md) — this proposal extends that map; the review touches the domain only through its decline register (#2: *BigInteger rides the crypto milestone*) and the §11 placeholder line |
| **Design influences** | .NET `System.Security.Cryptography` (`SHA256.HashData`, `HMACSHA256`, `CryptographicOperations.FixedTimeEquals`, `RandomNumberGenerator`) for names and shapes; proposals 0008/0029 for the house accumulator pattern; proposal 0009 for the byte-in/byte-out conventions; proposal 0007 for the OpenSSL substrate posture |

Every type and method this proposal builds on is cited to `Bezel_Standard_Library.md` or the language spec; everything new is **proposed** and appears in §5. Surface that exists only in another *proposal* (0006 streams, 0009 encodings) is cited as proposal surface, never as current stdlib.

---

## 1. Problem and motivation

The stdlib has **no cryptographic surface at all**. The evidence, per the confirmed review:

- Stdlib [§11](../Bezel_Standard_Library.md#11-modules-deferred-to-future-revisions) carries only the shapeless deferred-module line: "`System.Security.Cryptography` — cryptographic primitives." No shape, no scope, no proposal behind it — until this one.
- Stdlib [§2.7](../Bezel_Standard_Library.md#sec-random) `Random` is explicitly **not** crypto-safe and says so: "For cryptographic randomness, see `System.Security.Cryptography` (a future module)." Proposal 0028's `NextBytes(Span)` extension inherits §2.7's contract and does not change this.
- Proposal [0007](0007-system-net-substrate.md) §6 embeds TLS via the owner-ruled OpenSSL sans-IO seam (`bz_tls_*`), but that seam is **runtime-internal**: no digest, MAC, or entropy surface is exposed to user code.
- Proposal [0029](0029-checksums.md) (CRC-32, XxHash64) positions itself explicitly as **non**-cryptographic: its decline #7 refuses the `System.Security.Cryptography` home precisely because "these are non-cryptographic algorithms and that placement would wrongly imply security properties," and its §9 defers "any HMAC or cryptographic hash (SHA-256, SHA-3, BLAKE3) — belong in a future `System.Security.Cryptography` proposal." This is that proposal.

What services and tools cannot do today, with the concrete workloads:

1. **Webhook signature verification** — GitHub (`X-Hub-Signature-256`), Stripe (`Stripe-Signature`), Slack (`X-Slack-Signature`) all sign delivery bodies with **HMAC-SHA-256**; a Bezel HTTP service (proposal 0002) cannot verify any of them. Verification also needs a **constant-time comparison** — comparing a MAC with ordinary `SequenceEqual` leaks the match prefix length through timing, the classic remote-timing break.
2. **Token and session-id generation** — a secure random source plus 0009's `Base64Url` is the standard construction; §2.7's seeded PRNG is guessable and explicitly disclaims this use.
3. **Content addressing and integrity** — cache keys, ETags, artifact digests, lock-file pinning. Proposal [0040](0040-build-system.md) already *assumes* SHA-256 ("content-addressed link-cache (SHA-256 key over source + compiler + options)") — a first-party consumer waiting on this surface.
4. **JWT HS256/HS512** signing and verification (HMAC-SHA-256/512) — the auth-header work 0009 names as its own consumer ends at encoding; the MAC half lives here.

The separation of concerns with the neighbors is sharp and mutual: 0029's checksums are for *accidental* corruption (fast, forgeable); this module is for *adversarial* settings (collision/preimage resistance, unforgeability, unpredictability). 0008's `HashCode` is per-process-seeded and in-process-only. Three tools, three contracts, three modules.

---

## 2. What exists today (the substrate this composes over)

Everything below is current stdlib/spec, cited; the proposal invents none of it.

- **`IReadOnlySpan<byte>`** (stdlib [§4.16](../Bezel_Standard_Library.md#sec-ireadonlyspantelement)) — the unifying read-only byte-source interface, implemented by `ReadOnlySpan<byte>` (§4.14), `ReadOnlyArraySpan<byte>` (§4.15), and the other span families. The 0009-established input convention.
- **`Span<byte>`** (stdlib [§4.14](../Bezel_Standard_Library.md#sec-owned-buffers-and-spans)) — the writable view, admissible as a *synchronous* method parameter (`Encoding.GetBytes(s, destination: Span<byte>)` §3.2, `Stream.Read(buffer: Span<byte>)` §7.1 are the precedents).
- **`Array<byte>`** (stdlib [§4.4](../Bezel_Standard_Library.md#sec-arrayt)) with the sized constructor; trivial-dispose for byte elements (spec [§28.9](../Bezel_Language_Spec.md#sec-trivial-dispose-classes)), so digest results bind with plain `var`.
- **`owned Array<byte>` returns** — the standard fresh-byte-array producer shape: `Encoding.GetBytes` (§3.2), `File.ReadAllBytes` (§7.3), `Stream.ReadAll` (§7.1).
- **Inline arrays `[N]byte`** (spec [§15.1](../Bezel_Language_Spec.md#sec-inline-arrays)) — fixed-size value storage inside records; 0029 §4.2 already uses exactly this for `XxHash64Hasher`'s tail buffer.
- **The Try convention** (stdlib [§1](../Bezel_Standard_Library.md#1-conventions), with the §22.4 `when true` nullability postcondition on every shape-(a) member).
- **Exception taxonomy** (spec [§31.4](../Bezel_Language_Spec.md#sec-standard-exception-types); the "exceptions live with their producer, behind the same import" principle).
- **Intercept-point convention** (stdlib §1, v0.204): "A future addition to this library that reads the world must declare itself an intercept point in its own section at the time it is added." §6 of this proposal complies for the entropy source.
- **Proposal surface consumed in examples, cited as such:** `Convert.ToHexString`/`TryFromHexString` and `Base64Url` (proposal [0009](0009-base64-hex.md), accepted); `FileStream` read loops as in 0029 §4.2 (current stdlib §7.2).

---

## 3. Scope: which algorithms, and the recorded positions

**v1 ships:** SHA-256, SHA-512, HMAC-SHA-256, HMAC-SHA-512, constant-time byte comparison, and cryptographically secure random bytes. Nothing else.

- **SHA-256** — the anchor. Every named workload in §1 uses it; NIST FIPS 180-4; universally interoperable.
- **SHA-512 — shipped in v1.** Position: it is the same Merkle–Damgård structure at 64-bit word width (near-zero marginal design or implementation surface once SHA-256 exists), it is *required* for HMAC-SHA-512 (JWT HS512; several webhook providers), and it is faster than SHA-256 on 64-bit hardware for bulk hashing. Omitting it would force a second proposal for a mechanical twin. SHA-384 and SHA-512/256 are truncation variants — deferred as additive, no consumer named.
- **SHA-1 — deferred, not shipped in v1.** Position, stated for the record: SHA-1's collision resistance is **broken** (practical chosen-prefix collisions since 2017–2020); no in-tree consumer needs it; and placing a broken digest in the *founding* surface of the crypto module reads as endorsement. The genuine interop cases — git object ids, legacy `X-Hub-Signature` HMAC-SHA-1 webhooks — are real but currently hypothetical for this codebase. **Reopening trigger:** a concrete in-tree consumer. When it lands, it lands as ordinary `Sha1`/`HmacSha1` classes whose *documentation* carries the warning ("broken for collision resistance; interop only; HMAC-SHA-1 remains unbroken as a MAC but must not be chosen for new designs") — a documentation stigma, not a naming stigma (`InsecureSha1` was considered and declined: it breaks the family naming rule for a warning the docs state better).
- **MD5** — declined even for interop (strictly weaker than SHA-1 with the same reasoning; the checksum use cases it survives in are 0029's territory).
- **SHA-3 / BLAKE2 / BLAKE3** — deferred; no consumer, additive later, and the module layout here accommodates them without change.
- **AEAD ciphers (AES-GCM, ChaCha20-Poly1305), key derivation (HKDF, PBKDF2, argon2), asymmetric (RSA, ECDSA, Ed25519)** — out of v1 scope, named as the staging for v2+. Note for the record: nothing in v1 needs `BigInteger`, so the gap review's decline #2 ("BigInteger rides the crypto milestone") remains *dormant*, not contradicted — it activates only when the asymmetric wave is designed, exactly as that decline anticipated.

---

## 4. Design decisions

### D1 — Home: one new module, `System.Security.Cryptography`, import-gated

The §11 deferred line names the module; this proposal fills it. **Not** auto-imported: like `System.IO` (§7) and 0029's `System.IO.Hashing`, the names (`Sha256`, `HmacSha256`, `RandomNumberGenerator`, …) are specialized and import-gated; auto-import is reserved for the low-ceremony everywhere-tools of `System`/`System.Text` (0029 §6's reasoning, adopted verbatim). No sub-modules in v1 (`.Hashing`, `.Random` splits declined — eight types do not need a directory tree); revisit only if the v2 cipher/asymmetric waves make the flat module unwieldy.

### D2 — Both shapes per algorithm: one-shot static + chaining accumulator (the house pattern)

Each digest/MAC algorithm exposes **two shapes**, exactly as 0029 ships checksums and for the same reasons:

1. **Static one-shot** (`Sha256.Hash(bytes)`, `HmacSha256.Compute(key, message)`) — the dominant call: whole message in memory, one call, done. Covers every §1 workload directly.
2. **Incremental accumulator** — an **immutable-chaining value record** (`Sha256Hasher.Start().Add(...).Add(...)` then `Digest()`), the 0008 §5.3/§5.4 `HashCode` shape that 0029 §4.2 adopted for `Crc32Hasher`/`XxHash64Hasher`. `Add` returns a **new** record value and never mutates the receiver — no heap allocation, no `Dispose`, no §9.4 value-receiver-mutation question, sendable (§6). Needed for streaming inputs (hash a large file through a read loop) and segmented messages (signature base strings assembled from parts).

The accumulator state is fixed-size and record-expressible, as 0029 §4.2 established for the *non*-one-word case: `Sha256Hasher` carries 8 `uint` chaining words + a `[64]byte` block tail + a `long` length counter (≈ 110 bytes); `Sha512Hasher` carries 8 `ulong` words + `[128]byte` + a 128-bit length (two `ulong`s; ≈ 210 bytes); the HMAC hashers add one key-derived pad block (`[64]byte` / `[128]byte`). Each `Add` copies the record value — a few hundred bytes per call, called once per *buffer* (not per byte) in the canonical read loop; the copy cost is stated honestly and is negligible against the hash computation itself.

Shape details carried over from 0008/0029 unchanged:

- **`Start()` is the blessed entry point.** `default(Sha256Hasher)` (spec §17.11) is the all-zero state, **not** the FIPS 180-4 initial vector — the same documented footgun as `Crc32Hasher`/`HashCode`; `Start()` (and `Start(key)` for HMAC) is the only supported way in.
- **No `AddRange(IEnumerable<IReadOnlySpan<byte>>)`** — inexpressible, per 0029 §4.2's finding: spans do not implement `IEnumerable<T>` (spec §8.6); streaming is per-chunk `h := h.Add(span)` in a read loop.
- **The finalizer is a *method*, `Digest(): owned Array<byte>` — a forced, recorded deviation from 0029's `Value` property.** 0029's checksums finalize to `uint`/`ulong` — value types, expressible as a `readonly property`. A 32/64-byte digest must come back as a fresh `owned Array<byte>` (D3), and the grammar has **no `owned` slot on a property's type** (Appendix A.4: `PropertyDecl ::= … "property" Identifier ":" Type …`, versus `MethodDecl`'s `":" "owned"? Type`) — ownership-announcing returns are method surface. `Digest()` is a pure function of the accumulated value: it does **not** consume the hasher (an immutable record has nothing to consume), may be called repeatedly (each call allocates a fresh array), and a subsequent `Add` continues the same byte stream — the same idempotent-finalizer semantics as 0029's `Value`, in method spelling.

**Declined: one shape only.** One-shot-only forces whole-file buffering for large inputs (the exact failure 0006 §1 records for the stream family); accumulator-only makes the 90% call (`Sha256.Hash(x)`) three lines. Both, or the surface fights its consumers. Also **declined: a `HashAlgorithm` abstract-class hierarchy** (.NET's) — virtual dispatch and class-allocation ceremony for what records do for free, and record types cannot participate in class inheritance anyway; the concrete-per-algorithm surface is the 0029 house answer. Callers generic over "some digest" are a v2 static-interface question (0008 §3.5's deferral pattern), not a v1 class hierarchy.

### D3 — Bytes in, bytes out: `IReadOnlySpan<byte>` → `owned Array<byte>` (the 0009 conventions)

- **Input**: `IReadOnlySpan<byte>` (stdlib §4.16) — one overload accepts every byte source (`Array<byte>` via `.AsReadOnlySpan()` §4.15, `OwningBuffer<byte>` via `.AsReadOnlySpan()` §4.14). Adopted from 0009 D5 verbatim, including the performance posture: hashing cost is dominated by the compression function, not the span dispatch, and implementations (compiler-known, §10) may recover the concrete span type internally per §4.16's pattern-match note.
- **Output**: `owned Array<byte>` — the standard fresh-byte-array return (`Encoding.GetBytes` §3.2, `File.ReadAllBytes` §7.3, 0009's `Decode`). Digest lengths are fixed and documented per method (32 for SHA-256/HMAC-SHA-256; 64 for SHA-512/HMAC-SHA-512). `Array<byte>` is trivial-dispose (spec §28.9), so results bind with plain `var` and flow into 0009's `Convert.ToHexString`/`Base64Url.Encode` with zero ceremony.

**Declined: a fixed-size digest value record** (`Sha256Digest` wrapping `[32]byte`). Tempting — allocation-free, sendable-by-construction — but (a) it would carry the §9.2 auto-equality surface, and a **variable-time `=` on a MAC value is precisely the misuse D5's `FixedTimeEquals` exists to prevent**; suppressing or redefining record equality for one type is its own spec question; (b) every consumer (hex display, Base64, serializers, comparisons) speaks spans/arrays, so the record would immediately convert out. Recorded as revisitable if a zero-allocation digest pipeline materializes.

### D4 — HMAC keying: key as a call-bounded borrow; `Start(key)` copies derived state, never the borrow

One-shot: `HmacSha256.Compute(key: IReadOnlySpan<byte>, message: IReadOnlySpan<byte>): owned Array<byte>`. The key parameter is an ordinary call-bounded span borrow — nothing is retained past the call.

Incremental: `HmacSha256Hasher.Start(key: IReadOnlySpan<byte>): HmacSha256Hasher`. RFC 2104 keying is performed inside `Start`: a key longer than the block size (64/128 bytes) is first hashed; the result is padded and folded into the ipad/opad blocks. The record then holds the *derived* inner-hash state and the opad block **as value fields** — a transformation of the key material, copied into the record, with **no borrow of the caller's span retained**. The hasher is therefore an ordinary non-borrow-bearing record (spec §9.7) despite being constructed from a borrow; this is the load-bearing fact behind its sendability (§6). Any key length is valid, including zero (RFC 2104 imposes no minimum; the *advice* to use ≥ digest-length keys is documentation, not a fault).

**Secret-hygiene note, stated loudly (and carried to open question Q3):** v1 makes **no zeroization promise**. HMAC hasher values contain key-derived secret material; as chaining value records they are copied through locals and stack frames on every `Add`, and Bezel has no mechanism to scrub dead stack copies (nor do the .NET/Go equivalents). A `KeyBuffer`-style zero-on-dispose owned container is deliberately out of v1 — it protects the heap copy only, and shipping it would advertise a guarantee the stack copies void.

### D5 — Constant-time comparison: `CryptographicOperations.FixedTimeEquals`, with a pinned contract

```bezel
public static class CryptographicOperations
  public static method FixedTimeEquals(a: IReadOnlySpan<byte>, b: IReadOnlySpan<byte>): bool;
end CryptographicOperations;
```

The class and method names are inherited verbatim from .NET, which the stdlib's §1 naming rule blesses ("names inherited verbatim from .NET keep the .NET spelling"). The **contract**, pinned normatively:

1. Returns `true` iff `a.Length = b.Length` **and** the contents are byte-wise equal.
2. **Timing:** when the lengths are equal, execution time is a function of the length **only**, never of the contents or of the position of any mismatch — the implementation must examine every byte unconditionally (accumulate differences bitwise; a single fold, no data-dependent branch, no early exit). When the lengths differ, the method returns `false` in time that may depend on the two lengths but not on the contents. Lengths are treated as public: in every MAC/token-verification protocol the expected length is fixed by the algorithm and known to the attacker, so length-dependent time leaks nothing.
3. Never faults, on any input including two empty spans (`true`) — total.

Why lengths-public is the right cut: hiding length timing would require padding to a maximum, which has no defensible universal bound. This matches the .NET contract ("takes an amount of time which depends only on the length") and the audited implementations everywhere.

`FixedTimeEquals` is the **only** sanctioned way to compare MACs, tokens, and password-hash outputs; the module documentation says so and cross-references it from every `Hmac*` type. `SequenceEqual` (§4.16) remains what it is — an ordinary early-exit comparison for non-adversarial data.

### D6 — Secure random: `RandomNumberGenerator`, static, and a declared intercept point

```bezel
public static class RandomNumberGenerator
  public static method GetBytes(count: int): owned Array<byte>;   // count = 0 → empty array
  public static method Fill(destination: Span<byte>);             // fills every slot
end RandomNumberGenerator;
```

- **Name**: inherited verbatim from .NET (`RandomNumberGenerator.GetBytes`/`Fill` are the modern .NET statics) under the §1 naming rule — and maximally distant from §2.7's `Random`, which is the point: the two must never be confused, and §2.7's own text already points here.
- **Shape**: a static class with no instances, no seed, no state to manage — entropy comes from the OS (§10). There is deliberately **no constructor and no seedable form**; a "seeded CSPRNG" is a stream cipher, which is v2 cipher-wave material, and seeding is the §2.7 concept this type exists to be distinct from.
- **`Fill(destination: Span<byte>)`** is the allocation-free form for reused buffers; `Span<byte>` as a synchronous parameter follows `Encoding.GetBytes(s, destination:)` (§3.2). No async forms: reads never block meaningfully post-boot, and spans are barred from async signatures anyway (§7.1's #1438 note).
- **Bounded integers** (`GetInt32(fromInclusive, toExclusive)`, unbiased by rejection sampling) are deferred as additive — the named v1 workloads consume bytes; record so the eventual addition uses rejection sampling, never modulo.
- **Intercept point (required declaration, stdlib §1 v0.204):** `RandomNumberGenerator` **is an intercept point** — it reads the world (OS entropy), joining the §2.6/§2.7 entropy family; a conforming runtime may record and substitute its outputs for deterministic replay (proposal 0045's machinery). The digest, MAC, and comparison surfaces of this module are **pure computation and deliberately not interceptable**.

---

## 5. Proposed API surface (all types new, in `System.Security.Cryptography`)

```bezel
// ── one-shot digests ──
public static class Sha256
  // SHA-256 (FIPS 180-4) of `bytes`.  Returns a fresh 32-byte array.  Never faults.
  public static method Hash(bytes: IReadOnlySpan<byte>): owned Array<byte>;
end Sha256;

public static class Sha512
  // SHA-512 (FIPS 180-4) of `bytes`.  Returns a fresh 64-byte array.  Never faults.
  public static method Hash(bytes: IReadOnlySpan<byte>): owned Array<byte>;
end Sha512;

// ── incremental digest accumulators (immutable-chaining value records, 0008/0029 shape) ──
/// Entry point: Sha256Hasher.Start().  Do NOT use default(Sha256Hasher) — it is the
/// all-zero state, not the FIPS 180-4 initial vector.  Add returns a NEW value;
/// stream by `h := h.Add(chunk)` in a read loop (no span-sequence AddRange — 0029 §4.2).
public record Sha256Hasher
  public static method Start(): Sha256Hasher;
  public method Add(bytes: IReadOnlySpan<byte>): Sha256Hasher;
  // Digest of all bytes added so far: fresh 32-byte array per call.  Does not
  // consume the hasher; a later Add continues the same stream.  (A method, not a
  // `Value` property, because property types carry no `owned` — §4 D2.)
  public method Digest(): owned Array<byte>;
end Sha256Hasher;

public record Sha512Hasher
  public static method Start(): Sha512Hasher;
  public method Add(bytes: IReadOnlySpan<byte>): Sha512Hasher;
  public method Digest(): owned Array<byte>;                    // 64 bytes
end Sha512Hasher;

// ── one-shot HMAC (RFC 2104 / FIPS 198-1) ──
public static class HmacSha256
  // HMAC-SHA-256 of `message` under `key` (any key length; long keys pre-hashed
  // per RFC 2104).  Returns a fresh 32-byte array.  Never faults.
  // Compare results ONLY with CryptographicOperations.FixedTimeEquals.
  public static method Compute(key: IReadOnlySpan<byte>,
                               message: IReadOnlySpan<byte>): owned Array<byte>;
end HmacSha256;

public static class HmacSha512
  public static method Compute(key: IReadOnlySpan<byte>,
                               message: IReadOnlySpan<byte>): owned Array<byte>;  // 64 bytes
end HmacSha512;

// ── incremental HMAC accumulators ──
/// Entry point: HmacSha256Hasher.Start(key).  The key is a call-bounded borrow;
/// Start copies RFC 2104-derived state into the record — no borrow is retained
/// (§4 D4).  default(HmacSha256Hasher) is the same footgun as the digest hashers.
public record HmacSha256Hasher
  public static method Start(key: IReadOnlySpan<byte>): HmacSha256Hasher;
  public method Add(bytes: IReadOnlySpan<byte>): HmacSha256Hasher;
  public method Digest(): owned Array<byte>;                    // 32 bytes
end HmacSha256Hasher;

public record HmacSha512Hasher
  public static method Start(key: IReadOnlySpan<byte>): HmacSha512Hasher;
  public method Add(bytes: IReadOnlySpan<byte>): HmacSha512Hasher;
  public method Digest(): owned Array<byte>;                    // 64 bytes
end HmacSha512Hasher;

// ── constant-time comparison (§4 D5 pins the timing contract) ──
public static class CryptographicOperations
  public static method FixedTimeEquals(a: IReadOnlySpan<byte>, b: IReadOnlySpan<byte>): bool;
end CryptographicOperations;

// ── secure random (declared intercept point, §4 D6) ──
public static class RandomNumberGenerator
  public static method GetBytes(count: int): owned Array<byte>;
      // count < 0: ArgumentOutOfRangeException (§1 negative-magnitudes convention)
  public static method Fill(destination: Span<byte>);
end RandomNumberGenerator;

// ── fault type (§7) ──
public class CryptographicException: Exception
  public constructor(message: string);
end CryptographicException;
```

All accumulator records are compiler-known value records in the manner of 0008's `HashCode` and 0029's hashers (fixed-size state: chaining words + `[64]byte`/`[128]byte` block tails + length counters — spec §15.1 inline arrays as fields, the 0029 §4.2 construction); the listings above show only the public surface, not the storage, so the §9.2 zero-auto-property reading does not apply. No member of this module is marked `pure`: the digest paths read their input through `IReadOnlySpan<byte>`, whose interface members are not declared `pure` (stdlib §4.16), so the §22.15 pure-calls-pure check cannot pass today; revisit if the span interface ever gains `pure` members.

---

## 6. Ownership and sendability

Every decision, stated:

- **Inputs are borrows.** All `bytes`/`key`/`message` parameters are `IReadOnlySpan<byte>` call-bounded borrows (boxed per spec §8.6's borrow-bearing-record rule; the standard §4.16 lifetime constraints — nothing new). `Fill`'s `Span<byte>` is a call-bounded writable borrow. **Nothing in this module retains a caller borrow past the call** — the HMAC `Start(key)` copies derived state (D4).
- **Outputs are owned.** Every digest/MAC/`GetBytes` result is a freshly allocated `owned Array<byte>`, caller's to keep; `Array<byte>` is trivial-dispose (spec §28.9), so no `using var` ceremony arises and discard is legal.
- **The static classes** (`Sha256`, `Sha512`, `HmacSha256`, `HmacSha512`, `CryptographicOperations`, `RandomNumberGenerator`) have no instances — no ownership or disposal story exists. `RandomNumberGenerator`'s OS entropy handle, if the implementation holds one, is runtime-internal state, not user-visible surface.
- **The hasher records are sendable value types** (spec [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)): every field is a value type (integer words, `[N]byte` inline arrays — non-owning, so not move-only §15.3 — and length counters), no borrows are carried (D4), so the records are neither borrow-bearing (§9.7) nor move-only, hence sendable — they may cross `Domain.Spawn` and channels as copies. This is deliberate and useful (fan out chunk hashing, join accumulated states is *not* supported — SHA-2 is sequential — but *moving a partially-fed hasher to the domain that owns the next chunk* is). Note the consequence for HMAC: a keyed hasher crossing a domain carries key-derived secret material with it, as any sendable value would; the secret-hygiene note of D4 applies unchanged.
- **`CryptographicException`** follows the exception-ownership model of spec §31 (caught by borrow, no user disposal).
- **Domain-locality:** nothing in this module is domain-affine; all operations are synchronous pure computation except the entropy reads, which are thread-safe in the OS.

---

## 7. Fault taxonomy

Small by design — the digest/MAC surface is **total**:

| Member | Fault | Condition |
|---|---|---|
| `Sha*.Hash`, `Hmac*.Compute`, all `Add`/`Digest`/`Start` | — | Never fault. Any input length including empty; any key length including empty (D4). |
| `FixedTimeEquals` | — | Never faults; length mismatch is `false`, not a fault (D5). |
| `RandomNumberGenerator.GetBytes(count)` | `ArgumentOutOfRangeException` | `count < 0` — the stdlib §1 negative-magnitudes convention. `count = 0` returns an empty array (valid). |
| `RandomNumberGenerator.GetBytes` / `Fill` | `CryptographicException` | The OS entropy source fails (e.g. `getrandom(2)` unsupported/failing). Effectively unreachable on target platforms; a *defined* fault rather than a silent fallback to weak entropy, per the defined-fault posture (§3.2 Encoding's model). **Never** silently degrades to a PRNG. |

`CryptographicException` derives directly from `Exception` (not `IOException` — entropy failure is not I/O in the §7.5 sense) and lives in this module behind the same import, per spec §31.4's producer-locality principle. No `Try` shapes exist in v1: nothing here parses or extracts — the one fallible operation (`GetBytes`) fails only on a broken platform, which is not an *expected* outcome a caller branches on (Guide 2 §5's line).

---

## 8. Known-answer test vectors

Any implementation must reproduce these (FIPS 180-4 / RFC 4231 standard vectors). ASCII inputs shown as strings.

### SHA-256

| Input | `Sha256.Hash(...)` (hex) |
|---|---|
| `[]` (empty) | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `"abc"` | `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad` |

(The empty-input digest is the string 0009 §3 already displays in its hex example — the two proposals cross-check.)

### SHA-512

| Input | `Sha512.Hash(...)` (hex) |
|---|---|
| `[]` (empty) | `cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e` |
| `"abc"` | `ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f` |

### HMAC-SHA-256 (RFC 4231)

| Key | Message | `HmacSha256.Compute(...)` (hex) |
|---|---|---|
| `0x0b` × 20 | `"Hi There"` | `b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7` |
| `"Jefe"` | `"what do ya want for nothing?"` | `5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843` |

### HMAC-SHA-512 (RFC 4231, test case 1)

| Key | Message | `HmacSha512.Compute(...)` (hex) |
|---|---|---|
| `0x0b` × 20 | `"Hi There"` | `87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854` |

Additionally: the chaining accumulators must produce byte-identical output to the one-shots for every split of the input (`Start().Add(a).Add(b).Digest() = Hash(a ++ b)` for all partitions — the property-test the 0053 machinery generates naturally), and `RandomNumberGenerator` is tested for contract (length, no fault, and *not* for randomness — statistical tests are not correctness tests).

---

## 9. Usage examples

```bezel
import System.IO;                       // FileStream, FileMode (§7)
import System.Security.Cryptography;

// 1. Webhook signature verification (HMAC + hex + constant-time compare).
//    Convert.TryFromHexString is proposal-0009 surface (accepted), cited as such.
public method VerifySignature(secret: string, body: Array<byte>, signatureHex: string): bool
  var key := Encoding.UTF8.GetBytes(secret);                   // §3.2; owned, trivial-dispose
  var expected := HmacSha256.Compute(key.AsReadOnlySpan(), body.AsReadOnlySpan());
  if not Convert.TryFromHexString(signatureHex, out var presented) then
    return false;                                              // malformed hex ≠ a fault here
  end;
  // `presented` is narrowed non-null past the check (§22.4 when-true postcondition).
  return CryptographicOperations.FixedTimeEquals(
      expected.AsReadOnlySpan(), presented.AsReadOnlySpan());
end VerifySignature;

// 2. Token generation (secure random + proposal-0009 Base64Url).
var token := RandomNumberGenerator.GetBytes(32);
var sessionId := Base64Url.Encode(token.AsReadOnlySpan());

// 3. Content addressing: hash a large file through the canonical read loop
//    (the 0029 §4.2 loop shape, verbatim, with a Sha256Hasher).
using var stream := new FileStream(path, FileMode.Open);
var buffer := new Array<byte>(64 * 1024, 0);                   // §4.4 sized ctor
var h := Sha256Hasher.Start();
var n := stream.Read(buffer, 0, buffer.Count);                 // 0 at EOF (§7.1)
while n > 0 do
  h := h.Add(buffer.AsReadOnlySpan(0, n));                     // §4.15 view; value reassignment
  n := stream.Read(buffer, 0, buffer.Count);
end;
var cacheKey := Convert.ToHexString(h.Digest().AsReadOnlySpan());   // 0009 (accepted)

// 4. One-shot digest of an in-memory payload.
var digest := Sha256.Hash(payload.AsReadOnlySpan());           // payload: Array<byte>
```

---

## 10. Implementation substrate — the OpenSSL question (owner-facing)

The observable contract above is substrate-independent (deterministic, KAT-tested). The substrate choice is nonetheless **load-bearing** and is put to the owner (open question Q1):

**Recommended: ride the 0007 owner ruling — OpenSSL 3.x `libcrypto`, behind a narrow runtime-internal `bz_crypto_*` seam** parallel to 0007 §6's `bz_tls_*` (roughly: `bz_crypto_digest(alg, bytes) `, `bz_crypto_digest_{new,add,final,free}`, `bz_crypto_hmac(...)` + incremental trio, `bz_crypto_memcmp_ct`). Reasons: the *backend choice* is already owner-ruled for TLS (2026-07-04) under the CLAUDE.md system-library posture — though the linkage **reach** is new (every hashing program links `libcrypto`, where 0007 reached only TLS users) and is exactly the dependency decision Q1 puts to the owner; EVP digests/HMAC are the most-audited implementations available, including their constant-time disciplines; and hand-rolled crypto is an audit liability the project has no reason to take on. `FixedTimeEquals` maps to `CRYPTO_memcmp` (or an equivalent runtime-internal fold — trivially auditable either way).

**Entropy is deliberately decoupled:** `RandomNumberGenerator` reads `getrandom(2)` directly (not `RAND_bytes`), so the random surface has **no** libcrypto dependency regardless of Q1's answer — the OS CSPRNG is the ground truth either way, and this keeps the one security-critical *unpredictability* source independent of the substrate debate.

**The load-bearing linkage question (Q1):** under this recommendation, any program that hashes — including a CLI tool that never touches `System.Net` — links `libcrypto`. 0007's ruling was motivated by TLS; extending its reach to all crypto consumers is a *new* consequence the owner should rule on explicitly. The alternative is (b): a self-contained runtime implementation of SHA-2/HMAC (a few hundred lines of C++, KAT-gated) with **no** external linkage — zero dependency cost, nonzero audit cost, and a fork in behavior-provenance between this module and 0007's TLS. Both are compatible with every contract in this proposal; the proposal recommends (a) with (b) recorded as the fallback if the linkage is ruled unacceptable. Per CLAUDE.md, dependency adoption is a project-level decision — nothing here is implemented autonomously.

---

## 11. What v1 deliberately leaves out

- SHA-1 / HMAC-SHA-1 (deferred with a named reopening trigger — §3), MD5 (declined), SHA-384/SHA-512-truncations, SHA-3/BLAKE2/BLAKE3 (deferred, additive).
- AEAD ciphers, key derivation (HKDF/PBKDF2/argon2), asymmetric signatures/key exchange — the v2+ waves; `BigInteger` (gap-review decline #2) activates only there.
- `RandomNumberGenerator.GetInt32` bounded integers (additive; must use rejection sampling — recorded).
- Digest-over-`Stream` conveniences (`Sha256.HashAsync(stream)`) — rides the 0006 stream family; the §9 read loop covers it meanwhile (0029 §9 defers identically).
- A zero-on-dispose key container (`KeyBuffer`) — see D4's hygiene note and Q3.
- Hex/Base64 convenience wrappers (`HashToHexString`) — composition with 0009 is one call; declined per the smallest-surface discipline.

---

## 12. Declined alternatives (register)

1. **A `HashAlgorithm` abstract-class hierarchy** (.NET shape) — dispatch and allocation ceremony with no consumer; concrete statics + records are the 0029 house shape; generic-over-digest is a future static-interface question (0008 §3.5 pattern). (§4 D2.)
2. **Mutable class accumulators** — same decline as 0029 §8.1/0008 §7.7: the immutable-chaining record is allocation-free, sendable, `Dispose`-free, and avoids the §9.4 receiver-mutation gap.
3. **A fixed-size digest value record** — auto-equality would hand out a variable-time `=` on MAC values; declined for `owned Array<byte>` composition. (§4 D3.)
4. **Making §2.7 `Random` crypto-safe / seeding it from this module** — contract confusion between a seeded, replayable PRNG and an OS CSPRNG; §2.7's own text routes crypto here. Two types, two contracts.
5. **String-keyed algorithm agility** (`Hash.Compute("SHA-256", …)`, EVP-style lookup) — stringly-typed crypto selection is a classic downgrade/misuse vector and defeats compile-time surface review; the algorithm set is closed and nominal.
6. **Auto-importing the module** — specialized names; import-gated like §7 and 0029 §6.
7. **`insecure`-prefixed naming for legacy algorithms** — documentation carries warnings better than identifiers do; keeps the family rule uniform. (§3, SHA-1 position.)
8. **`Try` shapes anywhere** — no expected-failure extraction exists in this surface (§7).
9. **`pure` markers on the digest surface** — blocked today by §4.16's unmarked interface members under §22.15's pure-calls-pure; revisit with the span interface. (§5.)
10. **Placing checksum-grade algorithms here or crypto in `System.IO.Hashing`** — both directions were already declined in 0029 (its decline #7); the module boundary is security claim vs. none.

---

## 13. Open questions for the owner

1. **Substrate linkage (load-bearing).** Confirm riding 0007's OpenSSL 3.x ruling via a runtime-internal `bz_crypto_*` seam, **accepting that every hashing program links `libcrypto`** — or direct the self-contained runtime implementation (no linkage, more audit surface). §10 states both; the recommendation is OpenSSL. Entropy stays `getrandom(2)` either way.
2. **SHA-1 deferral.** Confirm the §3 position (defer with the named reopening trigger) — or direct v1 inclusion for interop with documentation warnings. The surface cost is mechanical either way; the question is what the founding module *says*.
3. **Secret hygiene.** v1 ships with no zeroization promise (D4). Is a `KeyBuffer` zero-on-dispose container wanted as a co-requisite (honest only about heap copies), or does the recorded limitation stand until a real consumer demands more?
4. **`Digest()` naming.** The finalizer is a method by grammatical necessity (§4 D2); confirm the name `Digest` (Java `MessageDigest.digest` lineage) over `GetDigest`/`ToArray`-style spellings — a pure naming call, flagged only because the type will be long-lived.
5. **The gap-review decline #2 trigger (BigInteger).** Decline #2's recorded trigger is "ride the crypto milestone"; §3 keeps it dormant by re-anchoring the trigger to the v2 asymmetric wave (v1's symmetric surface needs no big-integer arithmetic). Confirm the re-anchoring, or direct that `BigInteger` be designed alongside this module as the decline's letter suggests.

---

## 14. Relationship to existing work

- **Stdlib §11** — the deferred `System.Security.Cryptography` line this proposal makes concrete; **§2.7** — the explicit non-crypto `Random` whose text forward-references this module; **§1** — Try convention, negative-magnitudes, naming, and the v0.204 intercept-point convention (§4 D6 complies).
- **Proposal 0007** (accepted) — the OpenSSL 3.x owner ruling and the sans-IO seam discipline this proposal's recommended substrate parallels (§10); 0007 exposes no user-facing crypto, which is half of this proposal's evidence.
- **Proposals 0008/0029** (accepted/draft) — the immutable-chaining accumulator house pattern adopted here; 0029 additionally fixes the module boundary (non-crypto hashing ≠ crypto) from the other side, and its §9 explicitly defers SHA-256/HMAC to this proposal.
- **Proposal 0009** (accepted) — the `IReadOnlySpan<byte>` in / `owned Array<byte>` out conventions adopted verbatim (D3), and the hex/Base64 encodings every §9 example composes with.
- **Proposal 0028** (accepted) — `Random.NextBytes(Span)` stays firmly on the non-crypto side; no interaction beyond the contract wall.
- **Proposal 0040** (draft sketch) — the build system's content-addressed cache already names SHA-256; a first-party consumer.
- **Proposals 0001/0002** (draft/accepted) — webhook verification and JWT workloads arrive with the HTTP server; this module is their prerequisite.
- **[Gap review 2026-07](stdlib-gap-review-2026-07.md)** — cited as the map this proposal extends: crypto is absent from its 31 ranked gaps; its decline #2 (BigInteger) records the only crypto-adjacent decision, which §3 keeps dormant.
- **[DECLINED.md](DECLINED.md)** — no conflict: D-0001/2/3/4 and the eight gap-review declines touch neither digests, MACs, comparison, nor entropy.
