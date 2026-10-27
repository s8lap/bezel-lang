# Lane E — standard-library contracts and editorial (v0.345)

Issues: #7040, #7157, #7161, #6956 and #7275 (each a ruling), plus the editorial clarifications the triage collected: #6778 (stdlib §2.1, §7.4 and the writing reference), #7048 (§35.3), #6874 (§2.3), #6895 (§22.8), #6902 (§6.6), #7143 (§28.6 and Appendix C.3) and #7282 (§26.5). **No proposal.** None of the issues needs a design choice beyond the triage's recommendation. Reserved numbers: none.

**#7275 was labelled `needs-owner-decision` by the coordinator who filed it.** I decided it under the 2026-10-08 delegation, using the triage's recommendation (option (B), widened to every conformer). The coordinator should surface it to the owner as an owner-flagged item that was decided by delegation.

Two items are folded in:
- **`String.Remove` and `String.Insert` window bounds.** These are #7161's siblings. Their compiler trackers #7108 and #7172 were already open and waiting on this text. They join `Substring`'s contract paragraph.
- **The writing reference's "ten numeric primitives".** There are eleven.

#7121's editorial (§2.7 "Nested types … are not reordered") is **C2's**: the brief gives C2 that sentence. I made no edit for it.

**Deliverables.**
- `scratch/v0345/E.json`: 19 edits over 4 files, built by `scratch/v0345/Ework/E_build.py`. They cover the stdlib (9), the spec (7), the writing reference (2) and DECLINED.md (one edit carrying 5 `rejected` rows).
- **Dry run** at HEAD `b4243d87f` (spec v0.344; its `docs/` is identical to `e70dbfd8f`, which differs only in README.md): `python3 scripts/apply_doc_edits.py scratch/v0345/E.json` reports **19/19 edits apply clean across 4 files**.
- **Scratch-tree checks.** I applied the edits with `--write` to a `git init`-ed scratch tree (`scratch/v0345/Ework/tree`, from `git archive HEAD docs scripts tests/conformance`).
  - `scripts/check_docs.sh` says **OK**. Its one advisory, `CITATION [spec] §4.12`, is already present at HEAD.
  - `scripts/check_proposals.sh` says **OK**.
  - Logs: `Ework/check_docs.log` and `Ework/check_proposals.log`.
- The DECLINED index-size line is untouched. The coordinator moves it (+5 `rejected`).

