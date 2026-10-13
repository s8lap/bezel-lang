# Proposal 0183 — Retire the marked named slot `move N = var x`

| | |
|---|---|
| **Status** | graduated-v0.338 — superseded — graduated v0.338 (accepted 2026-10-13 as a delegated ruling under the owner's 2026-10-13 direction for the diet train ("Make a judgement on what should be retired based on how useful the feature is and how easy it is to write code differently to work around the absence of the feature") and the coordinator's retire/keep table, row D-E — the named destructure's transfer spelling `move N = var x` is retired: a named slot borrows or copies and never transfers, in an owning match included; a program that wants an owned or move-only value payload transferred writes the positional destructure, `_` discarding a sibling with the construct; `move` is not a slot spelling; critique round 1 applied (reach re-measured: no gtest or lit program and no parser arm; two unbanked rule-case headers respelled, verdicts kept); landed with the v0.338 changelog entry) |
| **Tracking issue** | the 2026-10 subtraction review (scratch/diet/D2.md, candidate D-E; scratch/v0338/JUDGEMENT.md); compiler tracker filed at the v0.338 landing (two diagnostics stop naming the marked slot as a remedy; `s35_5_0008`, banked `fail`, flips run → reject; `s35_5_0009`'s diagnostic needle) |
| **Target module(s)** | **spec**: §19.3 (*Named destructuring*'s last sentence; *Named-slot bindings and ownership* respelled; the marked-slot paragraph, its rejection paragraph, the example and the parse note deleted), §19.6 (*Named-destructure arms*), §15.3 (*Pattern binding*), §35.4 (*Disposal is defined by the active case*), §35.5 (the marked-slot paragraph deleted; the by-name bullet; the nested-destructuring paragraph), Appendix A.7 (`NamedSlot` and its comment; the `move`-rooted-forms table row), C.7 (one row deleted, one respelled). **rationale**: §3.14 (one dated paragraph). **code-generation reference**: §3.5 (one line), §6 (the consuming-match idiom), §7.2 (one *Looks right, is rejected* pair). **Guide 3** §2 (one bullet and its example). **`DECLINED.md`**: one `retired` row, one `declined-at-proposal` row. **Rule corpus**: two unbanked headers refreshed (`scratch/v0338/W3.corpus.json`). |
| **Written against** | spec/stdlib **v0.337** at `e1a20aa44` (main). Section numbers are v0.337's. |
| **Origin** | scratch/diet/D2.md §2, *§19.3 Patterns — Simpler*: "retire the marked named slot `move N = var x` (≈500 w + grammar A.7): 7 test files, 4 doc mentions; the positional form already transfers every obligation-bearing slot". Drafted by lane W3 of the v0.338 diet train. Re-measured at HEAD: the "7 test files" are seven *corpus* programs (four documentation examples and three rule cases); no gtest or lit program uses the form and the compiler never parsed it (§4). |

*Reference convention: unprefixed §-references are language-spec references at v0.337. Quotations are verbatim. ▲ marks proposed text. The former name of the language is not used anywhere in this document.*

---

## 0. Summary

Proposal 0092 (v0.250) gave the named destructure a transfer spelling: in an owning match, `Fetched(move response = var r)` transfers the `owned` payload `response` into `r` at arm selection, while an unmarked named slot keeps the borrow. Its recorded ground was the *per-slot peek* — "the named form's ability to borrow one payload while transferring another in the same list, the one capability no other form supplies" (rationale §3.14). The form costs a six-condition admission rule with a five-case rejection set, a three-token parse commit and its grammar comment, a §19.6 exhaustiveness clause, a §35.5 machinery paragraph, an Appendix C.7 row and qualifiers ("unmarked") in seven other sentences — about 500 words of §19.3 and 300 elsewhere.

Measured at HEAD, nothing uses the capability: zero gtest programs, zero lit programs, no parser arm (`Parser_Exprs.cpp` never reads `move` inside a slot list; the only compiler mentions are two ownership diagnostics that *name* the marked slot as a remedy), and the seven corpus programs are the examples that introduced the form (spec §19.3, the code-generation reference twice, Guide 3) plus three rule cases written from the spec text. The positional destructure transfers every obligation-bearing slot at once and `_` discards a sibling, disposed with the construct — which is the per-slot peek's one real shape (take one payload, drop the other). The proposal retires the marked slot: a named slot never transfers, and `move` is not a slot spelling. *Unmarked symmetry* — named slots transferring silently in an owning match — stays declined for the reason §3.14 records, so the named form has no transfer spelling at all; this is the v0.224 reading the form widened.

This is a §10 subtraction and it is spec-ahead-of-implementation in the retiring direction: the compiler already rejects the form (as a parse error), so no accepting program flips in `bezelc`; what flips is one banked rule case that recorded the divergence (`s35_5_0008`, `fail`).

---

## 1. The text at v0.337

§19.3 *Named destructuring*: "A named slot may carry the transfer marker on its source — `move N = var x` (grammar A.7) — admitted only in an owning match (§35.5)". The marked-slot paragraph: "The marked slot is admitted exactly when **(i)** the match is an **owning match** …, **(ii)** `N` names an **`owned`-marked payload field or a payload field of move-only value type** …, and **(iii)** the slot's pattern is a top-level binding (`var x`; v1)." The rejection paragraph names five rejected placements (a borrowing match; a copyable, `string` or runtime-managed payload; a plain class-typed payload of an owning case; a record or class head; a `_` right-hand side). §35.5 adds a paragraph on how the marked slot "rides this machinery unchanged", and the by-name bullet restates the admission; §19.6 counts a marked slot as irrefutable; §15.3's *Pattern binding* names it beside the positional slot; Appendix A.7's `NamedSlot ::= ("move")? Identifier "=" AltPattern` carries a 16-line comment on the three-token commit; Appendix C.7 has a row.

What the form adds beyond the positional destructure, in its own words (§19.3): "the positional form already transfers *every* obligation-bearing slot at once, so multiple named transfers spell a subset of what the construct already does."

---

## 2. Options

**A — status quo.** Keep the marked slot.

**B — retire the marked slot; named slots never transfer (this proposal).** The v0.224 borrow-only reading, restated as a rule; the positional destructure is the transfer spelling.

**C — retire the marked slot; adopt unmarked symmetry.** Named slots transfer in an owning match exactly as positional slots do, no marker. Declined at v0.250 (rationale §3.14, `DECLINED.md`: *Named-destructure transfer — unmarked symmetry*); reopening it needs new evidence against the stated reason (a named slot that transfers silently inverts the borrow reading the v0.224 design chose), and none has arrived.

**D — keep the marked slot for choice cases only.** Its admission was already choice-case-only (condition (ii) names a payload field; a record or class head is rejected "by derivation"), so this is the status quo under another name.

**E — a slot-list-level marker** (`Ready(move: conn = var c, attempts = var n)`), transferring every obligation-bearing slot of the list. One rule instead of six, but it is the positional destructure with names, which no site asked for.

---

## 3. Evaluation

| | A | B | C | D | E |
|---|---|---|---|---|---|
| soundness | sound | sound: the borrow reading is §19.3's general rule, and the owning match's disposal at construct completion already covers every payload a named slot does not take (§35.5 traces, unchanged) | sound but reverses a recorded decline on no new evidence | = A | sound |
| simplicity | six conditions, five rejections, a parse commit, two paragraphs in §35.5, a C.7 row | −1 spelling, −800 w, grammar `NamedSlot ::= Identifier "=" AltPattern` | −1 spelling, but a silent transfer at a form whose reading is "ask by name" | = A | −5 conditions, +1 spelling nobody uses |
| consistency | the only transfer spelling that sits *inside* a slot list | one transfer spelling per construct: the positional slot (and `case move x of` for the scrutinee) | the named form's `=` reads as interrogation (rationale §3.14); a silent transfer contradicts it | = A | as B plus a redundant form |
| migration | — | 7 corpus programs (4 doc examples, 3 rule cases), 0 gtest/lit, 0 compiler arms | the same programs, plus every unmarked named slot over an owned payload changes meaning (silent transfer) — a verdict flip in the *other* direction at every such site | — | 7 programs |
| reach of the lost capability | — | borrow-one-transfer-another in one list: 0 sites; spelled as positional with `_` where the sibling is unneeded, or as two statements where it is needed after the arm (`Ready(var c, var n)` then use `n` — the copy is free) | — | — | — |

B is chosen. C is the stronger simplification on paper, but it is a reversal of a recorded decline whose reason stands, and it changes the meaning of every existing unmarked named slot over an owned payload — the opposite of a subtraction. D is A. E adds a spelling at zero demand.

---

## 4. Measurement (HEAD `e1a20aa44`; `grep -rnE 'move [A-Za-z_]+ *= *(var|_)'`)

| where | count | sites |
|---|---|---|
| gtest programs | **0** | — |
| lit programs | **0** | — |
| compiler | **0 arms**; 2 diagnostics name the form as a remedy | `src/ownership/Ownership_Checks.cpp:8541` ("positional destructure / marked named slot that does …"), `:8611` ("(only a positional slot or a marked named slot of a …"); `Parser_Exprs.cpp` has no `move`-in-slot path (the `NamedPattern` type there is the type-name pattern, not a slot) |
| documentation examples (doc corpus) | 4 | spec §19.3 (`spec_0117`), code-generation reference §3.5 and §6 (`codegen_0012`, `codegen_0039`), Guide 3 §2 (`guide3_0002`) |
| rule corpus | 3 | `s35_5_0008` (run; **banked `fail`** — the compiler rejects the form today), `s35_5_0009` (reject, banked `pass`; marker in a borrowing match), `s19_3_0018` (reject, unbanked; `move N = _`) |
| guides | 1 bullet + 1 example | Guide 3 §2 |
| stdlib | 0 | — |
| spec mentions outside §19.3 | 6 sentences | §15.3, §19.6, §35.4, §35.5 ×3 (one paragraph, one bullet, one nested-pattern sentence), A.7 ×2, C.7 ×2 |

**Over-rejection.** `move` at a named slot becomes a compile error at the pattern, naming the positional form. Every program the compiler accepts today is unchanged (it accepted none with the form). Relative to the spec: `s35_5_0008`'s program (run) becomes a reject; `s35_5_0009` and `s19_3_0018` stay rejects with a different diagnostic. No unmarked named slot changes meaning.

---

## 5. Soundness

The marked slot was "a second slot spelling routing a payload the construct already owns" (rationale §3.14): "no new kernel rule, no axiom touched". Removing it removes a routing option inside a consume the construct performs anyway; the un-transferred remainder — now every named-matched payload — "is disposed at construct completion, or during unwinding, by the disposal traces" of §35.5, which already cover unmarked named slots. The write-point rule (§19.3 *Move-only payload bindings across a write point*) is stated for borrowing-match bindings and is untouched; an owning match's transferred positional binding is "a stable local of the body over the construct" there and stays so. Matrix cells: none quote the marked-slot text (borrow-flow R2/C11 and R5/C11 quote the general pattern-binding sentence, which is unchanged).

---

## 6. The normative edits

`scratch/v0338/W3.json`, ids `W3/0183-*` (22 edits), anchored after lane W3's editorial §19.3 edits. In outline:

- **§19.3.** *Named destructuring*'s last sentence ("A named slot may carry the transfer marker …") deleted. *Named-slot bindings and ownership* loses "unmarked" and gains the rule: ▲ "A named slot **never transfers**: … A program that wants an owned or move-only value payload transferred writes the **positional** destructure, which transfers every obligation-bearing slot at once, a `_` slot disposing a sibling with the construct; `move` is not a slot spelling (`Fetched(move response = var r)` is a compile error at the pattern naming the positional form — retired v0.338)." The marked-slot paragraph, the rejection paragraph, the example and the parse note (HEAD lines 4021–4035) are deleted.
- **§19.6:** "(a binding or `_`; a marked slot … contributes exactly as `N = var x` does)" → ▲ "(a binding or `_`)".
- **§15.3 *Pattern binding*:** "a **positional slot or a marked named slot** … an **unmarked named slot** keeps the borrow" → ▲ "a **positional slot** … a **named slot** keeps the borrow".
- **§35.4:** "positional slots, marked named slots, and the payloads reached recursively" → ▲ "positional slots and the payloads reached recursively".
- **§35.5:** the marked-slot paragraph (HEAD line 10129) deleted; the by-name bullet: ▲ "A named slot never transfers, in an owning match included: a named-matched owned or move-only value payload is borrowed during the arm and disposed at construct completion (§19.3). A program wanting the payload transferred writes the positional destructure."; the nested-destructuring paragraph loses the marked-slot sentence.
- **Appendix A.7:** `NamedSlot ::= Identifier "=" AltPattern`; the comment states ▲ "A named slot never transfers (§19.3); `move` is not a slot spelling (the marked slot `move N = var x` was retired at v0.338)." and drops the three-token-commit text; the `move`-rooted-forms table row loses its parenthetical.
- **Appendix C.7:** the marked-slot row deleted; the slot-list row: ▲ "with a named-slot binding a borrow (a named slot never transfers) and a constant slot an `=` test".
- **rationale §3.14:** one dated paragraph.
- **code-generation reference:** §3.5's `Fetched(move response = var r)` line deleted; the §6 idiom respelled (`Ready(var c, var n)`, "the positional destructure transfers its owned payloads (a named slot never transfers)"); one *Looks right, is rejected* pair at the end of §7.2.
- **Guide 3 §2:** the bullet and its example respelled positionally.
- **`DECLINED.md`:** a `retired` row; a `declined-at-proposal` row for options D and E.
- **Rule corpus** (`W3.corpus.json`): `s19_3_0017`'s header loses "unmarked"; `s19_3_0018`'s title, rule and diagnostic needle are respelled to the new rule (verdict `reject` kept).

---

## 7. Declined alternatives (rows for `DECLINED.md`)

- **Keeping the marked slot for choice cases only (option D)** — choice cases were the whole of the form's admission, so the confinement is not a narrowing. Reopening bar: a measured program that borrows one owned payload while transferring a sibling in one slot list and cannot discard the sibling with `_`.
- **A slot-list-level `move` (option E)** — the positional destructure with names, which no site asked for. Same bar.
- **Unmarked symmetry (option C)** — already a `rejected` row (*Named-destructure transfer — unmarked symmetry, and the receiver-side marker spellings*); its reason stands and this proposal does not touch it.

---

## 8. Changelog, migration, trackers

**Draft changelog bullet:** see `scratch/v0338/W3.md`.

**Migration.** *Newly rejected (relative to the spec; the compiler rejects it already):* `move N = p` in a named slot list — four documentation examples (respelled in this train), `s35_5_0008` (banked, listed for the tracker: run → reject), `s35_5_0009` (banked, stays reject; its `diagnostic: borrowing match` needle will mismatch the new diagnostic — tracker), `s19_3_0018` (unbanked; header refreshed, stays reject). *Newly admitted:* nothing. *Workaround:* `Ready(var c, var n)` transfers `conn` and copies `attempts`; `Ready(var c, _)` transfers `conn` and disposes the discarded sibling with the construct. *Doc corpus:* `spec_0117`, `codegen_0012`, `codegen_0039`, `guide3_0002` regenerate from the respelled blocks.

**Trackers to file at the landing** (one `spec-tracking` issue): (i) the two `Ownership_Checks.cpp` diagnostics stop naming the marked slot; (ii) a `move` token at a named slot reports "`move` is not a slot spelling — a named slot never transfers; write the positional destructure" (today it is a generic parse error); (iii) `s35_5_0008` flips to `reject` with that needle and `s35_5_0009`'s needle is re-keyed; *(composed at v0.338: `s35_5_0009`'s and `s35_5_0010`'s `rule:` headers were refreshed at the landing, verdicts and needles untouched)* (iv) corpus regeneration for the four doc cases.

---

## 9. Self-critique (adversarial pass)

(a) *Is the lost capability really zero?* The grep is over `tests/`, both corpora and `docs/` for every `move <ident> = ` inside a pattern; the compiler has no parse path for it, so no test could exercise it except as a reject — and none does. The capability's one real shape (take one payload, drop the other) is `Ready(var c, _)`; the shape it alone could spell (take one payload, *borrow* the other for the arm, then let the construct dispose it) needs the sibling to be an `owned` payload the arm reads without owning — a borrow of a value the arm could just as well own and let auto-drop, at the cost of a must-use warning if unused; where that cost matters the arm owns both and disposes one (`_ := s;`).

(b) *Does deleting the §35.5 paragraph lose a rule about guards?* Its guard clause ("a guard-declined arm transfers nothing, and two arms marking the same payload are two candidate recipients") was stated for the marked slot; the same fact for positional slots is *Guards: borrow during trial, transfer on selection* in §35.5, which is untouched.

(c) *The `s35_5_0009` case was `reject` for "no obligation to transfer" (the marker on a non-owned payload); it stays `reject` for "`move` is not a slot spelling".* A banked case is not edited here; its needle mismatch is the tracker's.

(d) *Is this proposal a reversal of 0092?* It retires 0092's form on the §10 bar (a keep is defended by sites, and none exist); 0092's own decline of unmarked symmetry is kept, so the named form returns to exactly the v0.224 reading. The rationale paragraph records both.

(e) *Former-name check:* none used.
