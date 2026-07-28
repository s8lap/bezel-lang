# Proposal 0074 — Small-stdlib ergonomics batch III (API design review, 2026-07-28)

| | |
|---|---|
| **Status** | draft |
| **Tracking issue** | — |
| **Target module(s)** | stdlib only — `System` ([§2.4](../Bezel_Standard_Library.md#sec-environment) `Environment`, [§2.6](../Bezel_Standard_Library.md#sec-guid) `Guid`, [§2.18](../Bezel_Standard_Library.md#sec-iformattable) format specifiers), the [§9.6](../Bezel_Standard_Library.md#sec-string) `String` primitive, plus one [§1](../Bezel_Standard_Library.md#sec-conventions) convention sentence and one [§4.8](../Bezel_Standard_Library.md#sec-dictionaryk-v) disambiguating clause. **No spec change, no new types, no new grammar.** |
| **Written against** | stdlib/spec v0.228 |
| **Origin** | The 2026-07-28 owner-directed API design review ("we copy .NET too often — can the common use cases be clearer/simpler/easier?"), adversarially critiqued; every item below survived the critique, and the candidates it killed are recorded in §7 with the critique's reasons. The review's single recurring *design* question — the nullable-return tier — is not here; it has its own home (proposal [0072](0072-nullable-return-convenience-tier.md)). |

*Reference convention: unprefixed §-references are stdlib references; spec references are marked.*

---

## 0. Shape of the batch

**Deliberately thin**: one rename, one symmetry addition, three contract pins (one at §2.6; two at §2.18 — the pass-through rule and its vocabulary-growth rider), one convention sentence, and one §4.8 disambiguating clause riding the rename. That thinness is itself the review's finding at this size class: the v0.228 surface is largely *done* — most .NET-familiar "missing" members turned out on verification to exist, to be already declined, or to be wrong (§7) — and what remains is naming debt and unstated contracts, not capability. Item 1 is **breaking** and is flagged as such where it stands. Each item is independently landable, in the mold of proposals 0062 (graduated v0.215) and 0067 (graduated v0.228).

**Ownership and sendability, stated once.** Nothing here mints, borrows, or disposes beyond what §2.4 already does: item 1's enumeration member keeps the existing `owned Dictionary<string, string>` return (the §1 fresh-instance convention), item 2 is a pure value read, and items 3–5 are documentation pins. No kernel rule is engaged; sendability (spec [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)) is unaffected throughout.

---

## 1. `Environment.Get` / `Set` / (`Variables`?) — the rename (**BREAKING**)

**Today (§2.4, v0.228).** The environment-variable family reads:

```bezel
public static method GetEnvironmentVariable(name: string): string?;
public static method GetInt(name: string): int?;     // v0.215 convenience tier
public static method GetBool(name: string): bool?;   // v0.215 convenience tier
public static method SetEnvironmentVariable(name: string, value: string?);
public static method GetEnvironmentVariables(): owned Dictionary<string, string>;
```

The family is internally inconsistent: v0.215 coined the short Bezel-idiom tier (`GetInt`, `GetBool`) **beside** the .NET-inherited long names, so one class now spells the same concept two ways — `Environment.GetInt("PORT")` next to `Environment.GetEnvironmentVariable("PORT")`. The class name already says *Environment*; the member repeating it is .NET's spelling, not information — the review's charge ("we copy .NET too often") lands squarely on this trio, and it is the hottest surface in the batch (config reading appears in essentially every service's startup path).

**Proposed:**

```bezel
// PROPOSED — renames (BREAKING; migration below)
public static method Get(name: string): string?;          // = today's GetEnvironmentVariable:
                                                           //   null when absent, unchanged
public static method Set(name: string, value: string?);   // = today's SetEnvironmentVariable:
                                                           //   null removes; an empty name, or one
                                                           //   containing "=", raises ArgumentException
                                                           //   — contract unchanged (v0.195 pin)
// the enumeration — open question Q2:
public static method Variables(): owned Dictionary<string, string>;
                                                           // = today's GetEnvironmentVariables;
                                                           //   or keep the long name ("GetAll" was
                                                           //   considered and reads thinly — a Get*
                                                           //   name with nothing after "All" to type it)
```

`GetInt`/`GetBool` are already short and stay untouched; after the rename the family has one register: `Get` is the raw `string?` read, `GetInt`/`GetBool` the typed conveniences over it.

**`Set` is legal and unambiguous — with one clause to add.** Three checks, run against the documents:

- *Legality.* `set` is a **contextual** keyword (spec [Appendix B.2](../Bezel_Language_Spec.md#sec-contextual-keywords): accessor-declaration positions only; "anywhere else" it is an ordinary identifier) — and the member name `Set` is a different, PascalCase identifier in any case.
- *The type `Set<T>`.* §4.10's collection does not collide: `Environment.Set(...)` is a qualified member access — a member position after a resolved type head, where members are consulted (spec §2.7's position-keyed resolution) — and the bare type name `Set` is only ever a *chain head*.
- *The §4.8 adjacency.* §4.8 states: "There is no separate `Set` method — neither here nor on any other collection … A call to a `Set` method on any collection is rejected as an unknown member." That rule is **collections-only** (it protects the indexer-write surface), but with `Environment.Set` landing one shelf over, the sentence deserves a disambiguating clause so no reader generalizes it. Proposed addition to the §4.8 sentence:

  > "(A rule about the collections' write surface only, not about the name: a `Set` member elsewhere in the library — e.g. `Environment.Set`, §2.4 — is unrelated.)"

**Migration (breaking; small; and the window is closing).** There are no external users; the entire in-tree exposure, counted at v0.228:

- **src** — the one codegen intrinsic site lowering `Environment.GetEnvironmentVariable` (`src/codegen/LlvmBackend_Calls.cpp`, the §2.4 dispatch: 2 occurrences, comment + match) retargets its member-name string;
- **tests** — the conformance mirror `tests/conformance/cases/stdlib_0003.bzl` declares the three long names (3 lines);
- **stdlib doc** — six mentions (§2.4's three declarations and prose, §2.13's cross-reference to the `GetEnvironmentVariables` result);
- **proposals** — 0011, 0013, and 0045 reference the long names. Coordination note, not churn: proposals are pinned at their own written-against revisions (the CLAUDE.md citation discipline), so they take a currency note at their next revalidation rather than a retro-edit.

This is as cheap as a rename ever gets — a handful of grep-mechanical sites — and it only gets more expensive: the first external user makes it permanent.

**Considered and declined: keeping the long names for .NET familiarity.** The §1 naming rule (v0.165, #1359) — "names inherited verbatim from .NET keep the .NET spelling" — governs the *casing of names the library keeps* (`IEEERemainder`, `Encoding.UTF8`), and rationale [§15.x](../Bezel_Design_Rationale.md#sec-convert-is-a-familiarity-alias) shows the project tolerating an **inherited** familiarity duplicate (`Convert`); neither forbids renaming by recorded decision — the v0.168 owning-enumeration-interface family rename (#1372, recorded in §1 itself) is the precedent that renames happen when a family is inconsistent. The alternative is stated honestly as open question Q1: rename nothing and record the two-register family as an accepted §15.x-style cost.

---

## 2. `String.IsEmpty` (§9.6) — the symmetry addition

**Today.** `IsEmpty` exists on `ReadOnlyStringSpan` (§3.3), on every §4.14/§4.15 span type and `IReadOnlySpan<TElement>` (§4.16), and on `Range<T>` (§9.8) — but not on `String`, whose **own span view has it**: `s.AsSpan().IsEmpty` is spellable today, `s.IsEmpty` is not. The instance spelling of "is this string empty" is `s.Length = 0`; the static `IsNullOrEmpty(value: string?)` exists but answers a different (null-tolerant) question.

**Proposed** (§9.6, beside `Length`):

```bezel
// PROPOSED
public pure readonly property IsEmpty: bool;    // Length = 0 — §3.3's spelling
```

**Contract.** `IsEmpty` = (`Length = 0`); O(1) (`Length` is O(1), §9.6). The definition is copied from §3.3's `IsEmpty: bool => Length = 0`, restated in `String`'s own declaration style (`pure readonly`, matching its `Length`/`IsAscii` neighbors). `IsNullOrEmpty` stays — the static is the null-tolerant form; the property requires a non-null receiver.

**Boundary, recorded.** The collections deliberately carry `Count` only — no `IsEmpty` anywhere in §4's add/remove family, and this item does not open one. `IsEmpty` is a property of the *extent-bearing* types (item 5's `Length` family), where the span family already established it; `String` is the one extent-bearing type that lacked it.

---

## 3. `Guid.NewGuid` (§2.6) — the contract pin

**Today.** The entire specification of `NewGuid` is one bare line — `public static method NewGuid(): Guid;` — with no version, no uniqueness statement, no randomness source. The gap is not hypothetical pedantry, for two reasons the same documents create:

- §2.6 pins `CompareTo` as "lexicographic over the 16 canonical big-endian bytes (stated explicitly — .NET's field-group order is a known trap)" — so the version-4-vs-version-7 choice is **observable through sort order**: a v4 generator sorts fresh ids randomly, a v7 (time-ordered) generator sorts them by creation time. With `NewGuid` unpinned, the behavior of a `SortedSet<Guid>` over generated ids is implementation-defined while `CompareTo` itself is pinned to the byte.
- §1's replay-intercept bullet (v0.204) already declares §2.6 an **entropy intercept point** — the document asserts that `NewGuid` reads the world's entropy while never stating what it produces from it.

**Proposed pin** (§2.6, as the member's contract comment):

```bezel
public static method NewGuid(): Guid;
      // PROPOSED PIN — a version-4 (random) UUID: the version and variant bits
      //   are fixed (0100 / 10), the remaining 122 bits drawn from the runtime's
      //   entropy source (the §1 v0.204 intercept point this section already
      //   declares). Uniqueness is probabilistic, not checked: no dedup, no
      //   cross-process coordination. The entropy source is NOT specified as
      //   cryptographically strong — §2.7's recorded posture: cryptographic
      //   randomness belongs to the future System.Security.Cryptography module
      //   — so NewGuid values must not serve as secrets (session tokens,
      //   capability strings).
```

The randomness-source sentence is deliberately *relative*: it neither promises CSPRNG quality (which would front-run the crypto module's design — proposal [0068](0068-crypto-primitives.md)'s `RandomNumberGenerator` is that surface, draft) nor permits a seedable §2.7-style PRNG in disguise (the intercept-point framing keeps it an entropy read, not a deterministic stream).

**The v7/sortable question stays a capability decision — not this pin.** A time-ordered id (UUIDv7) changes the observable sort order *and* leaks creation time into the identifier; if wanted, it should arrive as a separately named member by its own proposal, never as a silent version change under the `NewGuid` name this pin fixes. Related plumbing: 0068's secure-random seam. Recorded as open question Q3.

---

## 4. §2.18 temporal pattern literals — resolve [#2243](https://github.com/s8lap/bezel-lang/issues/2243) with interpretation (a)

**The defect (issue #2243, filed 2026-07-28 per the CLAUDE.md stop-and-open-an-issue rule).** §2.18's temporal row specifies "pattern strings of `yyyy` `MM` `dd` `HH` `mm` `ss` `fff` plus literal separators", and §2.18's fault rule says "An unrecognized specifier raises `FormatException` (strict-fault, as everywhere)". Whether an **alphabetic** non-specifier character — the `T` in `yyyy-MM-ddTHH:mm:ss` — is a "literal separator" (pass-through) or an "unrecognized specifier" (fault) is unstated. The surrounding texts presuppose the literal reading: the no-format `DateTime.ToString()` renders `yyyy-MM-ddTHH:mm:ss` (§2.5) — the pattern of the type's *own default output* is unwritable if `T` faults — and proposal [0010](0010-serialization-value-builtins.md)'s owner-ruled (2026-07-04) conditional-`Z` wire form is sketched in 0010's own text as `ToString("yyyy-MM-ddTHH:mm:ss.fff") + "Z"`. A user today cannot reliably write the very pattern an owner ruling specifies. (Scope note: this is a `ToString(format)` question only — no pattern-driven `Parse` exists; §2.5's `Parse` accepts the pinned rendered forms and nothing else.)

**Proposed: interpretation (a), pinned.** One sentence added to §2.18's temporal entry:

> **Proposed §2.18 sentence.** "In a temporal pattern, every character that does not begin one of the specifiers above is a **literal**, copied to the output verbatim — `T`, `-`, `:`, space, and any other non-specifier character alike; the unrecognized-specifier fault governs malformed runs of the specifier letters (`y`, `M`, `d`, `H`, `m`, `s`, `f`), not literal characters."

with the growth rider, recorded as a rule beside it:

> **Proposed growth rider.** "The temporal specifier vocabulary grows only by recorded ruling, and never in a way that reinterprets a character that passes through as a literal in existing patterns — a new specifier letter is a breaking change for every pattern that used it as a literal, and this section's default answer to such a proposal is no."

**The alternative, presented for the owner: (b) quoting/escaping.** Alphabetic non-specifiers fault; literals must be quoted (`'T'` — the C# mechanism). It reserves the letter space for future vocabulary, at three costs: an escape mechanism §2.18 currently lacks (new syntax inside format strings); a break of the presupposed unquoted spelling (both §2.5's self-describing default and 0010's ruled wire-form sketch would need requoting); and ceremony on every pattern to keep an option nothing currently needs. #2243's option (c) — an ad-hoc allowlist (`T`, `Z`) — is declined by issue and proposal alike. **Recommendation: (a)**; the owner may prefer (b) if reserving the letter space outweighs pattern simplicity (open question Q4).

---

## 5. §1 `Count`-vs-`Length` — the convention sentence (doc-only)

The rule is real and consistently followed, but stated nowhere: every extent-bearing type says `Length` — `String` (§9.6), `StringBuilder` (§3.1), `ReadOnlyStringSpan` (§3.3), `OwningBuffer<T>` and the span family (§4.14–§4.16), `FrozenArray<T>` (§4.19), `Stream` (§7.1, as `long`), `CString`/`WString`/`RawBytes` (§10) — while every add/remove collection says `Count` (`Array<T>`, the dictionaries, sets, stacks, queues, `LinkedList<T>`, `Counter<K>`, `Channel<T>`'s buffered count). No type carries both. Because the rule is unwritten, every new type re-derives it, and readers arriving from .NET — where `Array.Length` sits beside `List.Count` on the *same* kind of surface — have no anchor for why Bezel's resizable `Array<T>` says `Count`. One sentence in §1 pins it:

> **Proposed §1 sentence.** "**`Count` vs `Length`.** `Length` measures a fixed or contiguous extent — text (`String`, `StringBuilder`, `ReadOnlyStringSpan`), buffers and spans (§4.14–§4.16), `FrozenArray<T>`, `Stream`, and the §10 FFI carriers; `Count` counts the elements of a collection whose membership grows and shrinks (the `Add`/`Remove`/`Push`/`Enqueue` families). A type declares one or the other, never both, and new types follow the same line — which is why the resizable `Array<T>` says `Count` where .NET's fixed array says `Length`."

Doc-only; no member changes anywhere.

---

## 6. Cross-references (review findings routed elsewhere, so this batch stays thin)

- **`TakeLast`/`SkipLast`** — belong to the §5 LINQ surface and ride proposal [0026](0026-linq-additions.md); a separate addendum to 0026 handles them. Not this batch.
- **`Enum.Parse<E>` (string→enum)** — issue [#2244](https://github.com/s8lap/bezel-lang/issues/2244): a compiler-known generic over `E: enum`, not a library member; routed to its own small proposal per the issue's routing note.
- **`DateTime.ToIso8601()`** — routes to proposal [0010](0010-serialization-value-builtins.md)'s owner-ruled wire form, and is **wrong as a standalone general-purpose member**: the ruled rendering is `Kind`-conditional (`Z` iff `Kind.Utc`), and a `Kind = Local` value has no encodable offset in Bezel (no tzdata, §2.18) — a general `ToIso8601()` would lossily render local time as if unzoned. That hazard is manageable at 0010's serializer boundary, where the `Kind` contract is pinned; it is not a §2.5 convenience.

## 7. Considered and declined (the review's other candidates, with the critique's reasons)

- **`s.ToUtf8Bytes()` / `String.FromUtf8(bytes)`** — declined. Rationale §15.x keeps `Convert` as an **inherited** familiarity duplicate ("deleting would break arriving muscle memory for zero space savings") — reasoning that extends no license to a *new* duplicate, which serves no arriving muscle memory; `System.Text` is auto-imported (§3), so `Encoding.UTF8.GetBytes(s)` / `GetString(bytes)` is reachable with zero imports; and §9.6's own closing prose already routes byte access there, one-shot and allocation-conscious forms both. A new duplicate would add a second normative spelling of an operation the document just finished consolidating (§3.4's v0.188 retirement).
- **`Console.ErrorLine(...)`** — declined. `Console.Error.WriteLine(...)` works with zero imports — `Console` is auto-imported and calling members on its `Error` property is *use*, which never requires importing `TextWriter`'s module (spec [§2.8.3](../Bezel_Language_Spec.md#sec-transitive-closure-and-type-identity) rule 2: use without import; naming requires import) — and `TextWriter` already carries the full v0.165 per-type `WriteLine` family (§7.4). A parallel static family would duplicate a dozen overloads to save seven characters.
- **`Guid.New()` rename** — declined. 38 in-tree occurrences of `NewGuid` (docs + src + tests, counted at v0.228) for a rename whose short form visually echoes the `new` operator (`new Guid(...)` vs `Guid.New()`); and the review's real §2.6 finding is the missing *contract*, which item 3 fixes — the name was never the problem.
- **`DateTime.Add`/`Subtract` removal** (in favor of the operator group) — declined. The members are the §15.x class of inherited familiarity alias; removal is breaking for zero expressive gain, and `Subtract(other: DateTime): TimeSpan` beside `Subtract(span: TimeSpan): DateTime` states in names a distinction the `-` operators express by type — one contract spelled twice, tolerable for inherited surface on §15.x's reasoning.
- **Fluent `Join` on `IEnumerable<string>`** (`items.Join(", ")`) — declined. `String.Join(separator, items)` exists (§9.6); the fluent form is a second spelling of one operation (the §15.x line again: tolerate inherited duplicates, mint no new ones), and it would be the sole aggregation-to-string member on the §5 extension surface, an inconsistency of its own.
- **`StringBuilder.Append(value: Object)` removal** — declined. The overload matches `Console.Write(value: Object)` (§2.1) and `TextWriter.Write(value: Object)` (§7.4) — one convention across the three sinks — and the v0.165 per-type overloads already keep boxing off the hot paths (the declared `bool`/numeric family, §3.1); the `Object` form is the cold-path catch-all, not a performance trap.

## 8. Open questions for the owner

- **Q1 (item 1):** accept the `Get`/`Set` renames (recommended — the no-users window argument), or decline and record the two-register family as an accepted §15.x-style cost?
- **Q2 (item 1):** the enumeration member — rename to `Variables()` (recommended), keep `GetEnvironmentVariables` while renaming only `Get`/`Set` (a smaller break, a residual long name), or another spelling? (`GetAll` was considered and reads thinly.)
- **Q3 (item 3):** confirm the version-4 pin; and is a time-ordered (v7-style) id wanted as a *separately named future member* (capability decision; 0068's secure-random seam is the related plumbing), or should its decline be recorded now?
- **Q4 (item 4):** interpretation (a) with the growth rider (recommended), or (b) quoting/escaping to reserve the specifier letter space?
- **Q5 (item 5):** land the `Count`-vs-`Length` sentence as drafted, including its closing `Array<T>`-vs-.NET clause, or trimmed to the bare rule?