## Shared text (for the composer)
- **DECLINED.md `rejected` group end.** `E-declined-rows` anchors on the last `rejected` row's tail plus `\n\n### \`deferred\``. Any other lane adding a `rejected` row anchors on the same point, so re-anchor by sequential insertion.
- **Spec §2.3 Core-types cell** (`E-6874`). C2 (#6764, GetType/ReferenceEquals on value kinds and interfaces) might edit the same table row. My `old` is the cell's last clause up to the closing `|`.
- **Spec §22.8 single-assignment paragraph** (`E-6895`). C2 (#6903) and C3 (#6982) both rule on §22.8. My `old` is the *Assignment* definition sentence only.
- **Spec §6.6 typing paragraph** (`E-6902`). C3's #6887/#6935/#6897 concern §6.2 promotion and constant typing. My sentence restates §5.1/§4.2 as they stand at HEAD. If C3 changes how a constant operand is typed against an operand pair, re-read my sentence.
- **Spec §28.6 *Leak prevention* bullets and Appendix C.3** (`E-7143-*`). C1 (the disposal surface) edits §28.6 *Who may call `Dispose()`* and the §28 summary table, not these bullets. Each of my inserts anchors on the end of the fresh-store bullet or row.
- **Stdlib §2.19** (`E-6956-leadin`, `E-7275-comment`, `E-7275-step`) and **§9.6** (`E-7157-pad`, `E-7161-window`): no other lane is named there.

## Draft changelog bullets (house style)
- **Standard-library contracts (lane `E`).**
  - **stdlib §4.8, §4.9: the `Keys`/`Values` views enumerate in the dictionary's own order (ruling; [#7040](https://github.com/s8lap/bezel-lang/issues/7040)).**
    - For `Dictionary<K, V>` that is insertion order. An indexer-set on a present key keeps the key's position.
    - For `OwningDictionary<K, V>` it is the unspecified but deterministic order of §4.9's *Note on iteration order*.
    - Over one state of the dictionary, the n-th key and the n-th value belong to the same entry.
    - **Declined:** leaving the views' order unspecified (a `rejected` row).
  - **stdlib §9.6: which side `PadLeft` and `PadRight` pad on (ruling; [#7157](https://github.com/s8lap/bezel-lang/issues/7157)).** `PadLeft` prepends `paddingChar` and `PadRight` appends it until the result's `Length` is `totalWidth`: `"ab".PadLeft(5, '*')` is `"***ab"`. The compiler already conforms.
  - **stdlib §9.6: the window members `Substring`, `Remove` and `Insert` (ruling; [#7161](https://github.com/s8lap/bezel-lang/issues/7161), with `Remove` and `Insert` folded in from the open trackers [#7108](https://github.com/s8lap/bezel-lang/issues/7108) and [#7172](https://github.com/s8lap/bezel-lang/issues/7172)).**
    - Positions and lengths count code points.
    - `Substring(i, n)` is the `n` code points from `i`. `Substring(i)` is the rest of the receiver from `i`.
    - `Remove` drops the same window, and `Insert(i, v)` inserts `v` at position `i`.
    - `i = Length` and `n = 0` are in range.
    - A negative position or length, a position above `Length`, or a window past the end raises `IndexOutOfRangeException`. That is `IndexOf`'s bound, `StringBuilder`'s and the slice `s[r]`'s.
    - **Declined:** `ArgumentOutOfRangeException` (a `rejected` row).
  - **stdlib §2.19: `char` is not an `INumber<T>` conformer (ruling; [#6956](https://github.com/s8lap/bezel-lang/issues/6956)).**
    - The lead-in had said that "the primitive numeric tower and `char`" conform to all three abstractions. That contradicted spec §11.8, §14.2 clause 5 and the section's own list.
    - The lead-in now gives `char` `ISteppable<char>` and `IParsable<char>` only.
    - **Declined:** making `char` an `INumber<T>` conformer (a `rejected` row).
  - **stdlib §2.19: `ISteppable<T>.Step` is checked as `T`'s own arithmetic is (ruling, owner-flagged and decided by delegation; [#7275](https://github.com/s8lap/bezel-lang/issues/7275)).**
    - At an integer `T`, a result that is not representable raises `OverflowException` (spec §6.1).
    - `decimal` follows §6.4. `float` and `double` follow §6.3's IEEE arithmetic and raise nothing.
    - `char` takes §6.6's code-point advance, `char(long(c) + by)`, so a result in the surrogate block or past U+10FFFF raises. It does not skip the gap the way `Range<char>` iteration does.
    - **Declined:** an implementation-defined (wrapping) `Step`, and a gap-skipping `char` `Step` (two `rejected` rows).
- **Editorial clarifications (lane `E`; no verdict moves).**
  - stdlib §2.1, §7.4 and the writing reference's `Console` comment ([#6778](https://github.com/s8lap/bezel-lang/issues/6778)). The sentence that a `null` argument to an `Object` overload "renders as the four characters `null`" described a case the non-nullable signatures cannot produce. It now says a `null` argument has no applicable overload, and points to the interpolation hole.
  - §35.3 ([#7048](https://github.com/s8lap/bezel-lang/issues/7048)): `FetchOutcome.NotModified()` is the arity error in every expression position.
  - §2.3 ([#6874](https://github.com/s8lap/bezel-lang/issues/6874)): a current-module declaration named `Type` shadows the core `Type`, and `System.Type` still reaches it.
  - §22.8 ([#6895](https://github.com/s8lap/bezel-lang/issues/6895)): binding a `static readonly` field to `ref`/`out` inside its own static initializer is two assignments.
  - §6.6 ([#6902](https://github.com/s8lap/bezel-lang/issues/6902)): an integer constant right operand with no §6.2 promotion against T is admitted only by constant narrowing at T (`u += -1` over `uint` is a compile error).
  - §28.6 and Appendix C.3 ([#7143](https://github.com/s8lap/bezel-lang/issues/7143)): a borrow stored into an owned location is an error.
  - §26.5 rule 2 ([#7282](https://github.com/s8lap/bezel-lang/issues/7282)): a call site in another module can select a less specific block when the more specific member is `internal`. Inside a carried generic body, visibility is the definition site's.
  - Writing reference: the numeric primitives that conform to all three §2.19 abstractions are eleven, not ten (folded).
- **Migration.**
  - *Newly rejected:* none at compile time. `Math.Sign('A')` was already a §14.2 constraint failure under spec §11.8 and §14.2 clause 5. The corrected stdlib sentence removes the contradicting one. The compiler half is tracked by #3850.
  - *Newly admitted:* none.
  - *Spec-ahead-of-impl:*
    - #7275: integer and `char` `Step` wrap today. This is tracked by #7262; the `char` arm is new to it.
    - #7161's `Remove` and `Insert` bounds: `Remove` reads past the end (#7108), and `Insert` clamps (#7172).
    - `Substring`, `PadLeft`/`PadRight` and `Keys`/`Values` already conform.
  - *Doc-only:* #6778, #7048, #6874, #6895 (the diagnostic stays on #6895), #6902 (the `-3000000000` form stays on #6902), #7143 and #7282.

---

## #7040 — the order of a dictionary's `Keys` and `Values`

**Decision.** I took reading 1, the issue's and the triage's recommendation. `Dictionary<K, V>.Keys` and `.Values` enumerate in the dictionary's insertion order. `OwningDictionary<K, V>`'s views share one unspecified but deterministic order, the one §4.9's *Note on iteration order* describes. Over one state of the dictionary, the n-th key and the n-th value belong to the same entry. §4.8 also gains the fact that an indexer-set on a present key updates the entry in place and keeps its position. §4.20's `Counter` already says this "exactly as `Dictionary`", and the issue's reading 1 assumes it. The pairing is stated per state, so the `OwningDictionary` drain-and-rebuild reordering after a mutation is not ruled out.

**Class: ruling.** The text was silent and this decides it. Nothing in the text admitted another order, so no admission narrows.

**Edits:** `E-7040-dict` (§4.8, after the remove-and-re-add sentence) and `E-7040-owning` (§4.9's `Keys`/`Values` paragraph). DECLINED: reading 2.

**Compiler facts.** The compiler conforms, read from source. `src/runtime/bezel_runtime_collections.cpp:1003-1008`: an `order[]` array of bucket indices in insertion order, shared by `OwningDictionary`. The key snapshot `bz_dict_keys_str` (`:1434-1466`) and the value snapshots `bz_dict_values_str`/`_gen` (`:1711-1750`) all walk `order[]`. An update does not re-append: only a fresh insertion records a slot (`:1352`).

**Reach.** No guide, rationale or tour text relies on the views' order. `tests/conformance/rules/cases/s29_2_0013.bzl` (banked `pass`) counts keys only and is unaffected. The writing reference's `// live views` comment is left as it is (maintenance rule: no appended clause).

**Corpus:** none affected. Harvestable: a `Keys`/`Values` pairing row and an update-keeps-position row.

**Tracker:** none.

**New questions:** none.

**Self-critique.**
- *Could "one state" be read as forbidding the views from reflecting mutations?* No. The same paragraph keeps "reflect subsequent mutations".
- *Does the `OwningDictionary` sentence make an order statement §4.9 refused?* No. It says the two views share the order the Note calls unspecified, and the Note's determinism statement still governs.

**Ruling comment (ready to post).**
> **Ruled (v0.345, delegated ruling): reading 1.**
> - `Dictionary<K, V>.Keys` and `.Values` enumerate in insertion order, the order of entry iteration.
> - `OwningDictionary<K, V>`'s two views share its one unspecified but deterministic order (§4.9, *Note on iteration order*).
> - Over one state of the dictionary, the n-th key and the n-th value belong to the same entry.
>
> Stdlib §4.8 also now says that an indexer-set on a present key updates in place and keeps its position. Leaving the views' order unspecified gets a `rejected` row in DECLINED.md. The compiler already conforms: every runtime view walks the insertion-order table. No compiler change is needed, so this closes when v0.345 lands.
>
> ---
> _Generated by [Claude Code](https://claude.ai/code)_

---

## #7157 — which side `PadLeft` and `PadRight` pad on

**Decision.** I took reading 1, the names' plain meaning. `PadLeft` prepends `paddingChar` and `PadRight` appends it until the result's `Length` is `totalWidth`. The sentence goes into the §9.6 Pad contract comment with the issue's two worked values (`"***ab"`, `"ab***"`). It sits beside the existing `totalWidth <= Length` and negative-width clauses and `PadCenter`'s example, so the side and the length are fixed together.

**Class: ruling.** It is narrow: no plausible alternative exists, but the corpus needed a sentence it can cite.

**Edit:** `E-7157-pad`, three comment lines at the Pad contract's 30-column indent. The applier's indent guard passes.

**Compiler facts.** It conforms. `src/codegen/LlvmBackend_String.cpp:2257-2276` passes `leftAlign = 1` for `PadRight` to `bz_str_pad`. `src/runtime/bezel_runtime_strings.cpp:84-120` puts the pad first for `PadLeft` and after for `PadRight`, with the result length `totalWidth`. The issue's MEASURED results (QUOTED): `***ab` and `ab***`. The negative-width divergence (no `ArgumentOutOfRangeException`) is already tracked by #7120.

**Reach.** Pins consistent with the ruling:
- `tests/codegen/string_length_codepoints_528_test.cpp:172-202`;
- `tests/codegen/interpolated_string_test.cpp:252,266`;
- `tests/codegen/stdlib_string_transform_methods_leak_2199_test.cpp:250,262`.

No pin flips.

**Corpus:** none affected. The two values are now harvestable.

**Tracker:** none.

**New questions:** none.

**Ruling comment (ready to post).**
> **Ruled (v0.345, delegated ruling): reading 1.** Stdlib §9.6's Pad contract now says: "PadLeft prepends and PadRight appends paddingChar until the result's Length is totalWidth: `"ab".PadLeft(5, '*') = "***ab"`, `"ab".PadRight(5, '*') = "ab***"`." That fixes the side and the result length together, so the corpus can bank both cases. The compiler already conforms (`bz_str_pad`), so this closes when v0.345 lands. The negative-width check stays on #7120.
>
> ---
> _Generated by [Claude Code](https://claude.ai/code)_

---

## #7161 — `Substring`'s contract (with `Remove` and `Insert` folded in)

**Decision.** Stdlib §9.6 gains a paragraph, ***The window members: `Substring`, `Remove` and `Insert`***.
- Positions and lengths count code points.
- `Substring(startIndex, length)` is the `length` code points from `startIndex`. `Substring(startIndex)` is the rest of the receiver, §1's to-the-end window.
- `Remove` yields the receiver without the same window. `Insert(startIndex, value)` inserts `value` at position `startIndex`.
- `startIndex = Length` and `length = 0` are in range.
- A negative `startIndex` or `length`, a `startIndex` above `Length`, or `startIndex + length > Length` raises **`IndexOutOfRangeException`**. That is the triage's reading (1), over the issue's draft `ArgumentOutOfRangeException`.

Grounds for `IndexOutOfRangeException`. It is what every window and index bound in the library and the language raises:
- `IndexOf`'s `startIndex` in this same class;
- `StringBuilder.Insert`/`Remove` (stdlib §3.1:1408);
- the `Stream` window (§7.1);
- the span `Range` overloads;
- spec §4.6's `s[i]` and `s[r]`.

The compiler's `Substring` already raises it (fix #747). `ArgumentOutOfRangeException` is the library's fault for a bad *count or width* (`Repeat`, the Pad contract), not for a window position. Stdlib §1's *Negative magnitudes fault* row (#1396) is scoped to constructor magnitudes, and §5's `Take`/`Skip` sentence reads it as not reaching range operations, so a window's negative `length` is not an ArgumentOutOfRange case either; the DECLINED row now says so (critic fix).

**Fold.** `String.Remove` and `String.Insert` had the same gap: §9.6 declares them with no contract. #7108's body asks for exactly this sentence ("stdlib §9.6 needs a range contract for `Remove`"), and #7108 and #7172 are open compiler trackers that rely on it. The answer is the same as `Substring`'s, so the fold is small and decided.

**Class: ruling.** The out-of-range half was undecided. The in-range half was derivable from stdlib §1's window rows, as the triage notes, and is now stated directly.

**Edit:** `E-7161-window`, a new paragraph before ***`Split` contract***. DECLINED: `ArgumentOutOfRangeException`.

**Compiler facts.**
- `Substring` conforms. `src/codegen/LlvmBackend_String.cpp:1962-1995` (fix #747) guards `start < 0`, `start > len`, `length < 0` and `start + length > len`, then raises `IndexOutOfRangeException`. The runtime's clamp (`bezel_runtime_strings.cpp:646-665`) is only a backstop.
- `Insert` **clamps** (`LlvmBackend_String.cpp:2497`, "Clamp index to [0, selfLen]"). This is tracked by #7172.
- `Remove` has **no bounds check** (`:2546` onward), and the out-of-bounds read is tracked by #7108.
- The `byte`-argument sign-extension is #7102, a separate issue.

**Reach.**
- Consistent pin: `tests/codegen/string_methods_test.cpp:125-140` (`Substring(6)`/`(-1)`/`(3, 3)` raise).
- In-range rows: `stdlib_string_transform_methods_leak_2199_test.cpp:274-298`.
- No test pins `string.Insert`'s clamp or `Remove`'s over-read.
- (`tests/codegen/stringbuilder_methods_test.cpp:58-115` pins `StringBuilder` Insert/Remove out-of-range as a no-op. That contradicts §3.1's existing sentence, not this edit, and is already tracked by #7210 and #7221.)
- The issue's corpus rows `l9_6_0019` and `s7_5_0018` are not in `tests/conformance/rules/` at HEAD (never banked), so there is nothing to edit.

**Trackers owed:** none new. Notes for existing trackers:
- **#7108:** the text now states `Remove`'s window bound, including a negative `length`.
- **#7172:** `Insert` raises for `startIndex < 0` or `> Length`.

**New questions:** none to file.

**Self-critique.**
- *Does `Insert` at `Length` read as out of range?* No. The paragraph names it in range (`"abc".Insert(3, "d")` is `"abcd"`).
- *Could `startIndex + length` overflow `int`?* The bound is stated mathematically, and the compiler compares in `i64`.
- *Is a negative `length` with `startIndex = Length` in range?* No. It is negative, so it raises, which matches the codegen's `lenNeg`.
- *Does the paragraph contradict the §1 window row?* No. It cites it.

**Ruling comment (ready to post).**
> **Ruled (v0.345, delegated ruling).** Stdlib §9.6 gains *The window members: `Substring`, `Remove` and `Insert`*:
> - Positions and lengths count code points.
> - `Substring(i, n)` is the `n` code points from `i`, and `Substring(i)` is the rest from `i`.
> - `Remove` drops that window, and `Insert(i, v)` inserts `v` at position `i`.
> - `i = Length` and `n = 0` are in range.
> - A negative position or length, a position above `Length`, or a window past the end raises **`IndexOutOfRangeException`**.
>
> I chose `IndexOutOfRangeException` over the draft's `ArgumentOutOfRangeException` because every other window and index bound in the library and the language raises it (`IndexOf`, `StringBuilder.Insert`/`Remove`, `s[r]`, the `Stream` window), and the compiler's `Substring` already does (fix #747). The alternative gets a `rejected` row. `Remove` and `Insert` were folded in: their bounds were equally unstated, and #7108 (Remove's over-read) and #7172 (Insert's clamp) now have text to implement. `Substring` needs no change, so this closes when v0.345 lands.
>
> ---
> _Generated by [Claude Code](https://claude.ai/code)_

---

## #6956 — the stdlib §2.19 lead-in on `char`

**Decision.** The spec reading, as recommended by the issue, the triage and the critic. `char` conforms to `ISteppable<char>` and `IParsable<char>` only. The lead-in now reads: "The primitive numeric tower conforms to the first three, and `char` to `ISteppable<char>` and `IParsable<char>` only, via compiler-provided conformances (the list below; `char` is not an `INumber<T>` conformer, #6956)".

The other side of the contradiction already agrees with this reading:
- spec §11.8;
- spec §14.2 clause 5, which is a primitive's conformance source;
- stdlib §2.19's own list;
- `Char`'s declaration (§9.5);
- the writing reference §5 (`char conforms to ISteppable/IParsable`);
- the closed rulings #1782 and #4730.

**Class: ruling.** It resolves a contradiction between two normative statements (process §2). It is not a design change: it keeps the rule that the spec, §14.2 and the list state.

**Edit:** `E-6956-leadin`. DECLINED: `char` as an `INumber<T>` conformer. No row existed before. The ground recorded is §6.6/§3.5's "advanced, never scaled".

**Compiler facts.** Sema rejects `S<char>` under `where T: INumber<T>` and accepts `Math.Sign('A')` (QUOTED from the issue, measured at `08ed8a2f`). `tests/codegen/sum_constraint_rejects_non_inumber_4730_test.cpp:97` pins the rejection side. The acceptance is #3850's catch-up (OPEN, `spec-tracking`).

**Reach:** no in-tree program calls `Math.Sign` on a `char` (grep over tests/ and docs/).

**Corpus:** none affected.

**Tracker:** none new (#3850).

**New questions (folded):** the writing reference's "the ten numeric primitives conform to all three" is wrong. The list has eleven: `byte`…`ulong`, `float`, `double`, `decimal`. `E-cg-eleven` corrects it.

**Ruling comment (ready to post).**
> **Ruled (v0.345, delegated ruling): the spec reading.** `char` conforms to `ISteppable<char>` and `IParsable<char>` only. Stdlib §2.19's lead-in now says so, matching spec §11.8, §14.2 clause 5, the section's own conformance list and the closed rulings #1782 and #4730. Making `char` an `INumber<T>` conformer gets a `rejected` row: a code point is advanced, never scaled (§6.6). The compiler half (`Math.Sign('A')` must be the same §14.2 rejection as `S<char>`) stays on #3850. This issue closes when v0.345 lands.
>
> ---
> _Generated by [Claude Code](https://claude.ai/code)_

---

## #7275 — `ISteppable<T>.Step`'s overflow (owner-flagged)

**Owner flag.** The coordinator who filed this issue labelled it `needs-owner-decision`. I decided it under the delegation, using the triage's recommendation (option (B), widened). The coordinator should list it among the decisions the owner sees.

**Decision.** I chose option (B): the rule is stated on `Step` in stdlib §2.19 (a pointer in the declaration's comment and a ***`Step`*** paragraph after the conformance list), covering every conformer:
- **integer `T`:** the result is the mathematical `value + by`, whatever `by`'s sign. A result not representable in `T` raises `OverflowException` (§6.1).
- **`decimal`:** `value + by` under §6.4, raising on overflow.
- **`float` / `double`:** `value + by` under §6.3's IEEE arithmetic (`by` converts by §5.1 and may round). Nothing is raised.
- **`char`:** the code-point advance §6.6 gives `c += n`, which is `char(long(value) + by)`. A result outside U+0000–U+10FFFF, or in the surrogate block, raises `OverflowException` (§5.2). It does not step across the surrogate gap the way `Range<char>` iteration does.
- **a user conformer:** its declared body.

Worked values: `Step(1, 4_000_000_000L)` at `int` raises, `Step('a', 1)` is `'b'`, and `Step('\uD7FF', 1)` raises.

**Why (B) over (A).**
- (A) extends §6.1's normative sentence to "an arithmetic operation performed by a standard-library member". That is a new general rule with a fuzzy edge: which members?
- (A) also leaves the non-integer conformers open.
- (B) is where a reader of `Step` looks, adds no general rule, and covers all eleven primitives and `char`.

Option (C) (implementation-defined, today's wrap) is rejected and gets a row.

**The `char` arm.** This is the one point beyond the issue's text. The #7262 supplement flagged it as open ("whether `Step` steps across the gap or stops at it"). The triage recommended §5.2's code-point check, and I follow it.
- Spec §11.8 makes a primitive's witness "its intrinsic operations". `char`'s intrinsic advance is §6.6's and §17.6's `c += n` / `c++`, defined as `char(int(c) + n)` with the §5.2 check. That check raises at a surrogate.
- §16.3's gap-skipping *successor* belongs to a range's iteration. An iteration may skip and stop; a call must return a value. That is the #7262 ruling's own distinction.
- The gap-skipping alternative gets a `rejected` row whose reopening bar is a change to `c += n` itself.

This is a ruling on the triage's recommendation, not a new design choice. If the owner prefers `Step` to match `Range<char>` iteration, the paragraph's `char` sentence and the row flip together.

**Class: ruling.** The text was silent. §6.1 names operators, and §2.19's comment names no check. The ruling narrows no stated admission and contradicts no recorded decision; it agrees with the #7262 coordinator ruling.

**Edits:** `E-7275-comment` (the `Step` declaration's comment gains "checked as T's own arithmetic is (see Step below)"; the second line is aligned at the comment column) and `E-7275-step` (the paragraph). DECLINED: two rows.

**Compiler facts.** `src/codegen/LlvmBackend_Calls2.cpp:763-801` is the only `Step` lowering:
- `decimal`: `bz_decimal_add` with the `OverflowException` vtable, so it conforms.
- floating point: `SIToFP` then `FAdd`, so it conforms.
- **integers and `char`:** `CreateAdd` "at the value's own width (wrapping, like `+`)". It does not conform. This is #7262's defect (QUOTED there: `-294967295`).
- For `char` it also produces surrogate and out-of-range code points with no fault. That is a soundness-adjacent invalid-`char` value, new to #7262's scope.
- The issue body says `Average` never calls `Step` (QUOTED from the #7262 ruling), and I confirmed by grep that stdlib §5's `Step` mention is rationale only.

**Reach.**
- No gtest, lit or corpus row pins a wrap.
- `tests/codegen/static_interface_dispatch_807_test.cpp:359-382` (`'a' + 2 = 'c'`) is in range and unaffected.
- `tests/lit/sema/static_iface_conformance_807__char_steppable_not_inumber_ok.bzl` is unaffected.
- `tests/conformance/rules/cases/l2_19_0002.bzl` (banked `pass`, `Advance('a') = 'b'`) is unaffected.

**Corpus:** none flips. Harvestable once #7262 lands: an integer overflow row and a `char` surrogate row (expected `fail` until then).

**Tracker:** none new. The fix lane is #7262. A note for it (below) adds the `char` arm: a result in U+D800–U+DFFF or outside U+0000–U+10FFFF must raise `OverflowException`, and it must be pinned at `'\uD7FF' + 1` and at `'\U0010FFFF' + 1`.

**New questions:** none.

**Self-critique.**
- *At `ulong` with negative `by`?* The mathematical sum is the test, so `Step(0UL, -1)` raises. That is right.
- *At `float` with a huge `by`?* It rounds by §5.1's float conversion, which is stated.
- *Could `long(value) + by` overflow `long` at `char`?* Only for `by` near `long.MaxValue`, and that raises `OverflowException` anyway, so the outcome is the same.
- *Does the paragraph touch §16.3?* No. It names §16.3 only to say it is a different operation.

**Ruling comment (ready to post).**
> **Ruled (v0.345, delegated ruling): option (B), widened to every conformer.** This issue was labelled `needs-owner-decision`; it is decided under the 2026-10-08 delegation and listed for the owner's review. Stdlib §2.19 gains a ***`Step`*** paragraph, and the declaration's comment points to it.
> - **Integer `T`:** the result is the mathematical `value + by`. A result not representable in `T` raises `OverflowException` (spec §6.1).
> - **`decimal`:** §6.4, raising on overflow.
> - **`float`/`double`:** §6.3's IEEE arithmetic, with no exception.
> - **`char`:** §6.6's code-point advance, `char(long(value) + by)`. A result in the surrogate block or past U+10FFFF raises `OverflowException` (§5.2). It does **not** skip the surrogate gap the way `Range<char>` iteration does (§16.3): that successor function is an iteration's, not this member's.
>
> I chose (B) over (A) because (A) would add a general §6.1 rule about "library members" and still leave the non-integer conformers open. Rejected rows record the silent wrap (C) and a gap-skipping `char` `Step`. No compiler change follows beyond #7262's fix lane, which now also covers the `char` arm. This issue closes when v0.345 lands.
>
> ---
> _Generated by [Claude Code](https://claude.ai/code)_

**Note for #7262 (ready to post).**
> v0.345's stdlib §2.19 *`Step`* paragraph (#7275) states this issue's ruling as text. It adds the `char` arm: `Step` at `char` is `char(long(value) + by)` with §5.2's check, so a result in U+D800–U+DFFF or outside U+0000–U+10FFFF raises `OverflowException`. It does not skip the gap. The lowering at `src/codegen/LlvmBackend_Calls2.cpp:~794` wraps for `char` as it does for the integers, so it can produce an invalid `char` today. Please pin `T.Step('\uD7FF', 1)` and `T.Step('\U0010FFFF', 1)` (both raise) beside the integer rows. `decimal` and `float`/`double` already conform.
>
> ---
> _Generated by [Claude Code](https://claude.ai/code)_

---

## Editorial clarifications (no verdict moves)

| id | issue | where | what | compiler |
|---|---|---|---|---|
| `E-6778-console`, `E-6778-textwriter`, `E-6778-cg` | #6778 (closed; fixed by `14870d0fd`) | stdlib §2.1, §7.4; writing reference §5.2 comment | The "`null` argument renders `null`" sentence (unreachable with non-nullable parameters) becomes: no overload takes a nullable parameter, so a `null` argument is a compile error (§5.1, §22.6); print a maybe-absent value through a hole or after `??`/narrowing. The writing-reference comment line is replaced, not extended: it was false. | conforms (`14870d0fd`, test fix-forward `88909d420`) |
| `E-7048` | #7048 (closed; fixed by `0a891b3d7`) | §35.3 | After the payload-case example, the sentence gains "`FetchOutcome.NotModified()`, a payload-free case written with an argument list, is the arity error §35.5 names on the pattern side, in every expression position". The "(a value expression, not a call)" parenthesis is left verbatim, since banked `s35_3_0009` quotes it (critic fix). | conforms; corpus `s35_3_0009` banked `pass` (`735d320c7`) |
| `E-6874` | #6874 (closed) | §2.3 Core-types cell | A current-module declaration named `Type` shadows `Type` (§2.2); only `Object` is barred (§8.1); `System.Type` still reaches it. | conforms (QUOTED, the issue's probes) |
| `E-6895` | #6895 (open, `spec-tracking`) | §22.8 single-assignment | Binding a `static readonly` field to `ref`/`out` in its own static initializer is two assignments on one path, rejected by this rule, not by placement. I reworded the triage's draft: it called the initializer's store "the first", but the binding executes first. | verdict conforms; the diagnostic stays on #6895 |
| `E-6902` | #6902 (open, `spec-tracking`) | §6.6 typing paragraph | A constant right operand with no §6.2 promotion against T enters only by §5.1's constant narrowing at T: `u += -1` and `u += -3000000000` over `uint` are compile errors; `u += -3_000_000_000L` takes the checked write-back. | `-1` conforms (`bc6dd4bb3`); `-3000000000` stays on #6902 |
| `E-7143-bullet`, `E-7143-appc` | #7143 (closed) | §28.6 *Leak prevention*; Appendix C.3 | A borrow stored into an owned location (an `owned` field, an owning-collection slot, an owned-state binding) is an error (K1.7 + K2.3), the converse of the fresh-store bullet; §28.8.1 and *Methods returning `this`* are instances. Plus the matching Appendix C.3 row after the escape row. | conforms (QUOTED, `b7e0b6e7`) |
| `E-7282` | #7282 (open) | §26.5 rule 2 | A call site in another module can select a less specific block where the more specific block's member is `internal`; inside a carried public generic body, visibility is the definition site's (§2.8.2). | conforms for the ruled shape |

Class for every row: **clarification**, except #6778, which is **editorial** (it removes prose that no longer describes any reachable case). Each states a rule the posted ruling derived from text already present, and none moves a verdict. No guide, tour or rationale text restates any of them; I checked by grep for the changed phrases.

**Corpus (banked, list only):** `s35_3_0009` / `s35_3_0010` quote §35.3's v0.340 sentence. The quoted parenthesis still occurs verbatim (the critic moved the insert past it), and the new parenthesis gives `s35_3_0009` a sentence it can cite directly at a later re-cut. No verdict changes.

**Comments (ready to post), one per issue.**
- **#6778, #7048, #6874, #7143 (closed):**
  > The editorial sentence promised in this issue's ruling landed in v0.345 (*where*). No verdict moved.
  >
  > ---
  > _Generated by [Claude Code](https://claude.ai/code)_

  The coordinator fills in *where* from the table.
- **#6895:**
  > v0.345's §22.8 now says it directly: binding a `static readonly` field to a `ref`/`out` parameter inside its own static field initializer is two assignments on one path, rejected by the single-assignment rule, not by placement. This issue stays open for the diagnostic, which should name that rule.
  >
  > ---
  > _Generated by [Claude Code](https://claude.ai/code)_
- **#6902:**
  > v0.345's §6.6 now states the ruling: an integer constant right operand with no §6.2 promotion against T is admitted only by constant narrowing at T, so `u += -1` and `u += -3000000000` over `u: uint` are compile errors. This issue stays open as the compiler tracker for the `-3000000000` form.
  >
  > ---
  > _Generated by [Claude Code](https://claude.ai/code)_
- **#7282:**
  > v0.345's §26.5 rule 2 now states the ruling: a call site in another module can select a less specific block where the more specific block's member is `internal`, while visibility inside a carried public generic body is the definition site's (§2.8.2). The question in the ruling (confirm the repro's shape) still stands; once answered, this can close.
  >
  > ---
  > _Generated by [Claude Code](https://claude.ai/code)_
- **#7108 and #7172 (compiler trackers, folded text):**
  > v0.345's stdlib §9.6 *The window members: `Substring`, `Remove` and `Insert`* (#7161) states the contract this fix needs: a negative position or length, a position above `Length`, or a window past the end raises `IndexOutOfRangeException`, and `startIndex = Length` and `length = 0` are in range.
  >
  > ---
  > _Generated by [Claude Code](https://claude.ai/code)_

---

## Adversarial pass (whole lane)
- **Soundness escapes.**
  - #7275's `char` arm closes an invalid-`char` production that the compiler wrap allows today. That gap is now on #7262.
  - #7161's fold makes `Remove`'s over-read (#7108) a contract violation, not an unstated case.
  - No edit admits a program.
- **Over-rejection.** No edit rejects a program the text admitted. #6956's corrected lead-in removes a sentence that would have admitted `Math.Sign('A')`, but spec §11.8 and §14.2 clause 5, the rule a constraint check reads, already rejected it.
- **Every other passage stating the same rule.**
  - `Step`: stdlib §2.19's declaration comment and the new paragraph agree. The writing reference §5 lists the signature only. §16.3 is a different operation, as the paragraph says.
  - `char` conformance: spec §11.8, §14.2 clause 5, stdlib §2.19's list, §9.5's `Char`, and writing reference §5 (count corrected).
  - Dictionary order: §4.8, §4.9, §4.20 (`Counter`, which already says "updates in place and preserves its position", consistent).
  - Window bounds: stdlib §1 *Window parameters*, §3.1, §4.14's `Range` overloads, and spec §4.6.
  - `null` to `Console`: §2.1, §7.4 and the writing reference §5.2, all three edited. Spec §18.5's hole rendering is unchanged and still cited.
- **What I deliberately left undecided.**
  - The `StringBuilder` Insert/Remove no-op tests (`stringbuilder_methods_test.cpp:58-115`) contradict stdlib §3.1's existing text. That is #7210 and #7221's business, not a new question.
  - Whether `Range<char>`'s iteration is defined through `ISteppable<char>.Step`: it is not (§16.3 calls the restriction compiler-intrinsic), so the two may differ.

## Summary for the coordinator
| Issue | Class | Edits | Proposal | DECLINED | Tracker owed |
|---|---|---|---|---|---|
| #7040 | ruling | 2 | — | +1 `rejected` | no |
| #7157 | ruling | 1 | — | — | no (#7120 has the width check) |
| #7161 (+ `Remove`/`Insert` folded) | ruling | 1 | — | +1 `rejected` | no; note #7108, #7172 |
| #6956 | ruling (contradiction) | 1 (+1 folded writing-reference count) | — | +1 `rejected` | no (#3850) |
| #7275 (owner-flagged) | ruling | 2 | — | +2 `rejected` | no; note #7262 (`char` arm) |
| #6778 | editorial | 3 | — | — | no |
| #7048, #6874, #6895, #6902, #7143 (2), #7282 | clarification | 7 | — | — | no (#6895, #6902 remain the trackers) |

The 19 JSON edits are the 18 counted above (the folded `E-cg-eleven` included) plus `E-declined-rows`, one edit carrying all five DECLINED rows.

New questions: none to file. Two items are folded: the `String.Remove`/`Insert` window bounds and the writing reference's primitive count.
