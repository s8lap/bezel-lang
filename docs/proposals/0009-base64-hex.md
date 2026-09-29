# Proposal 0009 — Base64 + hex encodings

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Target modules** | `System.Text` (§3, auto-imported) · `System` / `Convert` (§2.3, auto-imported) |
| **Written against** | spec v0.177 / stdlib v0.165 |
| **Tracking issue** | [#1434](https://github.com/s8lap/bezel-lang/issues/1434) |
| **Gap-review item** | #3 (P0+P1) in [stdlib-gap-review-2026-07.md](stdlib-gap-review-2026-07.md) |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); refreshed 2026-09-17 to v0.306: the §3 usage example's `out owned var bytes` respelled `out var bytes` (v0.289 retired the `out owned var` recipient; §22.4's adoption clause makes the plain `out var` local adopt the callee's announced `out owned` classification, so the example's meaning is unchanged). Declaration-side `out owned result: Array<byte>?` and the v0.244-kept `using var _ := bytes!;` are unchanged. One tier-2 flag recorded in `proposals-currency-2026-09-17.md` §3 (the `when true` out-postcondition). Written-against pin unchanged. |

Blocks web auth headers, JWTs, binary-in-JSON, data: URIs, and hash-digest display; entirely absent today. This proposal is also the prerequisite for proposal 0010 (serialization `ISerializer.WriteBytes`), which will use standard-alphabet Base64 as its text-wire encoding.

---

## 1. Design decisions

### D1 — Home

**Decision.** `Base64` and `Base64Url` as dedicated static classes in `System.Text` (new §3.5 and §3.6). Hex encoding/decoding as `ToHexString`, `FromHexString`, and `TryFromHexString` added to `Convert` (§2.3).

**Rationale.** `System.Text` already houses `Encoding` (§3.2), the closest relative — both are binary↔string converters with a defined-fault error model. Auto-import means callers write `Base64.Encode(...)` without any `import` statement, matching the ergonomics of `Encoding.UTF8.GetBytes`. For hex, `Convert` (§2.3) is the natural home: it already provides `ToString(value: int, toBase: int)` for integer-to-hex formatting and `ToInt32`/`TryToInt32` for parse-with-exception / try-parse; hex byte-array encoding is the same class of type conversion.

### D2 — Surface: Encode/Decode pairing

**Decision.** Each class provides three methods: `Encode` (always succeeds), `Decode` (throws `FormatException` on bad input), and `TryDecode` following the v0.171 Try-convention shape (a): `bool` + nullable `out owned` parameter. Same three-method pairing for hex on `Convert`.

**Rationale.** Mirrors the `Parse`/`TryParse` pairing standardized by `IParsable<T>` (§2.19) and followed by `DateTime.Parse`/`TryParse` (§2.5), `int.Parse`/`TryParse` (§9), and `Guid.Parse`/`TryParse` (§2.6). Callers that can tolerate failures use `TryDecode`; callers that treat decode failure as a bug use `Decode` and let the exception propagate.

### D3 — Alphabets and padding

**Decision.** `Base64` class: standard RFC 4648 §4 alphabet (`A–Z a–z 0–9 + /`), `=` padding characters appended on encode, required on decode. `Base64Url` class: URL-safe RFC 4648 §5 alphabet (`A–Z a–z 0–9 - _`), no padding characters on encode or decode.

Decode is strict: any character outside the class's alphabet, or invalid padding, raises `FormatException`. The alternative — tolerating whitespace or unrecognized characters with silent skip — was considered and declined, following the same reasoning as `Encoding.GetString` (§3.2): "silent tolerance hides data problems; a caller that means to accept padded input trims explicitly" (§9 `double.Parse` note, same policy).

### D4 — Hex casing

**Decision.** `Convert.ToHexString` produces **lowercase** hex digits (`a–f`). `FromHexString` / `TryFromHexString` accept both cases (`a–f` and `A–F`) on input.

**Rationale.** Lowercase hex is the dominant convention in HTTP headers, JWT `kid` fingerprints, CSS color codes, and hash-digest display. Callers needing uppercase output can call `.ToUpper()` on the result; the `$"{value:X}"` IFormattable specifier (§2.18) already covers uppercase integers. Mixed-case tolerance on decode is universal convention and costs nothing.

### D5 — Input and output types

**Decision.** Encode methods accept `IReadOnlySpan<byte>` (§4.16). Decode methods return `owned Array<byte>`.

**Rationale for input.** `IReadOnlySpan<byte>` is the common read-only byte-source interface (§4.16), implemented by `ReadOnlySpan<byte>` (from `OwningBuffer<byte>.AsReadOnlySpan()`) and `ReadOnlyArraySpan<byte>` (from `Array<byte>.AsReadOnlySpan()`). A single overload therefore accepts all common byte sources without duplication. Encoding is not a hot inner loop (one string allocation per call; the per-byte cost is a table lookup); §4.16 recommends `IReadOnlySpan<T>` for "utility code that runs outside hot paths" exactly because the virtual dispatch overhead is negligible there.

**Rationale for output.** `owned Array<byte>` is the standard fresh-byte-array return type in the stdlib: `Encoding.GetBytes` (§3.2), `File.ReadAllBytes` (§7.3), and `Stream.ReadAll` (§7.1) all return `owned Array<byte>`. Callers receiving decoded bytes wrap them into `OwningBuffer<byte>` if they need buffer semantics, or iterate directly.

**Streaming.** Deferred to v2. A streaming form (encode/decode incrementally into a `Span<byte>` destination) would benefit serialization pipelines that want zero-copy encoding, but adds surface and complicates error handling. The whole-buffer methods cover all v1 use cases.

### D6 — Proposal 0010 dependency

Proposal 0010 (`ISerializer.WriteBytes` token) will use standard-alphabet Base64 as the text-wire encoding for byte arrays in JSON serialization. This proposal is its stated prerequisite (gap-review item #17 depends on #3).

---

## 2. Proposed stdlib additions (ready-to-land text)

The following sections are written to be inserted into `Bezel_Standard_Library.md` via the normal docs workflow. Section numbers assume no intervening insertions; the docs editor adjusts numbering at integration time.

### Proposed §3.5 — `Base64`

A static utility class for RFC 4648 §4 standard-alphabet Base64 encoding and decoding. Auto-imported as part of `System.Text`.

```bezel
public static class Base64
  // Encode all bytes in `bytes` using the standard Base64 alphabet
  // (A–Z a–z 0–9 + /) and append `=` padding to the nearest multiple of 4.
  // Returns a fresh string; never throws.
  public static method Encode(bytes: IReadOnlySpan<byte>): string;

  // Decode a standard Base64 string.  Raises FormatException if `s` contains
  // any character outside the standard alphabet or has invalid padding.
  // Returns a freshly-allocated Array<byte> owned by the caller.
  public static method Decode(s: string): owned Array<byte>;

  // Try-decode form.  Returns true and assigns `result` on success; returns
  // false and assigns `result := null` on failure (per the v0.171 Try
  // convention, §1).
  public static method TryDecode(s: string, out owned result: Array<byte>?): bool;
end Base64;
```

**Encode.** Accepts any `IReadOnlySpan<byte>` source (§4.16): callers with an `Array<byte>` call `.AsReadOnlySpan()` to get a `ReadOnlyArraySpan<byte>`; callers with an `OwningBuffer<byte>` call `.AsReadOnlySpan()` to get a `ReadOnlySpan<byte>`; both implement `IReadOnlySpan<byte>` and are accepted. The returned string contains only the 65 characters `A–Z`, `a–z`, `0–9`, `+`, `/`, and `=`.

**Decode.** Raises `FormatException` on any character outside the standard alphabet or on malformed padding (per the §3.2 / §2.18 strict-fault model: silent tolerance hides data problems). Leading/trailing whitespace is not accepted; callers that need to trim call `s.Trim()` first.

**TryDecode.** Assigns the decoded bytes to `result` on success; assigns `null` on failure. Per the v0.171 Try convention (§1, shape a), the out parameter is always assigned on both exit paths.

### Proposed §3.6 — `Base64Url`

A static utility class for RFC 4648 §5 URL-safe Base64 encoding and decoding. Auto-imported as part of `System.Text`.

```bezel
public static class Base64Url
  // Encode all bytes in `bytes` using the URL-safe Base64 alphabet
  // (A–Z a–z 0–9 - _).  No padding characters are appended.
  // Returns a fresh string; never throws.
  public static method Encode(bytes: IReadOnlySpan<byte>): string;

  // Decode a URL-safe Base64 string (no padding required; padding is
  // accepted but not required).  Raises FormatException if `s` contains
  // any character outside the URL-safe alphabet.
  // Returns a freshly-allocated Array<byte> owned by the caller.
  public static method Decode(s: string): owned Array<byte>;

  // Try-decode form.  Returns true and assigns `result` on success; returns
  // false and assigns `result := null` on failure.
  public static method TryDecode(s: string, out owned result: Array<byte>?): bool;
end Base64Url;
```

**Alphabet.** Uses `-` and `_` in place of `+` and `/`. No `=` padding is ever emitted; on decode, `=` padding characters are accepted but not required (matching the predominant JWT / OAuth convention).

### Proposed addition to §2.3 — `Convert` (hex methods)

The following three methods are added to `Convert`:

```bezel
  // Encode all bytes in `bytes` as a lowercase hexadecimal string
  // (digits 0–9 and a–f).  Returns a fresh string; never throws.
  // e.g. ToHexString([0xDE, 0xAD]) = "dead"
  public static method ToHexString(bytes: IReadOnlySpan<byte>): string;

  // Decode a hexadecimal string (accepts mixed case; digits 0–9, a–f, A–F).
  // Length must be even (each byte occupies exactly two hex digits).
  // Raises FormatException on an invalid character or odd length.
  // Returns a freshly-allocated Array<byte> owned by the caller.
  public static method FromHexString(s: string): owned Array<byte>;

  // Try-decode form.  Returns true and assigns `result` on success; returns
  // false and assigns `result := null` on failure.
  public static method TryFromHexString(s: string, out owned result: Array<byte>?): bool;
```

**Encode case.** Lowercase `a–f` is the default. Callers needing uppercase call `.ToUpper()` on the result; the `$"{v:X}"` IFormattable specifier (§2.18) covers integer-to-uppercase-hex without going through byte arrays.

---

## 3. Usage examples

```bezel
import System.Text;   // Base64, Base64Url — actually auto-imported; shown for clarity

// Encode a UTF-8 string as Base64 (common for HTTP Authorization headers).
var encoded := Base64.Encode(Encoding.UTF8.GetBytes(user + ":" + pass).AsReadOnlySpan());
// → e.g. "dXNlcjpwYXNz"

// Decode; handle corrupt input without throwing.
if Base64.TryDecode(incoming, out var bytes) then
  using var _ := bytes!;
  // process bytes!
  // (the ! is required until §22.6 flow-narrowing; v0.171 §1 note)
end;

// JWT token ID hash as URL-safe Base64.
var tokenId := Base64Url.Encode(tokenBytes.AsReadOnlySpan());

// Display a SHA-256 digest as lowercase hex.
var hex := Convert.ToHexString(digestBytes.AsReadOnlySpan());
// → e.g. "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

// Parse hex input; strict failure.
var decoded := Convert.FromHexString(hexInput);   // FormatException if invalid
```

---

## 4. Open questions (none for v1)

No open questions block this proposal. The design is fully specified above. If a future need arises for streaming encode/decode (e.g. for large binary blobs in HTTP bodies), that is a separate proposal building on the `Stream` gap (gap-review item #1) and the concrete stream family.

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

There are **no open owner questions** (§4): the design is fully settled — `Base64`/`Base64Url` in `System.Text` and hex on `Convert`; three-method Encode/Decode/TryDecode pairing (Try shape (a), `out owned result: Array<byte>?`); RFC 4648 §4 vs §5 alphabets with strict decode; lowercase hex output, mixed-case decode; `IReadOnlySpan<byte>` input, `owned Array<byte>` output; streaming deferred to v2. Acceptance carries the whole design as written. Docs-editor note at landing: fold in alongside the existing §2.3 `Convert` cleanup (the duplicated-members finding).
