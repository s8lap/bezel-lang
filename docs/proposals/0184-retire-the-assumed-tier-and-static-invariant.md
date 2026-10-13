# Proposal 0184 — Retire the `assumed` contract tier and `static invariant`

| | |
|---|---|
| **Status** | graduated-v0.338 — superseded — graduated v0.338 (accepted 2026-10-13 as a delegated ruling under the owner's 2026-10-13 direction for the diet train ("Make a judgement on what should be retired based on how useful the feature is and how easy it is to write code differently to work around the absence of the feature") and the coordinator's retire/keep table, rows D-C and D-G′, one proposal — the reserved, never-populated `assumed` tier is deleted (a contract clause is proven or checked) and the `static` qualifier on an invariant member is deleted: `static invariant(E);` is a compile error at the member; contract clauses on `extern` declarations stay a deferred design; critique round 1 applied (the workaround stated as deleting the inert member, `invariant(E);` starting enforcement rather than keeping it); landed with the v0.338 changelog entry) |
| **Tracking issue** | the 2026-10 subtraction review (scratch/diet/D2.md, candidates D-C and D-G; scratch/v0338/JUDGEMENT.md); compiler tracker filed at the v0.338 landing (the parser's #4449 warning becomes the rejection; `MetaContractTier::Assumed` deleted; six gtest files) |
| **Target module(s)** | **spec**: §22.16 (the lead paragraph's tier sentence; *The tier system* heading and paragraph; *Discharge sites*; *Class invariants*' lead paragraph and *Out of scope in this revision*; *Scope*'s `extern` sentence), §2.8.2 (*Contract records*' tier sentence), Appendix E (the "any tier → assumed" row, in the relocated §2.8.6 table), A.4 (`InvariantDecl`), C.15 (two bullets). **rationale**: §9.8 (one dated paragraph). **Guide 6** §4 (one sentence). **`DECLINED.md`**: two `retired` rows, one `declined-at-proposal` row, one `deferred` row respelled (*Contract clauses on `extern` declarations*). **No stdlib or code-generation-reference change** (neither names either form; the reference's §3.3 lists `invariant(E);` without a `static` form). |
| **Written against** | spec/stdlib **v0.337** at `e1a20aa44` (main). Section numbers are v0.337's; "Appendix E" is lane W3's relocation of §2.8.6 in the same train. |
| **Origin** | scratch/diet/D2.md §2, *§22.16 Contract clauses — Simpler*: "D-C retire the `assumed` tier (reserved, unpopulated): 0 reach; ≈120 w across §22.16, §2.8.2, §2.8.6. … `static invariant` (inert, warns) could go alone at 0 reach." Drafted by lane W3 of the v0.338 diet train. |

*Reference convention: unprefixed §-references are language-spec references at v0.337. Quotations are verbatim. ▲ marks proposed text. The former name of the language is not used anywhere in this document.*

---

## 0. Summary

The v0.203 contract design (proposal 0043, rationale §9.8) carried two reservations for a future it named. The **`assumed` tier** — "stated and carried, neither proven nor evaluated (unpopulated in this revision; reserved for contract clauses on `extern` declarations, to be designed jointly with the extern-purity question)" — has never had an inhabitant: no clause can occupy it, no declaration can request it, and the three places that mention it (§22.16's tier sentence and tier paragraph, §2.8.2's record tier, §2.8.6's "any tier → assumed" row) describe what a populated tier *would* classify as. **`static invariant`** — the `static` qualifier on an invariant member — parses, instruments nothing, and since v0.303 (#4256) must warn that it does nothing: "a prove-time obligation with no prover to discharge it and no run-time instrumentation standing in, so `static invariant(false);` compiles and does nothing". Its reach is the six gtest files that pin the warning and the parse.

Both are deleted under the §10 bar. The tier system becomes the two tiers a clause can actually occupy — proven, checked — and an invariant member takes no `static` qualifier: `static invariant(E);` is a compile error at the member, the verdict the warning was standing in for. Nothing a program can write today is lost: the inert clause asserted nothing, and no clause ever occupied `assumed`. Both deletions are widenable later — a prover that discharges invariants at the declaring body re-admits the qualifier, and a designed `extern` contract clause brings its tier with it — and `DECLINED.md`'s rows say so.

---

## 1. The text at v0.337

**The `assumed` tier.** §22.16 lead: "Every clause occupies exactly one recorded enforcement tier — **proven**, **checked**, or **assumed** — and the `static` clause qualifier demands the proven tier". *The tier system: proven / checked / assumed*: "**assumed** — stated and carried, neither proven nor evaluated (unpopulated in this revision; reserved for contract clauses on `extern` declarations, to be designed jointly with the extern-purity question)". *Scope*: "No clauses on **`extern` declarations** (the assumed tier's intended client, designed jointly with extern purity)". §2.8.2 *Contract records*: "**assumed** where §22.16 admits stated-unverified clauses (reserved; unpopulated in this revision)". §2.8.6's table: "| any tier → assumed (when that tier is populated) | **breaking** at every level that consumed the enforcement | …|". Guide 6 §4 repeats the three tiers. The compiler carries `MetaContractTier::Assumed = 2` (`src/module/ModuleMetadata.hpp:472`) and a comment (`src/ast/Node.hpp:684`); nothing assigns the value.

**`static invariant`.** Grammar A.4: `InvariantDecl ::= "static"? "invariant" "(" Expression ")" ";"`. §22.16 *Discharge sites*: "`static ensures`/`static invariant` makes an unprovable body a compile error at the declaration (§22.4's existing rule) — **the behaviour the prover fragment brings**, not this revision's: until it lands a `static invariant` is inert and warns at the clause". *Out of scope in this revision*: "`static invariant` parses from day one and is useful when the prover fragment lands — and because it is **inert** until then, the compiler **must** emit a warning at the clause naming that fact. … nothing else in the program distinguishes that from a checked predicate, and a contract a reader believes is enforced is worse than no contract." Appendix C.15: "(declaring body for `ensures`/`invariant`; …)". The compiler parses it (`Parser_Decls.cpp:1697–1716`, the warning), carries `isStatic` on the member, and codegen excludes it from instrumentation (`LlvmBackend_Setup.cpp:7313`: `if (inv->expr && !inv->isStatic)`).

---

## 2. Options

**A — status quo.** Keep both reservations.

**B — retire both (this proposal).** Two tiers; `static invariant(E);` a compile error.

**C — retire the `assumed` tier only.** `static invariant` stays inert-and-warning.

**D — retire `static invariant` only.** The `assumed` tier stays reserved.

**E — make `static invariant` a checked invariant plus a warning.** Not a retirement: it gives the qualifier a meaning the prover floor forbids ("`static` on an out-of-fragment clause is an error, never a silent downgrade to a runtime check").

---

## 3. Evaluation

| | A | B | C | D | E |
|---|---|---|---|---|---|
| soundness | sound; the inert clause is the recorded hazard (#4256) | sound: no clause's verdict or tier changes; the one form that compiled while asserting nothing is rejected | sound | sound | contradicts *The current prover floor* |
| simplicity | three tiers of which two exist; a qualifier that warns | −2 reservations: ≈120 w of tier text, ≈110 w of `static invariant` text, a grammar alternative, a mandatory warning, an Appendix E row, a C.15 clause, a metadata enum value | half | half | +1 rule |
| consistency | the tier system describes a value nothing can take; the grammar admits a qualifier the semantics cannot honour | the tier system is the set of tiers a clause can occupy; every `static` qualifier the grammar admits demands a proof the floor can be asked for (`static requires`, `static ensures`) | the qualifier stays inert | the tier stays empty | — |
| migration | — | 0 `assumed` sites; `static invariant`: 0 programs outside the six gtest files that pin the warning and the parse (§4) | — | — | — |
| reversibility | — | both widen back: the prover fragment (Appendix D.2) re-admits `static invariant`; a designed `extern` clause brings its tier | — | — | — |

B is chosen: C and D each keep one of two reservations with no inhabitant, and the owner's judgement names both. E is recorded as declined (it is the one reading that would keep the spelling at a cost).

---

## 4. Measurement (HEAD `e1a20aa44`)

| | count | sites |
|---|---|---|
| `assumed` tier — programs | **0** | no spelling exists |
| `assumed` tier — compiler | 1 enum value, 1 comment | `src/module/ModuleMetadata.hpp:472` (`Assumed = 2`), `src/ast/Node.hpp:684` |
| `assumed` tier — docs | 5 sentences + 1 row | §22.16 ×3, §2.8.2, §2.8.6 (row), Guide 6 §4; rationale §9.8's v0.203 paragraph (history, kept) |
| `static invariant` — gtest files | **6** | `tests/sema/static_invariant_inertness_warning_4449_test.cpp` (the warning, 7 programs: accept + warn → **reject**), `tests/sema/static_invariant_ordinary_class_4206_test.cpp` (3 programs: accept → **reject**), `tests/parser/contract_clauses_2053_test.cpp:246–254` (parse accept → **reject**), `tests/sema/extension_invariant_member_4204_test.cpp:106–119`, `enum_choice_invariant_member_4115_test.cpp:97–107`, `record_static_class_invariant_member_4186_test.cpp:102–113` (reject stays reject; the diagnostic may change — the body-kind rejection and the qualifier rejection are two errors on one member) |
| `static invariant` — lit, corpus, guides, stdlib | **0** | — |
| `static invariant` — compiler | parser warning (`Parser_Decls.cpp:1697–1716`), `isStatic` on `InvariantDecl` (`Node.hpp:774–790`), metadata (`ModuleMetadataBuilder.cpp:2597`), codegen exclusion (`LlvmBackend_Setup.cpp:7313`) | |

**Over-rejection.** `static invariant(E);` in any class body: the ten gtest programs above that accept it today, none elsewhere. No clause anywhere changes tier or verdict.

---

## 5. Soundness

The tier of a clause is a declaration fact (§2.8.2: "**proven** for a `static`-qualified clause … and **checked** otherwise"); removing a value no clause takes changes no record. The invariant member keeps every evaluation point of *Class invariants*; what is removed is a qualifier that §22.16 itself said "compiles and does nothing". Appendix D.2's **Contract proving** feature is unchanged: it is "the extended prover fragment of §22.16 … what gives the `static` clause qualifier reach beyond the constant-and-`when`-clause floor" for `requires` and `ensures`; an invariant discharged by that fragment is a later widening, not a Core change. No matrix cell quotes either form. Rule corpus: `s22_16_0018`/`0020` (the `static` qualifier on `requires`/`ensures`) are untouched; no case spells `static invariant` or names the `assumed` tier.

---

## 6. The normative edits

`scratch/v0338/W3.json`, ids `W3/0184-*` (15 edits), anchored after lane W3's editorial §22.16 edits and the §2.8.6 relocation. In outline:

- **§22.16 lead:** ▲ "Every clause occupies exactly one recorded enforcement tier — **proven** or **checked** — and the `static` clause qualifier demands the proven tier".
- ***The tier system: proven / checked*** (heading; anchor `sec-contract-tiers` kept): the `assumed` arm deleted.
- ***Discharge sites:*** ▲ "An `ensures` clause is discharged at the **declaring body**: … `static ensures` makes an unprovable body a compile error at the declaration (§22.4's existing rule). An `invariant` member takes no `static` qualifier (*Class invariants* below)."
- ***Class invariants*** lead: ▲ "An invariant member takes no `static` qualifier: `static invariant(E);` is a compile error at the member, the invariant being checked, never proven, in this revision (the proven tier for invariants returns, if at all, with a prover that discharges one at the declaring body)." *Out of scope in this revision* loses its `static invariant` sentences (the list of the three out-of-scope invariant kinds stays, with the DECLINED pointer).
- ***Scope:*** ▲ "No clauses on **`extern` declarations** (a deferred design, joint with extern purity), **destructors**, or **static constructors**."
- **§2.8.2 *Contract records*:** ▲ "… never on the member's body or call sites); and **checked** otherwise."
- **Appendix E:** the "any tier → assumed" row deleted.
- **Appendix A.4:** `InvariantDecl ::= "invariant" "(" Expression ")" ";"`, the comment noting ▲ "no `static` qualifier (an invariant is checked, never proven — §22.16)".
- **Appendix C.15:** "(declaring body for `ensures`; each call site for `requires`)"; "`invariant` members appear only in classes and take no `static` qualifier".
- **Guide 6 §4:** ▲ "every clause occupies one recorded **tier**: **proven** (discharged statically, zero cost) or **checked** (a runtime evaluation)."
- **rationale §9.8:** one dated paragraph.
- **`DECLINED.md`:** the `deferred` row *Contract clauses on `extern` declarations* loses "(the assumed tier's intended client)" and notes the retirement; two `retired` rows; one `declined-at-proposal` row (option E and the "chose not to evaluate" reading of `assumed`).

---

## 7. Declined alternatives (rows for `DECLINED.md`)

- **Reading `static invariant` as a checked invariant plus a warning (option E)** — a demanded proof silently downgraded to a runtime check is the drift the `static` qualifier exists to prevent (§22.16, *The current prover floor*). Reopening bar: the prover fragment discharging invariants at the declaring body, which re-admits `static invariant` as a widening.
- **Keeping the `assumed` tier as the word for a clause an implementation chose not to evaluate** — the recorded tier is demand-derived from the declaration (rationale §9.9), so no such clause exists to name. Reopening bar: a designed `extern` contract clause, which re-admits a tier with it.
- The existing `rejected` row *A required diagnostic withheld at an inert `static invariant`* (v0.303) is kept as history: its subject is now rejected outright, which is the stronger form of the signal it demanded.

---

## 8. Changelog, migration, trackers

**Draft changelog bullet:** see `scratch/v0338/W3.md`.

**Migration.** *Newly rejected:* `static invariant(E);` — ten gtest programs in three files (accept → reject; the #4449 warning tests lose their subject and become reject rows; the #4206 accept rows become rejects), three reject files whose diagnostic may change. *Newly admitted:* nothing. *Doc-only:* the `assumed` tier (no program, no metadata value ever written). *Workaround for `static invariant`:* delete the member — the verdict-faithful rewrite, since the clause was inert and checked nothing — or write `invariant(E);` to have the predicate enforced at the class invariant's ordinary points (a behaviour change: unlike the inert form, a false `E` then faults); there was never a proven alternative.

**Trackers to file at the landing** (one `spec-tracking` issue): (i) parser: the #4449 warning at `Parser_Decls.cpp:1697–1716` becomes the rejection ("an invariant member takes no `static` qualifier — write `invariant(E);`"), the `isStatic` field on `InvariantDecl` and its codegen exclusion deleted; (ii) metadata: `MetaContractTier::Assumed` deleted (a format bump if the value is serialized; `ModuleMetadataBuilder.cpp:970–1011`); (iii) the six gtest files as above; (iv) the `Node.hpp:684`/`774–790` comments.

---

## 9. Self-critique (adversarial pass)

(a) *Is deleting a reserved tier a design change at all?* It changes no program, but it contradicts a recorded decision (0043's reservation; the `deferred` row *Contract clauses on `extern` declarations*), which §2 of the process classifies as a design change — hence the proposal. The `deferred` row survives, respelled: the question is still open; only the pre-named answer is withdrawn.

(b) *Does rejecting `static invariant` violate the v0.303 ruling?* That ruling (#4256; the `rejected` row) demanded that an inert clause not pass silently. Rejection is the non-silent outcome it asked for, taken to its end; the row is kept as the precedent.

(c) *Appendix D.2's Contract-proving feature names "the extended prover fragment of §22.16 … what gives the `static` clause qualifier reach".* The invariant was never in that fragment's stated scope (linear arithmetic over parameters, fields, `result` and `old`; null tests; case analysis — discharged "at the declaring body" for `ensures`); re-admitting `static invariant` with it is a widening, which the rationale paragraph and the `retired` row name as the reopening bar.

(d) *Six files, not "0 reach".* The survey's "0 reach" was over programs that use the form for its own sake; the six files are the compiler's pins of the inert behaviour and of the warning, which is exactly what a retirement of an inert form must flip. They are listed by line.

(e) *Former-name check:* none used.
