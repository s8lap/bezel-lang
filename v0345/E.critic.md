# Lane E critic (v0.345)

**Verdict: ACCEPT-WITH-FIXES.** Three small fixes went into `Ework/E_build.py`. `E.json` was rebuilt from it: 19 edits, and all 19 apply clean at HEAD `b4243d87f`. I re-checked that `docs/` at that commit is identical to `e70dbfd8f`; the diff between them is README.md only. No decision is overturned. The originals are kept in `scratch/v0345/Ecrit/E.json.orig`, `E.md.orig` and `E_build.py.orig`.

**Scratch-tree checks.** I built a fresh `git init` tree at `scratch/v0345/Ecrit/tree` from `git archive HEAD docs scripts tests/conformance`, then applied the edits with `--write`, after my fixes.
- `check_docs.sh`: OK. The only advisory is `[spec] §4.12`. It is already at HEAD, and no edit's `new` text contains "§4.12".
- `check_proposals.sh`: OK.
- `corpus.py lint --strict`: 3343 cases, 0 problems.
- `corpus.py check`: up to date.
- Logs are in `Ecrit/check_docs.log` and `Ecrit/check_proposals.log`.

## Findings

### F1. #7161: `IndexOutOfRangeException` over the issue's `ArgumentOutOfRangeException` (upheld)
I checked the library's convention verbatim at HEAD. The grounds hold.
- **Stdlib §3.1:1408**: "`Insert` and `Remove` raise `IndexOutOfRangeException` when the index or the `(startIndex, length)` window falls outside `0..Length`". This is the `StringBuilder` twin of exactly these members.
- **§9.6 `IndexOf`**: "0 <= startIndex <= Length (IndexOutOfRangeException otherwise)".
- **§7.1 `Stream` window (:4954)**: "out-of-range raises `IndexOutOfRangeException`".
- **§4.14 `Slice(r)`**: normalization raises `IndexOutOf…`.
- **Spec §4.6**: `s[i]` and `s[r]` both raise `IndexOutOfRangeException`.
- **Compiler**: `LlvmBackend_String.cpp:1971-1991` raises it for all four conditions (fix #747). Its comment calls this "the same convention every" bound uses.

`ArgumentOutOfRangeException` appears only for:
- constructor magnitudes (stdlib §1 *Negative magnitudes fault*, #1396, which is scoped to "Every **constructor** parameter that states a magnitude");
- `Repeat` count and Pad width;
- `Random.Next` bounds;
- the bit-view `count`.

None of these is a window position. Stdlib §5's `Take`/`Skip` sentence also reads the #1396 rule as not reaching range operations.

- **Gap (fixed).** The DECLINED row did not answer the obvious objection that a negative `length` is a "magnitude" under #1396. I added that clause to the row and to the md.
- **The fold (upheld).** `Remove` and `Insert` get the same answer.
  - #7172's own body already cites §3.1 and asks for `IndexOutOfRangeException`.
  - #7108 asks for a §9.6 contract.
  - Both are open `bug` trackers, so the fold is small and clearly decided.
  - #7221's body says #7172 was "picked" somewhere. No commit in this clone carries it, so the lane's "Insert clamps" (read from HEAD source, `:2497`) stands for HEAD.
- **Reach (re-measured; zero flips).**
  - `stdlib_0147` (doc corpus, §9.6 block) is `skip:signature`.
  - No rule-corpus case mentions `Substring`. `l9_6_0019` and `s7_5_0018` are absent, as the lane says.
  - The gtest `string.Insert`/`Remove` uses are in range: `narrowed_nullable_builtin_int_arg_2614_test.cpp:164,178,220`.
  - `string_methods_test.cpp` pins `Substring` raising.
  - No test anywhere pairs `ArgumentOutOfRange` with `Substring`, `Remove` or `Insert`. The only grep hit is the doc-corpus copy of §9.6.

### F2. #7275's `char` arm: a code-point advance with no gap skipping (upheld as consistent with §6.6 and §16)
- **§6.6** gives `char` exactly one intrinsic advance. `char` targets admit `+=`/`-=` as "the code-point advance §17.6's `c++` and `c--` desugar to", and `c += 1` is `c := char(int(c) + 1)` with §5.2's code-point check.
- **§17.6** repeats this: "`c++` is `c := char(int(c) + 1)`".
- **§11.8's bridge** makes "intrinsic operations … the member bodies". So the lane's `char(long(value) + by)` is the derivation, not a free choice. `int(c) op long` is `long` under §6.2, so the `long(...)` spelling changes nothing.
- **§5.2:702** raises `OverflowException` for a surrogate or for a value past U+10FFFF.
- **§16.3:3147-3149's successor function** is defined for a range's *iteration*. §16.3:3110 says `Range<T>` is compiler-intrinsic and not a `where T: ISteppable<T>` clause, so iteration is not defined through `Step`.
- **DECLINED.md:662** (the deferred `Range<T>` widening) lists "`Range<char>`'s surrogate-gap stepping, a per-type rule with no constraint expression" as one of the questions a widening must answer. That confirms the two operations are separate today, and that a future widening, not this ruling, would have to reconcile them.
- **The #7262 supplement** left "steps across the gap or stops at it" open, and its own premise was that `char` "has no intrinsic arithmetic". Its §4.4 citation is right about operators, but §6.6's compound advance *is* an intrinsic operation, so the lane's reading answers the supplement's question rather than contradicting it.
- **Class.** Ruling (text silent, derivation from §11.8 + §6.6), with the owner flag surfaced. I agree.
- **Fix.** The paragraph's "`by` converting by §5.1" was a bare §-reference in the stdlib, where bare "§5.1" names *stdlib* §5.1 (no-selector aggregation). It is now "language spec [§5.1](…#sec-implicit-conversions)".
- **md fix.** In three places the worked value had been rendered as the literal U+D7FF character, which would garble on GitHub. It is now written `'퟿'`. The JSON was already correct.

### F3. #7040: the `OwningDictionary` order wording (upheld)
- The new sentence says the two views enumerate in "the one unspecified but deterministic order the *Note on iteration order* below describes". That Note is at stdlib :2326, after the paragraph at :2209, so "below" is right.
- It makes no order promise beyond the Note's determinism statement.
- The pairing is stated "over one state of the dictionary", so drain-and-rebuild reordering after a mutation is still allowed. "Reflect subsequent mutations" is kept.
- The §4.8 addition ("an indexer-set on a present key updates the entry in place and keeps its position") agrees with:
  - §4.20 `Counter` ("updates in place and preserves its position", "exactly as `Dictionary`");
  - §4.8's dictionary-literal "last occurrence wins".
- Only `Dictionary` and `OwningDictionary` declare `Keys`/`Values`.
- No test or corpus case depends on the order: `s29_2_0013` counts keys only.

### F4. Editorial items move no verdict (upheld, one fix)
- **#6778.** The ruling comment (2026-10-21) makes every `null` argument a compile error. The replacement text states exactly that.
  - All three "renders `null`" sites are replaced: stdlib §2.1, §7.4 and the writing reference.
  - No other doc states the old rule. Rationale §6.3 concerns interpolation and is untouched.
  - `TextWriter` declares no nullable `Write` overload.
  - On the writing reference: replacing a false comment line in place, with no pin, is within its maintenance rule.
- **#7048 (fixed).** The lane put the new clause *inside* "(a value expression, not a call)". Banked `s35_3_0009` (`pass`) quotes that parenthesis in its `rule:` line, so the quote would have gone stale. No tool checks quotes mechanically, so this would have failed silently. I re-anchored the insert after "for a payload case.", which leaves the parenthesis verbatim (it occurs once in the edited tree).
  - The sentence restates line 9979's existing bare-form rule ("a payload-free case written with an argument list, is the arity error §35.5 names on the pattern side") for the qualified form.
  - §35.5 itself states only the payload-bare direction. That gap was already present in line 9979 and is not this lane's doing.
- **#6874.** Consistent with §2.2's current-module shadowing sentence and with §8.1's ban, which covers `Object` only.
- **#6895.** The order "the binding, then the initializer's store" is right; the triage draft had it backwards. The verdict comes from §22.8's single-assignment rule, as the R2 critic upheld.
- **#6902.**
  - uint×int has no §5.1 conversion either way, so §6.2 gives no promotion.
  - A byte target *does* promote (`b += -1` stays a run-time checked write-back), and the sentence's "no §6.2 promotion against T" condition keeps that case out.
  - §4.2:19 confirms that the unsuffixed `-3000000000` has no standalone type.
  - No rule-corpus case writes a negative constant compound on an unsigned target.
- **#7143.**
  - §28.8.1 already says "a borrow is rejected, as it is at any store into an owned location ([§28.6])", so the bullet fills a reference that had nothing to point at.
  - §28.3 *State match on reassignment* already rejects a borrow at an owned-state LHS.
  - Note, not fixed: §28.3 derives the binding instance from K1.7 + **K3.11**, while the bullet's single tag says K1.7 + K2.3, which is the triage's tag. No verdict moves. The coordinator may want "(derives: K1.7 + K2.3; for a binding, §28.3's K1.7 + K3.11)".
- **#7282.** Consistent with §2.8.2's "Definition-site visibility" paragraph and with the R2 critic's upheld ruling.
- **#6956 lead-in and the writing reference's "eleven".** Both are right: the §2.19 list names 11 types.

### F5. Edits, anchors and DECLINED rows
- Every `old` occurs once.
- Each edit's `old` sits inside lane E's own sentences, except the shared points the md lists.
- All five DECLINED rows follow the `rejected` table's four-column shape and end with "Reopening bar: …".
- No proposal is needed. The README is untouched. No version headers or pins are touched.

## Fixes applied (all in `Ework/E_build.py`, then rebuilt)
1. **`E-7048`.** The insert moved after "for a payload case.", keeping the corpus-quoted parenthesis verbatim.
2. **`E-7275-step`.** The bare "§5.1" now links to language spec §5.1.
3. **`E-declined-rows`, #7161 row.** It gains the #1396-scope clause: constructor magnitudes, not a window's `length`.
4. **`E.md`.** The `'퟿'` spelling (three places), and the E-7048 row and corpus note updated. A #1396 sentence was added to #7161's grounds.

## Residual question to file
**Title:** `[stdlib-contract] String.From(buffer, start, length) and string.AsSpan(start[, length]) state no window bound (stdlib §9.6, v0.345)`

**Body:**
v0.345 gives `Substring`, `Remove` and `Insert` a window contract (§9.6, *The window members*, #7161): `IndexOutOfRangeException` outside `0..Length`. Two other §9.6 window members still state no bound:
- `String.From(buffer: Array<char>, start, length)`, a window over the array's `Count`;
- `AsSpan(start)` / `AsSpan(start, length)`.

§4.14's no-eager-check rule for span `(start, length)` construction may or may not be intended to reach `string.AsSpan`. Recommend:
- `From` raises `IndexOutOfRangeException` for a window outside `0..Count`, the §7.1 `Stream` window's rule;
- a ruling on whether `string.AsSpan(start, length)` checks eagerly, as `AsSpan(r)` does, or follows §4.14's deferred per-access check.

## Composition notes
- **E-6902 (§6.6) vs C3.** C3's #6887, #6897 and #6935 concern constant typing at range endpoints and case patterns, not §6.6. If C3 changes how a target-typed constant meets a non-promoting operand, re-read E-6902's sentence.
- **E-6874 (§2.3 Core-types row) vs C2 (#6764).** Same table row. E anchors on the Shadowable cell's tail.
- **E-6895 (§22.8)** shares the paragraph with C2's #6903 and C3's #6982. E's anchor is the *Assignment* sentence only.
- **E-7143 (§28.6 bullets, App. C.3) vs C1 (§28.6 *Who may call Dispose*, the §28 table).** The texts are disjoint.
- **DECLINED `rejected` group end.** Insert sequentially with the other lanes.
- **Owner-flag surfacing.** #7275 must be listed as owner-flagged and decided by delegation. If the owner prefers gap-skipping, the §2.19 `char` sentence and the fourth DECLINED row flip together.
