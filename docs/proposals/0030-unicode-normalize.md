# Proposal 0030 — Unicode normalization (explicit)

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Tracking issue** | [#1455](https://github.com/s8lap/bezel-lang/issues/1455) |
| **Target module(s)** | `System.Text` (auto-imported, §3 of stdlib); adds two methods to `String` (§9.6) and a new `NormalizationForm` enum |
| **Written against** | Language spec **v0.177**; stdlib companion v0.165 |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); re-verified clean at v0.306 (2026-09-17 sweep) — §19.6's enum-exhaustiveness argument, `NotImplementedException` (§31.4), the ordinal-equality contract (§9.6/§4.6) and the `:=`-defaulted `form` parameters (already the v0.290 one-signature shape) all still hold. No tier-1 respells, no tier-2 flags. Written-against pin unchanged. |
| **Origin** | Gap review [#1403](https://github.com/s8lap/bezel-lang/issues/1403) item #31 (P2) |

---

## 1. Problem and motivation

The same visible text can be encoded as different Unicode byte sequences. "é" can be one code point (`U+00E9`, precomposed) or two code points (`U+0065` + `U+0301`, base letter + combining acute accent). A string comparison of these two representations is `false` under ordinal equality (§9.6) even though both look and mean the same thing to a human reader.

Normalization converts a string to a canonical or compatibility form, collapsing equivalent encodings to a single representation. After normalization, ordinal equality works correctly for all strings in the same form. Applications that need to compare user-visible text (URL canonicalization, identifier matching in IDN hostnames, database key normalization, text search) require this transform.

This proposal adds the explicit `Normalize` and `IsNormalized` surface to `String`. It is the **sanctioned explicit counterpart** to gap-review declined item #8 (implicit normalization in equality) — that decline is unchanged, and its reasoning remains: equality stays ordinal (§9.6), normalization is an explicit caller choice, and the two are designed to compose (`s.Normalize(NFC) = t.Normalize(NFC)` is the idiom for Unicode-equivalent comparison).

---

## 2. String well-formedness in Bezel — why "invalid input" is impossible

A potential objection to `Normalize` is: what happens when given an invalid or malformed string? The spec answers this:

- **spec §4.6 (v0.177):** "`char` is a single Unicode scalar value, 32 bits wide. Surrogate code points (U+D800 to U+DFFF) are not valid `char` values."
- **spec §4.6:** "string — An immutable sequence of `char` (Unicode scalar values)."

Every Bezel `string` is a sequence of Unicode scalar values (code points 0x0000–0xD7FF and 0xE000–0x10FFFF). Lone surrogates, overlong encodings, and code points above U+10FFFF cannot exist in a valid `string`. NFC and NFD normalization are total functions on sequences of Unicode scalar values — every such sequence has a canonical form. Therefore:

- `s.Normalize(form)` **never faults** for any valid `string` value.
- There is no need for a `TryNormalize` form; the throwing pattern would be dead code.
- This is a specific instance of the general benefit of Bezel's well-typed `char` — the same guarantee that makes `string` iteration always produce valid chars.

---

## 3. The load-bearing design question: how to supply Unicode data tables

Normalization requires Unicode data — specifically, the canonical decomposition mappings and canonical composition exclusion lists from the Unicode Character Database (UCD). These are not embedded in a typical runtime; they must be sourced from somewhere. This is the same class of question as proposal 0012's tzdata packaging decision, and it is the primary design choice this proposal must settle.

### Option A — Bundle canonical NFC/NFD tables in the runtime

Include the Unicode canonical decomposition/composition tables (derived from `UnicodeData.txt` and `CompositionExclusions.txt`) as static data compiled into the Bezel runtime library.

**Size:** The full canonical decomposition table for Unicode 15.1 compresses to approximately 40–70 KB (the exact size depends on encoding; ICU's compressed canonical data is ~50 KB; a minimal Go-style table is ~40 KB). Adding both NFC composition and NFD decomposition tables totals roughly 80–130 KB of static data.

**Pros:** Self-contained, reproducible, no platform dependency, works in any deployment environment (containers, embedded, static-linked binaries).

**Cons:** The data must be versioned against a Unicode version. New Bezel releases may ship updated data; programs compiled against different runtime versions may disagree on normalization of recently-added characters. This is the same version-coupling that tzdata has.

### Option B — System ICU via FFI (spec §32)

Call through `extern` to the platform's `libicu` / `libicuuc` (available on Linux/macOS; not reliably available in constrained environments).

**Pros:** Always uses the platform's current Unicode data; no bundled data size.

**Cons:** Significant dependency debt. ICU is a ~30 MB library; its C API is non-trivial; the FFI surface (spec §32) is trusted-but-unverified. ICU version skew between build and run time produces surprising behavior. Not available in static-linked or container-minimal deployments. Adds a §32 surface just for this one feature.

**Rejected.** The dependency and portability cost is too high for what amounts to a data-table lookup. The FFI surface is for interoperating with existing native libraries that do non-trivial things; it is not the right tool for owning a data table.

### Option C — Bundle canonical (NFC/NFD), defer compatibility (NFKC/NFKD)

Bundle the canonical forms (NFC and NFD) in the runtime from the start. Defer the compatibility forms (NFKC and NFKD) to a later proposal.

**Justification for the deferral of NFKC/NFKD:**
- NFC/NFD handle the primary use cases: text comparison, URL normalization, IDN hostname processing, and identifier canonicalization all use canonical forms.
- NFKC/NFKD additionally collapse compatibility variants: ligatures (`ﬁ` → `fi`), width variants (fullwidth `Ａ` → `A`), and legacy compatibility characters. These are needed for legacy text migration, font engineering, and some IDN processing, but are niche compared to canonical normalization.
- The compatibility mappings are roughly twice the size of the canonical decomposition data — deferring them saves ~50 KB of static data in v1. (These mappings are **not** a separate UCD file: they live in field 5 of `UnicodeData.txt`, prefixed with a compatibility tag such as `<compat>`/`<wide>`/`<fraction>`; the canonical decompositions are the *untagged* entries of the same field. There is no `CompatibilityDecomposition.txt`.)
- The `NormalizationForm` enum ships **all four values from the start** — and this is *forced, not merely convenient*. Enum members are a public surface, and adding one later is a breaking change to exhaustive `case` matching: a `case form of` over `{NFC, NFD, NFKC, NFKD}` with no `_` arm is statically exhaustive (§19.6) *today*; had v1 shipped only `{NFC, NFD}`, every such match written against v1 would become non-exhaustive when the compatibility members land — silently faulting with `MatchFailureException` on the new value in expression position (§19.6, the enum-cast/out-of-range path). Shipping the members up front and staging only the *implementation* (NFKC/NFKD raise `NotImplementedException`, §31.4, until the tables are bundled) is the only sequence that avoids breaking downstream exhaustive matches — this is the strongest argument for surface-complete-implementation-staged, and it applies to any enum the stdlib exposes.

**This proposal recommends option C.**

---

## 4. `NormalizationForm` enum

```bezel
public enum NormalizationForm
  // Canonical decomposition, followed by canonical composition.
  // The most common form for text comparison and storage.
  NFC,

  // Canonical decomposition only.
  // Decomposes all composed characters to their base-plus-combining-marks form.
  NFD,

  // Compatibility decomposition, followed by canonical composition.  NOT YET IMPLEMENTED in v1.
  // Raises NotImplementedException until a later revision ships the compatibility tables.
  NFKC,

  // Compatibility decomposition only.  NOT YET IMPLEMENTED in v1.
  // Raises NotImplementedException until a later revision ships the compatibility tables.
  NFKD,
end NormalizationForm;
```

The v1 implementation contract: `NFC` and `NFD` are always implemented. `NFKC` and `NFKD` raise `NotImplementedException` (spec §31.4) in v1 implementations. The `NotImplementedException` text must name the form and direct to the proposal tracking issue or the relevant stdlib section. A later revision removes the `NotImplementedException` when the compatibility tables are bundled.

---

## 5. String methods

Two methods are added to `String` (stdlib §9.6). Both are in `System.Text`, auto-imported:

```bezel
// Additions to class String (§9.6 of stdlib):

// Returns a new string that is the normalization of this string under the given form.
// NFC and NFD are always supported.  NFKC/NFKD raise NotImplementedException in v1.
// Never faults for NFC/NFD: every Bezel string is a valid Unicode scalar-value sequence
// (spec §4.6), and canonical normalization is total on such sequences.
public method Normalize(form: NormalizationForm := NormalizationForm.NFC): string;

// Returns true if this string is already in the given normalization form.
// NFC and NFD are always supported.  NFKC/NFKD raise NotImplementedException in v1.
// IsNormalized is guaranteed to be no slower than Normalize itself; implementations
// are expected to short-circuit as soon as a non-normalized character is found.
public method IsNormalized(form: NormalizationForm := NormalizationForm.NFC): bool;
```

**Default form is NFC.** NFC is the most commonly needed form: it is what most databases, URLs, and IDN implementations treat as the canonical representation. An explicit no-argument call `s.Normalize()` therefore does the right thing for the common case.

**Idempotence.** By the Unicode standard, normalization is idempotent: `s.Normalize(NFC).Normalize(NFC) = s.Normalize(NFC)` for all `s`. Equivalently, if `s.IsNormalized(NFC)` then `s.Normalize(NFC) = s`. Implementations must uphold this.

**`IsNormalized` does not modify.** It is a read-only predicate. It is useful as a fast check before an expensive operation: "normalize input if not already normalized" — `if not s.IsNormalized() then s := s.Normalize() end`.

**Allocation semantics.** `Normalize` returns a `string` (reference type). If the input is already normalized, implementations may return `this` (the same string object) to avoid allocation; this is an optimization, not a guarantee. Programs must not rely on referential identity of the result.

---

## 6. The equality contract stays unchanged

**Gap-review decline #8 (v0.171):** "Implicit Unicode normalization in string equality — equality stays ordinal (§9.6 contract); normalization ships only as an explicit transform (`Normalize(form)`, item 31)."

This proposal is precisely that explicit transform. The ordinal contract of `=` on `string` (spec §4.6, stdlib §9.6) is unchanged:

```bezel
var a := "é";           // precomposed é (U+00E9)
var b := "é";          // decomposed  e + combining acute (U+0065 U+0301)
var equal1 := a = b;         // false — ordinal equality, different code points

// After normalization to the same form, ordinal equality works:
var equal2 := a.Normalize() = b.Normalize();  // true — both are NFC "é"
```

The `=` operator never normalizes implicitly. Any code that needs Unicode-equivalent comparison must call `Normalize` explicitly. This is by design: normalization is not free (it is O(n) with table lookups), and many use cases genuinely want ordinal comparison (byte-for-byte round-trip fidelity, diagnostic output, protocol-level string matching).

---

## 7. `System.Globalization` relationship

`Normalize`/`IsNormalized` are in `System.Text` (auto-imported), not `System.Globalization`. Reasons:
- Normalization is a Unicode algorithm, not a culture-specific behavior. NFC is NFC everywhere; it does not depend on locale.
- `System.Globalization` is reserved for locale-aware operations (culture-sensitive sorting, number/date formatting with locale) — none of which `Normalize` involves.
- Placing it in `System.Text` alongside `Encoding` keeps all the "bytes ↔ strings" and "string canonicalization" tools in one module, matching where callers will look.

---

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

The load-bearing design question (§3, data-table sourcing) is resolved as **Option C: bundle canonical NFC/NFD in the runtime now, defer compatibility NFKC/NFKD** to a later revision. The ICU-FFI route (Option B) is declined — dependency debt (~30 MB library, a §32 trusted surface, build/run version skew, unavailable in static/container-minimal deployments) is disproportionate to a data-table lookup, consistent with the project's dependency-debt posture. Two strengthening edits were applied at acceptance: (1) the §3 justification now states the *forcing* reason the enum ships all four members up front — later enum additions break statically-exhaustive `case` matches (§19.6), so surface-complete-implementation-staged is required, not merely convenient (NFKC/NFKD raise `NotImplementedException`, §31.4, until the tables land); (2) the erroneous `CompatibilityDecomposition.txt` reference is corrected — compatibility mappings live in field 5 of `UnicodeData.txt` (tagged), not a separate UCD file. The equality contract stays ordinal (§9.6/§4.6); `Normalize`/`IsNormalized` are the explicit counterpart to declined item #8, and never fault for NFC/NFD because every Bezel `string` is a valid Unicode scalar-value sequence (§4.6). Lands in the d3-small-utilities batch (self-contained Unicode-table packaging) as `System.Text` `NormalizationForm` + two §9.6 `String` methods.

## 8. Declined alternatives (register)

1. **Implicit normalization in `=`** — the standing decline #8 from the gap review; unchanged. Ordinal equality is the contract of §9.6 and §4.6.
2. **System ICU via FFI (option B)** — declined; dependency and portability cost outweigh the benefit (§3).
3. **Bundle all four forms including NFKC/NFKD immediately** — declined for v1; the compatibility tables double the data size for use cases that are niche in Bezel's target domain (§3, option C reasoning).
4. **Raising `FormatException` instead of `NotImplementedException` for NFKC/NFKD** — declined; `FormatException` signals bad input data (§31.4); `NotImplementedException` signals an unimplemented operation, which is the correct signal for "this form is not yet supported."
5. **A separate `StringComparer.Unicode(NFC)` comparison class** — deferred; a `StringComparer` or `IComparer<string>` surface could compose normalization with case-folding for richer text comparison. Out of scope for this proposal; the explicit `Normalize` + ordinal `=` composition is the v1 idiom.
6. **Accepting `string?` overloads** — declined; `null` strings are handled by the caller; a null-safe `IsNormalized` would silently accept a `null` input, masking a likely caller bug. Use `(if s = null then false else s.IsNormalized())` explicitly.

---

## 9. What v1 deliberately leaves out

- NFKC/NFKD (compatibility forms) — deferred per §3 option C.
- `StringComparer` / `IComparer<string>` with Unicode-aware collation (culture-aware sort) — `System.Globalization` territory.
- Normalization of `ReadOnlyStringSpan` — additive, deferred.
- Case-folding combined with normalization — a separate future addition.

---

## 10. Proposed stdlib additions (ready-to-land text)

**Addition to `System.Text` section of `Bezel_Standard_Library.md`:**

Insert `NormalizationForm` as a new subsection before or after the existing `Encoding` section (exact numbering assigned at integration time).

```
### 3.N  NormalizationForm

An enum whose values name the four Unicode normalization forms.

    public enum NormalizationForm
      NFC,    // Canonical Decomposition, Canonical Composition
      NFD,    // Canonical Decomposition
      NFKC,   // Compatibility Decomposition, Canonical Composition  [deferred in v1]
      NFKD,   // Compatibility Decomposition                         [deferred in v1]
    end NormalizationForm;

`NFKC` and `NFKD` raise `NotImplementedException` in v1 runtime implementations
(the compatibility mapping tables are not bundled until a subsequent release).
```

**Additions to `§9.6 String`:**

```
  // Normalization (v0.TBD)
  // Returns a string in the specified Unicode normalization form.
  // NFC and NFD are fully supported; NFKC and NFKD raise NotImplementedException
  // in v1 (compatibility tables not yet bundled).
  // Never faults for NFC/NFD: all Bezel strings are valid Unicode scalar-value
  // sequences (spec §4.6), and canonical normalization is total on such sequences.
  // Returns `this` unchanged when already normalized (idempotent; may share storage).
  public method Normalize(form: NormalizationForm := NormalizationForm.NFC): string;

  // Returns true if the string is already in the specified normalization form.
  // No faster than Normalize itself; implementations short-circuit on first mismatch.
  // NFC and NFD are fully supported; NFKC and NFKD raise NotImplementedException in v1.
  public method IsNormalized(form: NormalizationForm := NormalizationForm.NFC): bool;
```
