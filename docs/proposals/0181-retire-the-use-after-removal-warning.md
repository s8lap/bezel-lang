# Proposal 0181 — Retire the element-borrow use-after-removal warning

| | |
|---|---|
| **Status** | graduated-v0.338 — superseded — graduated v0.338 (accepted 2026-10-13 as a delegated ruling under the owner's 2026-10-13 direction for the diet train ("Make a judgement on what should be retired based on how useful the feature is and how easy it is to write code differently to work around the absence of the feature") and the coordinator's retire/keep table, row D-D, option A, a §10 subtraction — §28.11's element-borrow use-after-removal warning (proposal 0129's lint B, v0.307) is retired: an owning-collection element borrow is valid until its slot is mutated and is not statically tracked per slot, the checked-mode backstop (§28.2) carries the hazard, and the remedies stay in the code-generation reference; lint A (§29.12) unchanged; critique round 1 applied (the deleted stdlib sentence located in §4.21 `OwningSlotMap<T>`, not §4.9; the code-generation §7.5 pair made runnable; the rationale's backstop contrast with lint A corrected; the `OwningBuffer<T>` `init` aside restored in §28.11's editorial pass; the tracker list completed); landed with the v0.338 changelog entry) |
| **Tracking issue** | [#4603](https://github.com/s8lap/bezel-lang/issues/4603) (open, `spec-tracking`: both lints of proposal 0129; its lint-B leg is withdrawn at landing and lint A, §29.12's mutation-during-iteration warning, stays). One `spec-tracking` note is filed at landing for the five banked corpus cases (§6.2) |
| **Target module(s)** | **spec**: §28.11 (*Owning collections*: the warning paragraph replaced by one sentence of posture and remedy), §28.3 (the producers table's indexer row: the parenthetical that named the warning), Appendix C.3 (the warning's bullet deleted). **stdlib**: §4.5 (`OwningArray<T>`, the indexer-getter sentence), §4.21 (`OwningSlotMap<T>`, *Ownership faces*: the producer-set/kill-set sentence deleted). **rationale**: §5.28 (one dated paragraph: *The fifth instance is retired*). **code-generation reference**: §4.5 (the owning-collection sentence), §4.6 (the diagnostics list), §7.5 (one *Looks right* pair, a run-time fault rather than a rejection). **proposals folder**: `DECLINED.md` (one `retired` row; the per-site-suppression row's lint count), `README.md` (this row). **No guide, grammar or Appendix A change.** |
| **Written against** | spec/stdlib **v0.337** at `e1a20aa44` (main). Every `old` anchor in `scratch/v0338/W2.json` (ids `W2/0181/…`) is the HEAD text; `python3 scripts/apply_doc_edits.py scratch/v0338/W2.json` dry-runs clean together with lane W2's editorial edits (42 edits, 6 files) |
| **Origin** | The spec diet review (`scratch/diet/D2.md`, §28.11's *simpler* candidate D-D: "a warning (no verdict), unimplemented, 3 test files"), the owner's 2026-10-13 direction, and the coordinator's judgement (`scratch/v0338/JUDGEMENT.md`: "soundness carried by the borrow rules and checked mode; lint-level advice belongs to tooling"). Decided by lane W2 of the v0.338 train. |

*Reference convention: unprefixed §-references are language-spec references at `e1a20aa44` (v0.337); "stdlib §" references are to the standard library companion; K tags are §28.3's kernel ids. Quotations are verbatim. Every program below uses only members the stdlib companion declares (`OwningArray<T>`: `Add`, the indexer, `RemoveAt`, `PopAt`, `Clear()`, stdlib §4.5). No invented API; ▲ marks proposed text. The former name of the language is not used anywhere in this document.*

---

## 0. Summary

Since v0.307 (proposal 0129, lint B) §28.11 *Owning collections* has said the compiler **must warn** when a local holding an element borrow of an owning collection — an indexer-get, a default-iteration variable, or a §29.11-traced terminal-accessor result — is used after a call on the **same collection binding** that disposes the borrowed element and **dominates the use on every path**: `Clear()`, `Remove(b)` of the borrow itself, or `RemoveAt(i)` at the same index expression with no §29.12 structural operation in between. The warning moves no verdict: "The program stays accepted". It is implemented nowhere (`src/` has no site; the two corpus cases that expect it are banked as `fail #4603`). Its definition is 390 words in §28.11 plus a clause in §28.3's producers table, an Appendix C.3 bullet, two stdlib sentences and two code-generation-reference mentions, and it carries two *chosen silences, liftable* beside it (the indexer-set at the same index; the keyed `OwningDictionary` removal) — the recorded admissions that its predicate is narrower than the hazard it names.

This proposal retires the warning. What stays is what soundness always rested on: §28.11 states that an element borrow is valid until its slot is mutated and is not statically tracked per slot (the posture §28.3's producers table already gives the indexer row), a use after the mutation is a mid-scope kill under the checked-mode backstop (K4.1, §28.2: `DisposedAccessException`, deterministic), and the three remedies (re-take the borrow after the mutation; take the element out first with `PopAt`/`Pop`/`PopValue`; mutate after the last use) stay in the code-generation reference as a *Looks right* pair. A lint over the same straight-line shape is tooling's to offer; the specification no longer mandates it.

Measured reach: no gtest or lit program asserts the warning (there is nothing to assert against — it was never implemented); no `src/` site; five banked rule-corpus cases name it (two `warn` cases whose baseline is already `fail #4603`, three `quiet` cases whose `rule:` headers quote the deleted paragraph and whose verdicts do not move). Nothing is newly rejected or newly admitted. This is a §10 subtraction, defended by no site.

---

## 1. The problem, in the text

### 1.1 The rule, as v0.337 states it

§28.11 *Owning collections*, second paragraph (390 words). Its deciding sentence:

> The compiler **must warn** when a local holding such a borrow is used (read, borrowed from, or passed) after a call on the **same collection binding** that **disposes the borrowed element** and **dominates the use on every path** — the dominating-kill shape of §28.2's use-after-dispose warning, with the kill on the collection rather than on the binding (derives: K4.2 + K7.4, the source per K2.1): `Clear()`, which disposes every element; `Remove(b)` — the element-removal member of the sequence and set families — where the argument is the borrow itself; and `RemoveAt(i)` where the borrow was taken as `coll[i]` with the same index expression — a constant, or a local read not reassigned between — and no structural operation of §29.12's list on the same collection binding between the borrow and the kill

and its closing posture: "The program stays accepted; no per-site suppression exists (rationale §5.28)."

The same paragraph records two silences: "An element-level write — an indexer-set — does not draw this warning … (a chosen silence, liftable; the checked-mode backstop stands behind it)" and "A keyed removal — `OwningDictionary<K, V>`'s `Remove(k)` after a borrow taken as `od[k]` — is likewise outside the kill set in this revision … (a chosen silence, liftable; the backstop stands behind it)", and one over-warn: where the element type overrides `Equals`, `Remove(b)` may dispose an equal element other than the borrowed one and "the warning is stated as certain there regardless".

### 1.2 What it costs: the text that exists only for it

| Document | Passage | Words |
|---|---|---|
| spec §28.11 | *The element-borrow use-after-removal warning* (the paragraph above) | 390 |
| spec §28.3 | the producers table's indexer row: "a mid-scope value kill whose dominating straight-line subset draws §28.11's element-borrow use-after-removal warning and which is otherwise not statically tracked per slot" | 20 |
| spec Appendix C.3 | the bullet restating the kill set and the two silences | 80 |
| stdlib §4.5 | the indexer-getter sentence naming the warning and its kill set | 45 |
| stdlib §4.21 (`OwningSlotMap<T>`) | *Ownership faces*: the producer-set/kill-set sentence with its "chosen silence, liftable" | 75 |
| code-generation reference §4.5, §4.6 | "compiles and **warns**"; the diagnostics list | 15 |
| rationale §5.28 | the fifth-instance paragraph (history; kept, with a dated retirement paragraph after it) | — |

About 625 normative words across the spec, stdlib and code-generation reference for one diagnostic.

### 1.3 The compiler (`e1a20aa44`, source read; no build)

- `grep -rn -i 'after-removal\|use_after_removal' src/` finds nothing. The ownership pass has no element-borrow kill analysis over collection bindings; the only slot-level analysis it ever had (the v0.334 field/slot restoration tracking) was retired by proposal 0174 at v0.336.
- The rule-corpus baseline records the gap: `s28_11_0008` and `s28_11_0009` (`expect: warn`) are banked `fail #4603`; `s28_11_0010`, `s28_11_0011`, `s28_11_0012` (`expect: quiet`) pass vacuously, as any program that draws no diagnostic does.
- #4603 is open with both lints of proposal 0129 and no implementation activity on either leg. Lint A (§29.12) is outside this proposal.

### 1.4 What the warning adds over the backstop

Nothing in verdict, and one straight-line shape in diagnosis. Every program the warning names is accepted, and every use it names faults deterministically in checked disposal mode — the default build (rationale §5.30) — because the removing member disposes the element (the slot policy's owned column: `Remove`/`RemoveAt` "**disposes** the removed element", `Clear()` "**disposes** every element") and a member access through the stale borrow meets K4.1's flag test. The warning's own text says so three times ("the checked-mode backstop stands behind it"; "the backstop stands behind it"; "The program stays accepted"). Its predicate — same binding, same index spelling, dominance on every path, no structural operation between — is the subset the compiler could claim as certain without alias or slot tracking; the two liftable silences and the `Equals` over-warn are the recorded edges of that subset.

The one build in which a diagnostic would add safety is the unchecked (release) build, where "use of a disposed instance is undefined behavior" (§28.2). That build is "selected per build, never implied", and the checked build is the default precisely so that the static rules need not close every dangle (rationale §5.30). A straight-line lint helps that build only for the shape it names; the shapes beside it (a conditional kill, a different index spelling, a keyed removal, an indexer-set, a mutation through an alias) were never named. The specification's posture for all of them is the backstop, and the proposal makes the named shape join them.

---

## 2. Measurement (at `e1a20aa44`)

| Set | Count | How measured |
|---|---|---|
| gtest programs asserting the warning | **0** | `grep -rn -i 'after-removal\|after removal\|4603' tests/codegen tests/sema tests/ownership …` — the one hit, `tests/codegen/llist_remove_eq_565_test.cpp`, reads "Count decremented after successful removal" (an unrelated `LinkedList<string>.Remove` count test) |
| lit programs | **0** | `grep -rln -i removal tests/lit` — the five hits are unrelated (`oftype` removed, primitive-method names) |
| `src/` sites | **0** | §1.3 |
| rule-corpus cases naming the warning | **5**, all banked | `s28_11_0008`, `s28_11_0009` (`warn`, baseline `fail #4603`); `s28_11_0010`, `s28_11_0011`, `s28_11_0012` (`quiet`, baseline `pass`) |
| doc-example corpus programs | **0** | no fenced block in any document draws or mentions it |
| guides | **0** | `grep -n -i 'after-removal\|after removal' docs/Bezel_Guide_0*.md` |
| stdlib sketches | **0** | the two stdlib mentions are prose, §1.2 |
| matrix cells quoting the paragraph | **4** (disposal R5/C1, R5/C6, R5/C7, R5/C9) | `scratch/v0338/work-W2/quote_home.py`; each cell's verdict (`once`, `once-backstopped`) is unchanged by the retirement (§5.4) |

Programs that draw the warning today: none can, since no compiler emits it. Programs whose verdict moves: none. Programs whose *diagnostic set* moves: the two `warn` corpus cases, from an unimplemented warning to silence.

---

## 3. Options

### 3.1 A — retire the warning; keep the posture sentence and the remedies (adopted)

§28.11's paragraph becomes one sentence: ▲ "A borrow so produced is valid until its slot is mutated — an indexer-set, `Remove`/`RemoveAt`, `Clear()` or the collection's own disposal disposes the referent — and is not statically tracked per slot: a use of it afterwards is a mid-scope kill under the checked-mode backstop (K4.1, §28.2). Re-take the borrow after the mutation, take the element out first through `PopAt`/`Pop`/`PopValue`, which leave no borrow behind, or mutate after the last use." The §28.3 indexer-row parenthetical loses the warning clause, Appendix C.3 loses the bullet, the stdlib sentences point at the backstop, and the code-generation reference gains a *Looks right* pair stating the run-time fault and the three remedies.

### 3.2 B — the status quo (declined)

Keeps 625 words and a `must warn` the compiler has never honoured, for a diagnostic whose every instance is backstopped. Declined under the criterion the owner gave: usefulness measured at zero sites, and the "workaround" for the absence of a warning is nothing at all — the program is the same program.

### 3.3 C — keep the shape as non-normative advice in the spec (declined)

A "should warn" or an informative note keeps the kill-set enumeration (the only expensive part) and the two liftable silences, while making the text's one load-bearing word unenforceable. A specification sentence that binds no implementation is exactly what the diet is removing; the advice belongs in the code-generation reference, where it now is (§7.5).

### 3.4 D — widen the warning to the recorded silences (declined)

Lifting the indexer-set and keyed-removal silences would make the predicate cover more of the hazard, at the cost of key-equality reasoning (§8.3) and a second operation vocabulary beside §29.12's — the reasons 0129 declined them. Widening a diagnostic no compiler implements, for zero measured sites, is the wrong direction under "prefer simplicity".

### 3.5 E — move the rule to a tooling appendix of the specification (declined)

The specification has no non-normative lint appendix, and creating one for a single entry adds a document surface. `docs/research` and the compiler's own diagnostics documentation are the homes for lint design; the rationale's §5.28 paragraph keeps the design record.

---

## 4. Evaluation

### 4.1 Soundness

Unchanged. The warning never made a program sound: its text says the program stays accepted and the backstop stands behind every shape. After the retirement the dangle is handled where it always was — K4.1's flag test on member entry in the default build. No K4.3 clause is added or removed; the residual register is untouched.

### 4.2 Simplicity

−625 normative words, one fewer mandated diagnostic, two fewer "chosen silence, liftable" records, one fewer Appendix C bullet, and §28.3's indexer row reads the same posture as every other borrow of a slot's current value (§28.14's scoped construction, K5.6): slots are not frozen, the use is backstopped.

### 4.3 Consistency

Rationale §5.28's doctrine (a statically-certain hazard, an admission kept for recorded reasons, a normative warning naming the boundary event and the remedies) keeps four instances. The doctrine is not retired; its fifth instance is, on measured reach and on the owner's criterion, which the coordinator's table applies row by row. The other four instances are outside this proposal and are not re-examined here. One of them shares this instance's measured state: lint A (§29.12's mutation-during-iteration warning, the same proposal 0129 and tracker #4603) is also unimplemented and also moves no verdict; it differs in that the fault it names is a runtime `InvalidOperationException` raised in every build, whereas the hazard retired here is undefined behaviour in an unchecked build (§1.4). Whether lint A earns its place is a separate §10 question, not decided by this proposal.

### 4.4 Migration (measured, §2)

- **Newly rejected:** nothing. **Newly admitted:** nothing. **Newly warned:** nothing.
- **Warning withdrawn (spec-ahead-of-impl both before and after):** the straight-line shape of §1.1. No test program, document example or stdlib sketch draws it.
- **Rule corpus:** `s28_11_0008` and `s28_11_0009` are banked `warn`/`fail #4603`; after the retirement the correct expectation is `quiet` (the program compiles with no diagnostic), which is what the compiler produces today — the baseline rows become `pass` once the headers are re-cut. `s28_11_0010`–`s28_11_0012` keep `quiet` and pass; their `rule:` headers quote the deleted paragraph and are re-headed to the replacement sentence. All five are banked, so they are left for the tracker with this list (process §6, deletion-shaped); no case is edited in this train.
- **#4603:** the lint-B leg is withdrawn by a note at landing; the issue stays open for lint A.

---

## 5. Decision (delegated ruling, 2026-10-13)

### 5.0 Decision: **A** — the warning is retired

Under the owner's criterion (retire when measured reach is negligible and the absence is worked around by a short, local, obvious rewrite with no loss of safety): reach is zero sites in every program set; the absence needs no rewrite at all, since the warned program is accepted both before and after; and safety is unchanged, the checked-mode backstop faulting every instance deterministically.

### 5.1 The §10 subtraction review

Process §10 asks of the shipped surface "what is no longer earning its place?", with the keep defended rather than assumed, and states that implementation state is not a removal ground. The ground here is not that the warning is unimplemented; it is that the warning decides nothing — no program is accepted or rejected by it — and that the hazard it names is already reported, deterministically, by the mechanism the specification makes the default. The implementation state is evidence only that nothing in the tree depends on it.

### 5.2 Why a design change

A compiler-mandated diagnostic is a normative obligation (§28.11: "The compiler **must warn**"; Appendix C.3 lists it among the enforced invariants). Removing an obligation changes what a conforming compiler must do and is a design change under process §2, taken through this proposal and §10.

### 5.3 Declined and reopened (rows for `DECLINED.md`, in `W2.json`)

One `retired` row (the warning itself, with the reopening bar: a measured corpus of real programs in which the shape escapes the backstop — an unchecked build whose use after removal is reached in practice — and a lint-level home outside the specification having been declined). The per-site-suppression row's parenthetical records the count moving from five lints to four. Options C, D and E are declined in this document (§3) and need no `DECLINED.md` rows of their own: C and E are editorial placements, and D is the lift 0129 already declined, whose record stands.

### 5.4 The matrices

Four disposal-matrix cells of the v0.336 run quote the retired paragraph: **R5/C1** (quotes the §28.3 indexer-row clause; verdict `once-backstopped`), **R5/C6** (quotes "An element-level write — an indexer-set — does not draw this warning"; verdict `once-backstopped`), **R5/C7** (quotes the dominating-kill sentence and the re-taken-borrow sentence; verdict `once`), **R5/C9** (quotes the `must warn` sentence; verdict `once-backstopped`). No verdict moves: each cell's disposal count rests on the owned column's dispose-on-removal and K4.1's flag, which the retirement keeps. The next matrix re-run re-quotes the four cells against the replacement sentence ("is not statically tracked per slot: a use of it afterwards is a mid-scope kill under the checked-mode backstop"); R5/C7's note ("An element borrow joined across a conditional `Clear()`/`RemoveAt` is not warned … and falls to K4.1's flag") becomes the posture for every path. The disposal matrix's README (C9's description names "§28.2's use-after-dispose warning versus the maybe-consumed …") is unaffected; §28.2's own use-after-dispose warning on a *binding* is not touched by this proposal.

### 5.5 What realistic code loses

A diagnostic no compiler has emitted. The guides never taught the shape as a warning (Guide 5 teaches `PopAt`/`Drain` as the ways to take elements out); the code-generation reference's §4.5 sentence, which did say "compiles and **warns**", now says "compiles **silently** and faults in checked mode", and §7.5 shows the fault and the `PopAt` spelling side by side.

---

## 6. Edits at graduation

### 6.1 The edit set

`scratch/v0338/W2.json`, ids `W2/0181/…` (12 edits): spec §28.11 (`28.11/warning-retired`), §28.3 (`28.3/producers-row`), Appendix C.3 (`C.3/bullet-deleted`); stdlib §4.5, §4.21; code-generation reference §4.5, §4.6, §7.5; rationale §5.28; `DECLINED.md` (two); `README.md` (one). The spec's §28.3 edit lands inside the retirement's own parenthetical of the indexer row (lane W1 owns §28.3; the anchor is 32 words of that parenthetical and nothing else). The `README.md` and `DECLINED.md` rows anchor on their group-closing headings for sequential insertion with lanes W3 and W4.

### 6.2 Conformance accounting (process §6, deletion-shaped)

- `tests/conformance/rules/cases/s28_11_0008.bzl`, `s28_11_0009.bzl`: banked `warn` (baseline `fail #4603`) → `quiet`; titles and `rule:` headers re-cut to the replacement sentence ("a use of an element borrow after a dominating `RemoveAt` at its index compiles with no diagnostic; the use faults in checked mode"). Left for the tracker.
- `s28_11_0010.bzl`, `s28_11_0011.bzl`, `s28_11_0012.bzl`: banked `quiet`, verdict kept; `rule:` headers quote the deleted paragraph and are re-headed. Left for the tracker. *(Composed at v0.338: the three `rule:` headers were refreshed at the landing to quote the replacement sentence, verdicts and baseline rows untouched; `s28_11_0008`/`0009` stay for the tracker.)*
- `manifest.tsv` rows for the five cases regenerate with the headers; `baseline.tsv` rows 1378–1379 (`s28_11_0008`, `0009`: `fail #4603`) become `pass` once the expectations are re-cut.
- `tests/conformance/rules/README.md`'s banked-docket table (the 0129 / #4603 row lists `s28_11_0008`, `0009` among its fail rows) loses those two entries when they are re-banked.
- Comment refresh, no behaviour: `src/ownership/Ownership_Checks.cpp` (≈ line 16744) and `tests/ownership/await_site_stmt_walk_3074_test.cpp` (line 10) name §30.5's dissolved lead *Which binding forms qualify* in prose; the lead is now *Which declarations qualify* (lane W2's editorial edit, not this proposal).
- Coverage gap not written: a `run` case in which the use after `RemoveAt` faults with `DisposedAccessException` in checked mode (the §7.5 pair's first half), which pins the posture the replacement sentence states.
- The doc-example corpus is not regenerated (#1380); the new §7.5 block is one more extracted fragment, recorded in the changelog's EXTRACT count.

### 6.3 The compiler tracker (filed at landing)

A note on #4603 withdrawing the lint-B leg (nothing to implement; nothing to delete, since nothing was built), and listing the five corpus cases of §6.2 for re-heading and the baseline rows for re-banking. Lint A is untouched.

*Currency note (2026-10-21, v0.340, [#6573](https://github.com/s8lap/bezel-lang/issues/6573)).* The premise of §0, §1.3, §2, §4.4, §6.2 and this section — that nothing was built for lint B — was false when written: fix #4603 (`c845af61e`, v0.330) implemented both of proposal 0129's lints, and `s28_11_0008`/`0009` were banked **`pass`** at `expect: warn`, not `fail`. The retirement stands on its other grounds (no verdict moved; the hazard is the checked-mode backstop's; the predicate was narrower than the hazard). The v0.340 train decided (lane `B1`, [#6573](https://github.com/s8lap/bezel-lang/issues/6573)) that the compiler's lint-B code is deleted and the two cases are re-cut to `expect: quiet` — title and `rule:` header to §28.11's replacement sentence, the `diagnostic:` field dropped — in the deleting commit, which re-banks them `pass`; `tests/conformance/rules/README.md`'s banked-docket row for 0129 / #4603 loses the two entries with them.

---

## 7. Self-critique

- *Is "unimplemented" doing the work?* No: the decision rests on verdict-neutrality plus the backstop; §5.1 says so and the reopening bar is phrased over programs, not over implementation effort. But the measurement is thinner than a verdict-moving retirement's would be — there are no programs to count because nothing could draw the warning — so "zero reach" here means "no program depends on it", not "no program would have benefited".
- *The unchecked build.* The honest cost is §1.4's last paragraph: in a release build the shape is undefined behaviour and the warning would have named one slice of it. The specification already accepts that posture for every neighbouring slice; the proposal does not widen the exposure, it stops singling out one slice.
- *Rationale §5.28's doctrine* now has a retired instance, which its "no per-site suppression spelling exists for any of the five lints" sentence must not keep counting. The dated paragraph updates the count; the `DECLINED.md` suppression row records the same.
- *Lane seams.* The §28.3 anchor is inside W1's section; if W1 respells the indexer row, the composition re-anchors the 32-word parenthetical (W2.md names it). `README.md`/`DECLINED.md` insertions are sequential with W3/W4.
