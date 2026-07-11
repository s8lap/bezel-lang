# Proposal 0038 (RFC) — Retire `ReadOnlyAsciiByteSpan`: five span types, not six

| | |
|---|---|
| **Status** | **superseded — graduated in v0.188 (`ebfa7e0`)**: stdlib §3.4 tombstoned + §9.6 deleted + §4.16 six→five, spec §4.6/§28.3/§28.11/§29.2 + Appendix D.2 trimmed, rationale §5.15/§15/§18; #971 item 3 retired. Open questions resolved as delegated rulings (recorded in the v0.188 changelog). *(prior: accepted (owner, 2026-07-12); draft (RFC))* |
| **Tracking issue** | [#1649](https://github.com/s8lap/bezel-lang/issues/1649)
| **Target module(s)** | Stdlib [§3.4](../Bezel_Standard_Library.md#sec-readonlyasciibytespan) (deleted), [§9.6](../Bezel_Standard_Library.md#sec-string) (`AsAsciiByteSpan`/`TryAsAsciiByteSpan` + one prose passage), [§4.16](../Bezel_Standard_Library.md#sec-ireadonlyspantelement) (six→five type list, one table row, one example sentence), §1 conventions (one naming exemplar deleted from a pair); spec [§4.6] (one passage), [§28.3] (one producer-table mention), [§28.11] (one move-exclusion mention), [§29.2] (one desugaring paragraph), Appendix D.2 (Buffers-and-spans row: "stdlib §3.3–3.4" → "stdlib §3.3"); rationale **living sections only** (§5.15, §15, §18 — changelogs are append-only history and are not edited). |
| **Written against** | spec/stdlib **v0.183**. *(Carried to v0.184: the cited sections are unchanged by the 2026-07-11 ownership-coherence revision; re-verify at landing.)* Every citation below re-verified against the v0.183 text and independently re-verified by an adversarial critique pass (2026-07-11): every count, quote, file, and line checked out, and the killer-counterexample hunt was run to **exhaustion** (full enumeration) and came up empty. The compiler evidence is the corrected form: *five name-classification mentions plus one deferral comment, zero functional surface* (an earlier "zero src/ hits" claim was false and was corrected before this finalization). |
| **Design character** | **RFC** — a removal of a **recorded** v0.106 stdlib decision, which is precisely why this is a proposal and not an edit (§3). The recommendation is argued; the owner decides. |

## 0. Summary

`ReadOnlyAsciiByteSpan` (stdlib [§3.4](../Bezel_Standard_Library.md#sec-readonlyasciibytespan)) is the sixth and weakest member of the span family: a read-only byte view over an ASCII string, added in v0.106 as a zero-allocation alternative to `Encoding.UTF8.GetBytes` for "FFI and I/O paths." This RFC recommends **deleting it**, together with its construction surface (`string.AsAsciiByteSpan()` / `TryAsAsciiByteSpan(out)`) and its per-type tax across four spec sections, the §4.16 interface table, and Appendix D.2.

The case, in one paragraph: the type's motivating guarantee was **conditional from birth** (§3.4 itself: zero-copy holds only "when the string is stored in 1-byte-per-code-point representation … the language does not require it"); its motivating *use case* has been overtaken — the FFI boundary never admitted it at all (records are not FFI-compatible, spec §32.5), and the I/O allocation cost it existed to avoid is now addressed for **all** strings, not just ASCII ones, by v0.165's `Encoding.GetBytes(s, destination: Span<byte>)` into a caller-reused buffer; it is the standard library's only documented **silently-unsafe public constructor** ("passing a non-ASCII string to the constructor would produce a span whose indexer might silently lose information on access … an obvious bug", §3.4 — documented, not fixed); **nothing builds on it** (the one cross-type bridge is one-directional, on the ascii span itself; proposals 0009/0029 consume bytes via `IReadOnlySpan<byte>` and merely list it as one implementor); and the compiler has **never implemented its functional surface** (#971 defers the entire `AsAsciiByteSpan` family; the conformance case for §3.4 is already `skip: blocked-on-#971`). Removal is therefore the rare deletion with essentially no migration: no landed functional code is stranded, no document outside the edit list uses the type, and the unchecked constructor is fixed by ceasing to exist.

Two things this proposal is **not**:

- **Not a span-family unification.** The recorded v0.104–v0.107 decision — separate nominal span types per source, no source polymorphism — **stands**; this RFC removes one member under that architecture and explicitly declines to reopen the architecture (§7).
- **Not a removal of ASCII affordances.** `string.IsAscii` (spec §4.6) is independent and stays; `Encoding.ASCII`/`UTF8` and their strict error models stay; byte-level parsing on buffers (`ReadOnlySpan<byte>` accessors, and the v0.165 #1358 mirror onto `ArraySpan<byte>`/`ReadOnlyArraySpan<byte>`) stays.

## 1. The type today (v0.183)

Stdlib §3.4 declares a borrow-bearing record `ReadOnlyAsciiByteSpan(Source: string, Start: int, Length: int): IReadOnlySpan<byte>` with: bounds-checked indexer `[i]: byte` (reading `byte(Source[Start + i])`), `IsEmpty`, `Slice`, `IndexOf(byte)`, `Contains(byte)`, three `SequenceEqual` overloads (own type; `ReadOnlySpan<byte>` cross-source; `IReadOnlySpan<byte>` interface form, v0.108), and `CopyTo(destination: Span<byte>)` (v0.109). Construction is via `string.AsAsciiByteSpan()` (throws on non-ASCII) / `TryAsAsciiByteSpan(out value): bool` (stdlib §9.6), both of which validate the ASCII invariant; the **direct positional constructor is admitted and validates nothing** — §3.4's own "Construction" paragraph documents the resulting silent-narrowing hazard rather than closing it.

Three of §3.4's own paragraphs supply this RFC's ammunition, quoted so the argument stays pinned to the text:

- **The conditional guarantee** ("Per-access cost"): "When the string is stored in wider representation, the indexer reads the wider value (2 or 4 bytes) and narrows to byte; the view is still allocation-free, but the per-access cost is a width-dispatched read plus a narrowing operation. Implementations are expected to use 1-byte storage for ASCII strings whenever practical, **but the language does not require it**".
- **The in-section fallback**: "programs requiring guaranteed contiguous-byte access for FFI can still use `Encoding.UTF8.GetBytes(s)` … at the cost of one allocation."
- **The unchecked constructor** ("Construction"): "the direct constructor … does not perform an ASCII check itself — passing a non-ASCII string to the constructor would produce a span whose indexer might silently lose information on access (the byte cast would narrow a code point above 127 to its low 8 bits, an obvious bug)."

## 2. The per-type tax (the ledger)

A span type is never just its stdlib section. Every member of the family costs, and this member pays the full rate (all locations verified against v0.183):

1. **Stdlib §3.4** — the ~58-line section itself.
2. **Stdlib §9.6** — two method declarations (`AsAsciiByteSpan`, `TryAsAsciiByteSpan`) plus the prose passage routing ASCII byte access through them.
3. **Stdlib §4.16** — the "all six concrete span types" enumeration, the implements-table row (`ReadOnlyAsciiByteSpan` → `IReadOnlySpan<byte>`), and the `SequenceEqual`-through-the-interface example built on `byteSpan.SequenceEqual(asciiSpan)`.
4. **Stdlib §1 (conventions)** — the acronym-naming ruling (v0.165, #1359) uses the pair (`IeeeEquals`, `AsAsciiByteSpan`) as its PascalCase exemplars; the edit **deletes** `AsAsciiByteSpan` from the pair (`IeeeEquals`, already present, remains the exemplar) — a deletion, not a swap.
5. **Spec §4.6** — the zero-copy-byte-view passage advertising the two methods.
6. **Spec §28.3** — the move-producer table's exclusion row names it among the span types that "do not admit `move` for any T".
7. **Spec §28.11** — the move-admission prose names it again ("the string-backed read-only span types `ReadOnlyStringSpan` … and `ReadOnlyAsciiByteSpan` … also do not admit `move`").
8. **Spec §29.2** — a dedicated `for`-desugaring paragraph (direct indexed loop, `byte` element, borrow of the underlying string).
9. **Appendix D.2** — the Buffers-and-spans row's stdlib list spans "§3.3–3.4"; shrinks to "§3.3".
10. **Rationale living sections** — §5.15 (the ~9-line "ASCII byte spans (v0.106)" design-story block, the six-type family accounting, the CopyTo symmetry table row, the "three-way symmetry across the standard collection sources" sentence), §15 Strings (the byte-level-view sentence), §18 Things deferred (the spans-over-additional-sources entry's inventory). Changelog entries (v0.106, v0.107, v0.108, v0.109 …) are history and stay.
11. **Compiler** — five name-classification mentions (`src/sema/ExprTypecheck_Exprs.cpp:3291,3312`; `src/ownership/Ownership_impl.hpp:912`; `src/ownership/Ownership_Checks.cpp:5697`; `src/common/OwnershipFacts.hpp:129`) where the type sits in string-backed-span classification lists, plus one deferral comment (`src/sema/ExprTypecheck_Calls.cpp:9611`). **No functional surface exists** — #971 records the entire `AsAsciiByteSpan` family as "Not started."
12. **Conformance/tests** — two conformance cases mention the type: `stdlib_0032` (the §3.4 record itself; manifest row 358, already `skip / blocked-on-#971`) and `stdlib_0107` (the §9.6 string surface; manifest row 433, `96-String`, `skip:signature`), which declares the two `AsAsciiByteSpan` method lines at ~L40–42; plus one test-file comment mentioning the type in a rejection list (`tests/ownership/move_borrowing_span_slot_871_test.cpp`).

Items 6–8 are the recurring family tax this RFC's sibling findings care about: every rule that walks span types — move exclusions, iteration desugarings, interface tables — must name each member. Six names in every list, forever, versus five.

## 3. Recorded decisions engaged

This removal, unlike 0035/0036/0037's subjects, targets a **recorded decision** — v0.106 added the type deliberately, with rationale (§5.15 and the v0.106 changelog): ASCII strings are bit-identical to UTF-8, so a validated byte view avoids `GetBytes`' per-call allocation for FFI/I-O paths. Honesty requires confronting that record on its own terms rather than around it:

- **The v0.106 motivation's FFI half never materialized.** Spec §32.5's FFI-compatible-type list excludes records categorically ("Not allowed: … classes, **records**, interfaces …"). A `ReadOnlyAsciiByteSpan` cannot cross the boundary; the actual boundary carriers for string bytes are `CString.From(s)` / `CString.FromBytes` / `RawBytes` (stdlib §10) — all allocating-copying by design, for lifetime reasons the FFI chapter records. Nor does an indirect path exist: `CString.From(s)` takes the string, `CString.FromBytes(bytes: Array<byte>)` takes an array — never a span. The span could only ever feed a *Bezel-side* loop that copied bytes somewhere anyway. The honest home for future zero-copy foreign-memory views is proposal 0033's `Ptr<T>`/`RawBytes` work, where lifetime and layout are being designed as a unit.
- **The v0.106 motivation's I/O half has been overtaken by v0.165.** `Encoding` now carries `GetByteCount(s): int` and `GetBytes(s, destination: Span<byte>): int` (#1355) — encode into a caller-owned, reusable `OwningBuffer<byte>` with **zero per-call allocation**, for **every** string, not only ASCII ones, with no conditional-on-storage caveat. The remaining delta between the retired view and the v0.165 path is one O(n) copy into a warm buffer — and the view's own O(1) claim was conditional (§1). The premise the type was built on has eroded in normative text, which makes this a changed-circumstances retirement, not a re-litigation of v0.106 as decided.
- **The v0.104/v0.107 separate-type architecture decision stands and is not reopened** (§7). Retiring one member is an exercise *within* that architecture.
- **The v0.165 naming ruling (#1359)** uses the method name as one of two exemplars; the ruling itself is untouched (exemplar deleted from the pair, §2 item 4).
- **No decline record blocks this.** DECLINED.md and the rationale's decline records contain nothing adjacent; #971 defers *implementation*, it does not rule on retention.

## 4. The case for retirement

### 4.1 Nothing builds on it

Verified (and re-verified exhaustively by the critique pass): no stdlib API takes or returns the type except its own §3.4/§9.6 surface. The one cross-type bridge — `SequenceEqual(other: ReadOnlySpan<byte>)` — is declared **on the ascii span itself**, one-directionally ("There is no corresponding overload on `ReadOnlySpan<byte>`", §3.4), so its deletion strands no other type's surface. Proposals 0009 (Base64/hex) and 0029 (checksums) accept `IReadOnlySpan<byte>` and list the ascii span only as one of several implementors; both compile unchanged with the list one shorter (a one-phrase trim in each, §6). Proposal 0027 builds on `ReadOnlyStringSpan` — the *char* view — not on this type.

### 4.2 It is unimplemented, so deletion strands nothing

#971 (open) records the entire construction surface as "Not started"; grep confirms the five src/ mentions are classification-list entries (borrow-bearing-record and string-backed-span name lists), not behavior. The conformance case is already skipped as blocked. This is the same species of evidence as the sibling RFC-0039's #1307: **normative text no program has ever run under** — spec debt whose deletion converts a deferred implementation obligation into nothing.

### 4.3 Deletion fixes a documented hazard

The unchecked public constructor (§1) is the stdlib's only entry point documented as silently corrupting data on misuse (verified: no other construction surface in the stdlib carries comparable hazard language; other span constructors mis-used fail *loudly* — bounds-checked indexers throw). Any retention plan owes a fix (a checking constructor, or a spec change making the constructor internal-equivalent — which Bezel's visibility model would need new vocabulary for); retirement owes nothing.

### 4.4 What each of its jobs is served by instead

| Use case | Today (with the type) | After retirement |
|---|---|---|
| One-shot UTF-8 bytes from a string (any content) | `Encoding.UTF8.GetBytes(s)` — the §3.4-documented fallback | unchanged — `GetBytes(s): owned Array<byte>`, always correct, one allocation |
| Hot-loop / repeated encoding without per-call allocation | `s.AsAsciiByteSpan()` — ASCII-only, zero-copy *conditionally* | `Encoding.UTF8.GetBytes(s, buf.AsSpan())` into a reused `OwningBuffer<byte>` (v0.165, #1355) — all strings, unconditional, zero per-call allocation |
| Feeding a byte-span-consuming API (`IReadOnlySpan<byte>`) — of which the **only one in today's stdlib is `SequenceEqual`** (verified by exhaustive enumeration; nothing else in the stdlib consumes `IReadOnlySpan<byte>`) | pass the ascii span | `bytes.AsReadOnlySpan()` on the `Array<byte>`, or `buf.AsReadOnlySpan()` on the reused buffer |
| FFI boundary | **never worked** — records are not FFI-compatible (§32.5); real path is `CString`/`RawBytes` | unchanged — `CString.From(s)` / `FromBytes`; future zero-copy foreign views belong to proposal 0033 |
| "Does this ASCII header start with these bytes" | ascii span + `SequenceEqual` | compare via `ReadOnlyStringSpan`/string equality on the char side, or `GetBytes` once and compare byte spans — or re-add a validated view later if evidence demands (§5) |

## 5. What is genuinely lost, honestly

One thing: the **O(1)-setup, allocation-free, borrowed byte view of an ASCII string's own storage**. Every replacement in §4.4 that produces bytes performs one O(n) copy (into a fresh array or a reused buffer). For a program that repeatedly scans the bytes of large, known-ASCII, long-lived strings without wanting even an amortized buffer, the retired type was the only tool — with the caveats that (a) the zero-copy property was implementation-conditional, (b) per-access reads were width-dispatched in the general case, and (c) no such program exists in the corpus, the guides, the proposals, or the test suite. Re-adding a *validated-only* view later (construction APIs only; no public raw constructor) is a purely additive stdlib change if demand materializes — deletion is reversible; the unchecked constructor's hazard and the six-way family tax, once shipped to users, would not have been.

## 6. Edit list

- **Stdlib**: delete §3.4 (tombstone the section number rather than renumbering — §3.3 and §4.x are externally cited; same convention question as RFC-0037 Q3); §9.6: delete the two method declarations and rewrite the byte-view prose passage to route through `Encoding.UTF8.GetBytes` (both forms — allocating and v0.165 span-destination); §4.16: six→five in the enumeration, drop the table row, retarget the `SequenceEqual`-interface example to a `ReadOnlySpan<byte>`/`ReadOnlyArraySpan<byte>` pair; §1 conventions: delete `AsAsciiByteSpan` from the exemplar pair (`IeeeEquals` remains).
- **Spec**: §4.6: replace the byte-view passage with the two-form `GetBytes` routing (keep `IsAscii`, which is independent); §28.3: drop the name from the producer-table exclusion row; §28.11: drop the name from the move-exclusion sentence; §29.2: delete the desugaring paragraph; D.2: "stdlib §3.3–3.4" → "stdlib §3.3".
- **Rationale**: living sections — §5.15: the "ASCII byte spans (v0.106)" design-story block (the section's ~9-line account of the type, its construction validation, and the no-byte-buffer-accessors decision) is replaced by a two-sentence retirement note pointing at this decision's record; the six→five family accounting, the CopyTo symmetry-table row, and the "three-way symmetry across the standard collection sources" sentence adjust accordingly (string retains its char view; the byte views live on the buffer/array sources); §15: adjust the byte-level-view sentence to the `GetBytes` routing; §18: adjust the spans-over-additional-sources inventory sentence. Add this decision's record (changed-circumstances retirement of v0.106; cite v0.165 #1355 and §32.5). Changelogs untouched (append-only history).
- **Proposals**: 0009 §"Rationale for input" and 0029 §"Input" — drop the implementor mention (one phrase each; both accepted proposals, edits ride their next touch).
- **Compiler**: remove the five classification-list mentions and the one deferral comment; nothing else exists (verified).
- **Conformance/tests** (per #1380 the committed cases are hand-maintained and do **not** regenerate when the docs are edited — each item is a hand edit): delete case `stdlib_0032.bzl` + its manifest row (358); **trim the two `AsAsciiByteSpan`/`TryAsAsciiByteSpan` method lines (and their comment lines) from `stdlib_0107.bzl`** (manifest row 433, `96-String`, `skip:signature`) — or fold that trim into the next §9.6-touching regen; adjust the one rejection-list comment in `tests/ownership/move_borrowing_span_slot_871_test.cpp`.
- **Issues**: #971 — item 3 (the `AsAsciiByteSpan` family) closes as retired; items **1, 2, and 4** (`ReadOnlyStringSpan` search methods, for-in iteration, and the `IReadOnlySpan<char>`-as-interface-param limitation, #970) remain open and unaffected.

## 7. Scope limits (stated so they stay stated)

- **`ReadOnlyStringSpan` is not touched.** It is the char view any string admits, proposal 0027 (SplitSpans) builds on it, and #971's live items concern it.
- **The `ArraySpan<T>`/`ReadOnlyArraySpan<T>` pair is not touched.** The v0.165 #1358 byte-accessor mirror makes them the live file→binary-parse path.
- **Family-wide source-polymorphic unification is not proposed.** The v0.104/v0.107 recorded reasoning (source polymorphism would break the record shape or demand a general mechanism) stands; this RFC operates within it.
- **No new validated-view replacement is designed here.** §5 records the re-addition path as future-additive; designing it now would rebuild the tax this RFC removes.

## 8. Sizing

| Piece | Size |
|---|---|
| Stdlib edit | Small — one section deleted, three sections' passages adjusted, one exemplar deleted |
| Spec edit | Small — four passages/paragraphs + one D.2 range |
| Rationale edit | Small — three living-section adjustments + one decision record |
| Proposals | Trivial — two one-phrase trims |
| Compiler | Trivial, net-negative — six comment/name-list deletions; no functional code exists |
| Conformance/tests | Trivial — one skipped case deleted + manifest row, two method lines trimmed from a second case, one comment adjusted |
| Migration risk | None identified — no consumer exists in any document or any test that executes |

## 9. Owner questions

1. **Q1 — the ruling**: retire the type? Recommended: **yes** — the v0.106 premise has eroded in normative text (v0.165 buffer-destination encoding; §32.5's record exclusion), nothing consumes it, nothing implements it, and it carries the stdlib's one documented silently-unsafe constructor.
2. **Q2 — section tombstone vs renumber** (stdlib §3.4): recommended tombstone, matching RFC-0037 Q3's reasoning.
3. **Q3 — the §4.6/§9.6 replacement prose**: recommend explicitly teaching the v0.165 span-destination form as the allocation-conscious path (it is currently easy to miss next to the classic `GetBytes(s)`).
4. **Q4 — re-addition trigger**: record in the rationale that a future validated byte view (no public unchecked constructor) is the sanctioned shape if profiling evidence ever demands one, so the door is documented rather than re-argued.

## Recommendation

**Adopt** — delete stdlib §3.4 and the `AsAsciiByteSpan` surface with the full tendril list of §6, route the use cases per §4.4, close #971's third item as retired, and record the decision as a changed-circumstances retirement of v0.106.

**Confidence: high.** The evidence base is unusually clean for a removal: the type is unimplemented (so no behavior changes anywhere), unconsumed (verified across stdlib, spec, guides, all 36 proposals, and the test corpus — and re-verified exhaustively by the independent critique pass, which found the counterexample hunt empty by full enumeration), self-documentedly conditional in its headline benefit, and superseded in its main use case by a landed v0.165 API. The one verified correction from adversarial review (five src/ name-list mentions, not zero) is incorporated and does not weaken the thrust — the mentions are deletions, not migrations. The single genuine loss (§5) is narrow, evidence-free in the corpus, and reversible additively.

---

## Finalization record (2026-07-11, phase 3)

Adversarial critique verdict: **SHIP** (0 blocking, 4 advisory). Disposition:

- **A1 (fixed)** — the second conformance case, `stdlib_0107.bzl` (manifest row 433, `96-String`, `skip:signature`), which declares the two `AsAsciiByteSpan` method lines, added to §2 item 12 and §6's conformance bullet with the #1380 hand-edit note.
- **A2 (fixed)** — "#971 items 1–2 survive" corrected to "items 1, 2, and 4" (item 4 is the `IReadOnlySpan<char>` interface-param limitation, #970) in the header and §6.
- **A3 (fixed)** — §2 item 4 and §6 now describe the §1 naming-exemplar edit as a **deletion** from the (`IeeeEquals`, `AsAsciiByteSpan`) pair, not a swap.
- **A4 (fixed)** — §4.4's byte-span-consumer row now states affirmatively that `SequenceEqual` is the *only* `IReadOnlySpan<byte>`-consuming API in today's stdlib (critique-verified by exhaustive enumeration), making the nothing-builds-on-it claim self-contained in the table.
- Cross-cutting: the tracking-issue cell now instructs the landing agent to file the issue and link #971.

No blocking items existed; no substantive position changed.
