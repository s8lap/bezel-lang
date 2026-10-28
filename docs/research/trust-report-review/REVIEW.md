# Review of proposal 0131 (the trust-admission report) against spec v0.349 and the r35 compiler

> **Landing note (2026-10-28).** Paths under `scratch/` name working files that were not committed.


Read-only base: `docs/research/trust-report-review/head` = origin/main `293b9278c` (spec v0.349). Compiler: r35 release `bezelc 0.0.0-f6863e62`. Model: `docs/research/soundness/bzcore-l1/bzcore.py`. Everything marked MEASURED was run in this review (`probes/results.md`, `work/sweep/`, `work/bzcore_f1_v3l5.txt`); QUOTED marks text or numbers taken from the tree. Nothing under `docs/`, `tests/`, `src/` was edited; nothing was posted.

## Summary for the owner

**What is stale.** The spec's report paragraph itself is current (v0.334 restated it by clause; v0.327 added the comparer-held clause; v0.333 added 0157's transfer entry). What has drifted is around it: (1) rationale §5.31 still enumerates "field-held, collection-held and frame-held" and omits the comparer-held clause; (2) §28.11's report sentence names "a parameter-, receiver- or field-path-sourced element at its store" with no carve-out for a purely local collection — 0131 §3's silent row, which the compiler now contradicts by over-reporting; (3) the code-generation reference's §4.6 sentence omits the comparer; (4) proposal 0131's body (superseded, never meant to be rewritten) is four trains behind: no comparer-held family, no transfer entry, §3's fresh-store row is an error since v0.346 (0207), §7's line numbers are dead, §9 Q1 was never answered; (5) the README row says "seven cases" and four families. `edits.json` carries the five editorial/clarification edits (dry run clean against a private export); the proposal gets one dated currency note rather than a rewrite.

**What r35 implements.** Three sites, landed 2026-10-13 (`cd21669f8`, #4605 then closed): the field store, the collection store (classified and unclassified receivers), and `Task.FireAndForget` whose argument is a *member call on a reported receiver*. MEASURED over 3,864 corpus programs: 50 lines (36 field, 13 collection, 1 frame), i.e. 1.3 % of programs, so the mode is quiet on typical code — but seven of the 50 are false (enum constants, static method references, a function-typed property read, a static field read, a static-property store, a choice value), one probe class over-reports (a purely local collection), and the following are silent though the spec names them: the spec's own §30.1 example `Task.FireAndForget(ShowAsync(c))` (argument-held sources), every other frame-held form (stored task, bound-not-awaited, Pattern A plain operand, `return await` over an owned field), Family D (form-3 registration residual, `scoped` field quiescence), the comparer-held clause (its constructor is "not yet implemented" on r35), 0157's transfer entry (cases `s28_3_0162`–`0167` were never run: no baseline rows), bare-field-spelled sources (`_peers.Add(_owner)` silent, `_peers.Add(this._owner)` reported), primary-constructor captures and field initializers, function-value invocation results, the `OwningDictionary` key slot, `AddRange`. The warning text is one fixed sentence that names neither the source nor the holder. Ten ready-to-file issues are under `issues/` (eight for the mode, two incidental compiler findings marked "check before filing").

**Compile-time candidates, ranked (each held to "clearly an improvement").**

| # | shape | certainty | compiler has | reach | recommendation |
|---|---|---|---|---|---|
| 1 | **Stop reporting a store into a purely local collection** the source outlives (b07) | provably safe (K2.3 scope comparison; 0131 §3's row) | yes — the store check knows the receiver is a local | corpus: 0 lines today only because r35's classified-receiver arm reports them (b07 shows it) | **clarification edit** (`edits.json`) + issue 04; noise reduction, no verdict |
| 2 | **Stop reporting non-borrow operands** (enum constants, method references, function-typed/value reads, static-storage sources, static holders) | provably no clause | yes — the identifier arm already type-tests; the member-access arm does not | 7/50 corpus lines | **issue 04** (implementation defect, no spec change) |
| 3 | **Report the transfer of a slot-displaced value** under a live slot borrow (#7434's shape, `e02`) | the race is certain on every run of the straight-line form; today 0 lines, accepted, runs | partly — §28.7's window set knows "a local borrow of this slot's current value"; the alias re-sourcing at the move is new bookkeeping | tree: 0 sites; model: 6/169,632 at len 5 | **design change, small: draft 0210** (report-only, 0157's pattern). Not an error: the freeze variant measured in the model rejects 552 programs, 510 never fault |
| 4 | Dominated use through a **field-held** borrow after the source's explicit `Dispose()` in the caller (`e05` shape) | certain per run, but the storage is in the callee — not visible in one body | no (the sink rule hides it; study 0132 is the recorded deferral) | — | **decline** (0132's bar; §5.28's "cannot be scoped by signature") |
| 5 | Unbound temporary passed to a storing callee (`holder.Hold(new Item())`, e06) | certain in the callee, invisible in the caller | no (same ground as 4) | — | **decline** (same; note r35 does not even dispose the temporary — #6775) |
| 6 | Any class **on by default as a warning** | — | — | 36 back-reference stores vs 12 `scoped` class-typed parameters in the same corpus | **decline**: DECLINED row 808's reopening bar ("the `scoped` spelling has become the norm") is not met (3:1 the other way); §5.28's doctrine holds |

**One interaction that needs a ruling note (issue 08).** The #4602 free-at-Dispose amendment frees an object in full at `Dispose()` "where [K5] proves no borrow survives". K5 is body-local; a borrow a callee admitted on trust — exactly the report's register — is invisible to it. MEASURED (e05): r35 keeps the K4.1 fault (`DisposedAccessException`) because its escape marking is conservative and gates the free at run time; the ruling's wording should say the static proof is conjoined with the run-time escape set, or the default build loses the backstop at every site the report names.

## 1. Currency, in detail

Base texts quoted at v0.349.

1.1 **Rationale §5.31, paragraph 1** — "The clauses are recorded in the text — field-held, collection-held and frame-held borrows, plus §28.14's two residuals". Stale since v0.327 (changelog L12092–12094: "The clause joins K4.3 rule 3's enumerated set beside the field-held, collection-held and frame-held clauses … and the trust-admission report's register"). Fix: name the comparer-held clause (edit `r0131-rationale-5.31-comparer`). Editorial.

1.2 **Spec §28.11, the report sentence** (L8243) — "The trust-admission report names a parameter-, receiver- or field-path-sourced element at its store, and an element sourced in the body at a transfer …". Read literally it reports `using var tmp := new Array<Item>(); tmp.Add(item);` (a parameter into a purely local collection), which 0131 §1.2 Family B and §3 both call "statically bounded — no report", and which rationale §5.31's v0.333 paragraph grounds for the body-sourced twin ("admitted by the scope comparison, which bounds the element against its source's scope end"). r35 reports it (b07, MEASURED), and no corpus case pins the silent row. Fix: state the exception (edit `r0131-spec-28.11-local-collection-not-reported`). Classified **clarification** (states the already-implied rule; one reviewer confirms); if the reviewer reads it as deciding a case, it is a ruling — the stricter-gate rule applies. The report paragraph's principle ("an admission that rests on a K4.3 clause") decides it: the local-collection store rests on K2.3.

1.3 **Code-generation reference §4.6** (L674) — the opt-in sentence lists the field/collection stores, the transfer and the abandoned frame but not the comparer. Editorial (edit `r0131-fcg-4.6-comparer`).

1.4 **Proposal 0131 body** — §1.2 names four families (A–D) and excludes nothing about comparers (the clause did not exist); §1.2 Family B's parenthesis and §3's row call the fresh store "§15.4's existing fresh-store *warning*" — an error since v0.346 (0207; MEASURED: r35 still warns, b10); §2.1's paragraph was replaced wholesale at v0.334 (0169, R6; the K3 row list of 0157 lives in clause 3 now); §7's line references (`:7329`, `:8073`, `:8131`, `:11010`, `:18595`) are v0.306's; §9 Q1 ("the measured count") is now answerable — 50 lines / 3,864 programs; Q3's bound-not-awaited predicate is still unimplemented; Q4 ("one report per site, or per source?") is decided in neither spec nor implementation (the fixed text names no source at all). Fix: one dated currency note after the reference-convention line (edit `r0131-proposal-0131-currency-note`); the body stays the v0.306 record, as every superseded proposal's does.

1.5 **README row 167** — "field-held, collection-held, frame-held, the §28.14 registration and quiescence residuals … seven cases". Fix: a *Register changes since* clause (edit `r0131-readme-row-0131-currency`).

1.6 **Not stale** (checked): K4 clause 3 (names comparer-held and the enumeration sentence); the report paragraph's three clauses and the zero-report claim with four exclusions and the task-retention mention (0169); §28.3 field-store clause 3 ("the trust-admission report names every such store … the K4.1 backstop … live in the default build", v0.308's #5028 clarification, consistent with checked-by-default); §28.14's two residual paragraphs; §30.1/§30.5's K4.3 tags; Guide 01 §2's paragraph (names the flag and the default checked build); stdlib §4's comparer paragraph; DECLINED rows 44/46/47/763/772/790/791/808 (each cites the report correctly; none is contradicted by this review — 0210 respects row 46 by leaving the *kill* unreported and naming only the transfer).

1.7 **The 0205/0206/0207 trains and the register.** None of the three removed a trust class or narrowed one into an error: 0205 and 0206 tighten flag-backstopped kills (K4.2 / K5.6 — already excluded from the register) where the use is dominated in one body; 0207 turns §15.4's fresh-store warning — which 0131 §1.2 already excluded as "a statically-certain hazard, not a trust clause" — into an error. The register is therefore unchanged by v0.346; only 0131's §3 row and Family B parenthesis need the currency note. The v0.346 spec example under K4.3 (changelog L15391) is 0131's §3 row 1, consistent.

1.8 **K4's enumeration sentence has one false instance.** §28.3.9 *Scope* clause 1 calls the slot kill by move "governed by … [K4.3], a recorded obligation". 0131 §1.2's exclusion table says it is "reported once, at that holder's store" — true when the surviving borrow is field- or collection-held, false for a *local* borrow of the slot's value (`var b := a.F;`), which has no holder store. #7434 (open) and the model's F1 measure the shape; r35 accepts it silently with the flag and runs the race (e02, MEASURED, exit 0). Fix: draft 0210 (below); the fallback, if 0210 is declined, is a clarifying sentence in the report paragraph naming the gap.

## 2. Implementation state (r35), by clause

Source: `src/ownership/Ownership_Checks.cpp` — `trustFieldStoreCarrier` (`:7119`), `reportTrustAdmission` (`:7151`), call sites `:11863` (field store, `!src` arm), `:12479` (collection store, unclassified receiver), `:12765` (collection store, classified), `:21001` (`Task.FireAndForget`, member-call receiver only). Corpus: `s28_3_0090`/`0091`/`0093`/`0095`/`0096` pass and are banked; `0092` fails (`SlotHandle` unimplemented, #4604); `0094` passes; `0162`–`0167` and `s28_14_0032` have no baseline row (never run).

| clause / form | spec (v0.349) | r35 | probe | issue |
|---|---|---|---|---|
| field-held: plain parameter, `this.x`, property store, interface type, field path through a parameter | report | **reports** | a01, a03, a05, a10, a11, a13 | — |
| field-held: bare field name as source | report | silent | a04 vs a13 | 03 |
| field-held: primary-constructor capture, field initializer | report | silent | a06, a12 | 03 |
| field-held: `_v := make()` | report (clause 2 names it) | silent | a07 | 03 |
| collection-held: `Add`/`Insert`/`Push`/`Enqueue`/`AddFirst`/indexer-set/`Dictionary.Add`, unclassified receiver, parameter-held receiver | report | **reports** | b01, b03, b05, b06, b13 | — |
| collection-held: `OwningDictionary` key, `AddRange` | report | silent | b04, b15 | 03 |
| collection-held: purely local collection | silent (0131 §3; K2.3) | **reports** | b07 | 04 + edit 1.2 |
| 0157 transfer entry | report at the transfer | silent | b08 | 06 |
| comparer-held | report at the ctor call | ctor rejected NYI | b11, b12 | 07 |
| frame-held: `Task.FireAndForget(c.PingAsync())` | report | **reports** | c01, c04 (receiver only) | — |
| frame-held: argument sources (`FireAndForget(ShowAsync(c))`, the §30.1 example), lambda capture | report | silent | c03, c13 | 01 |
| frame-held: task stored (field, owning collection), bound-not-awaited, Pattern A plain operand, `return await` over owned field | report | silent | c05, c10, c06, c08, c09 | 01 |
| frame-held quiet twins (awaited; `using var t; await t`; mint-announcing return; `TaskScope.Spawn`) | silent | silent | c02, c07, c11, c12 | — |
| §28.14 form-3 registration residual; `scoped` field quiescence | report | silent | d01, d02 | 02 |
| §28.14 quiet twins (form 2; scoped construction) | silent | silent | d03, d04 | — |
| exclusions: K4.2 kill, §22.12 static warning, value parameter, local rejected | silent / own diagnostic | correct | e01, e04, a09, a08 | — |
| warning content (source, holder, per-site remedies) | required by clause 2 | one fixed sentence | all | 05 |
| over-reports of non-borrow operands | no clause | 7 lines in the corpus | sweep | 04 |

Incidental compiler findings (not 0131's; duplicate-check before filing): `s28_3_0151` (`expect: reject`) is accepted — a receiver-sourced borrow stored into a settable static property (issue 09); §28.11 clause 3's `owned`-parameter collection rejects a top-level local store (b14; issue 10); the replacing `move … with e` does not parse (e09; the #5923 note); the nullable bare take is rejected on the exception edge (e03); an unbound temporary passed to a storing callee is not disposed (e06 returns 3; #6775).

## 3. "Detect easily at compile time" — per class

The bar: a sub-shape whose stale use is certain on every run, visible in one body with analysis the compiler already has; or a provably safe sub-shape that can stop being reported. Declines respected: DECLINED rows 44/46/47/763/772 (no new K4.3 clauses for spans, slot overwrites, boxed borrows, statics), 790/791 (0157's options A/B), 808 (always-on; bar: measured count showing `scoped` is the norm); `docs/research/certain-faults/REPORT.md`'s "Looked like a hit" list (mutation-during-iteration, terminal accessor, constant `case`, `Send` after `Close`, inverse candidates) — none re-proposed.

**Field-held.** The caller-side certain shape — source disposed, holder used — is certain only given the callee's body (`a.OwnerName()` reads `_owner`), which the sink rule hides from the caller by design (§28.3 clause 3: "The caller's check reads only the callee's signature"; rationale §5.28 Q1; study 0132 is the recorded deferral with its own bar: "0131's measured count vs. `scoped` adoption; an implementation probe; a class-level §9.7 ruling"). The 0131 count is now measured (36:12), which is 0132's first evidence item, not a ground to act. The callee-side shapes are already errors (a local or `owned` parameter stored). **Decline**; the one safe narrowing is the static-storage source (K2's always-live source) — issue 04.

**Collection-held.** Safe narrowing: the purely local collection (candidate 1; edit + issue 04). Certain-fault narrowing: the body-sourced element whose source is then transferred is already the 0157 *report* site; an error there would reopen DECLINED 790/791's ground ("retention is not body-local, since a callee handed the collection as a plain argument may `Remove` or `Clear` it") — the fault is not certain. **Decline.**

**Frame-held.** `Task.FireAndForget(ShowAsync(c))` over a `using var c` in the same body (the §30.1 example) faults whenever the task outlives the scope — likely, not certain (the task may settle before scope end), so it stays a report line, not a warning; the awaited-on-some-paths form is conditional by construction. The spec's own example being silent on r35 is the finding (issue 01). **Decline** the promotion; fix the report.

**Comparer-held.** A scope-bound local comparer retained by a field-held dictionary (b12) is a certain dangle after the constructor, visible in one body *if* the retention is read off the signature as the stdlib says; whether that is a rejection (the §28.3 store rule applied to a retained parameter) or a report is a spec question to decide with the implementation — the constructor is NYI, so nothing can be measured. Noted in issue 07; no proposal.

**§28.14 residuals.** The registration residual's static form is "freezing such fields against `move`", which rationale A11 declines ("flow-marking fields by what initialized them"); quiescence is a teardown property of user code. Neither is a one-body certain fault. **Decline.**

**The slot-kill-by-move transfer (K4.3's recorded obligation with no site).** Candidate 3. The straight-line form `var b := a.F; var c := a.Take(); Domain.Spawn<Q>(W, move c); b.M();` is a certain cross-domain read; the compiler's §28.7 window machinery knows "a local borrow of this slot's current value" but K5 does not re-source `b` to `c` at the move. MEASURED in the model: the taint-based freeze (`work/mutation_f1_slot_displaced_frozen.py`) at locals+fields, vars 3, len 5 → 552 newly rejected, 510 never fault, 0 newly unsound; at len 4 → 12 rejected, all never fault. The sharp rule is not expressible in bzcore L1's state (a slot borrow and a holder borrow carry the same source set); #7432's per-field generation is the prerequisite. **Recommendation:** accept the report-only half (draft 0210 — no verdict changes, zero migration, makes K4's sentence true), route the error to #7434 with the measured over-rejection as its bar.

## 4. Usefulness of the report

- **Actionable?** Partly. Each line names the clause and a complete remedy list, but not *which* source or holder, and the list is not per site (a field store is told to "await the frame to settlement"; every line leads with `SlotHandle`, which is unimplemented, #4604). Issue 05.
- **Noisy?** No: 50 lines over 3,864 spec-shaped programs (MEASURED), 43 of them true back-reference and registry shapes; the seven false ones are a fixable arm (issue 04). Over the docs' own examples the sweep could not run (the doc-example harness wraps fragments with a preamble/env splice and takes no extra flag); the rules corpus is the better sample anyway, being whole programs.
- **On by default?** No class qualifies. The reopening bar for an always-on warning (DECLINED 808) asks for the field-held count to be small relative to `scoped` adoption; the corpus shows 36 plain back-references to 12 `scoped` class-typed parameters. The frame-held abandon over a same-body `using var` is the closest to a hazard and is still conditional.
- **Does the zero-report claim hold today?** No, on four counts at r35 (argument-held frames, Family D, the transfer entry, the comparer), and it never held for the slot-kill transfer (0210). The claim is the mode's purpose (rationale §5.31), so the issues are ordered by what restores it: 01, 06, 02, 07, then 03 (spelling gaps), then 04/05 (noise and text).

## 5. Deliverables

- `edits.json` — 5 editorial/clarification edits; `python3 -I apply_doc_edits.py ../../edits.json` from `work/export/` (a private copy of the five files): **5/5 apply clean**. Change classes: 1.1, 1.3, 1.4, 1.5 editorial; 1.2 clarification.
- `0210-report-a-transfer-of-a-slot-displaced-value.proposal.md` — the one design change recommended (report-only).
- `issues/` — 01–08 ready to file (title, body, labels incl. `campaign:borrow-provenance`; 08 is a `spec-question` on the #4602 ruling, labelled `campaign:using-heads` for the ruling's home); 09–10 incidental, to duplicate-check first (the search API is not reachable from this session).
- `probes/` — 53 programs, `run_all.sh`, `results.md`, `README.md`; `work/sweep/` — the corpus sweep (`files.tsv`, `lines.tsv`, per-file logs); `work/bzcore_f1_v3l5.txt` and the mutation file.

## 6. Self-critique

- The "purely local collection" clarification (1.2) is my reading of the principle plus 0131's own row; the landed sentence does not say it, and a reviewer could call it a ruling. I classified it clarification and flagged the stricter-gate rule; the edit's wording ("not escaping it, whose scope every source outlives") is deliberately narrow.
- Family D, the comparer-held clause and the 0157 entry were measured as *absent*, which is a weak measurement (silence); the src grep confirms there is no emission path for any of them (four `reportTrustAdmission` call sites), so the conclusion does not rest on the probes alone.
- The bzcore run measures the model's taint approximation, not the sharp F1 rule; the 510 pure over-rejections show the approximation is unusable, and I inferred — not measured — that the sharp rule needs a state extension. #7432 is the right home for that.
- The corpus sweep compiled rule cases with `-c`; 1,528 of 3,864 reject by design (the corpus's `reject` tier), so the 50-line count is over the 2,336 accepted programs; the per-clause classification of the 50 lines was by reading each site (listed in `work/sweep/`), not by tooling.
- The doc-example corpus (`tests/conformance/cases`, 671 cases) was not swept under the flag; its harness needs a flag plumb I did not add. The rules corpus and `tests/lit/ownership` were.
- GitHub search was unavailable (REST search endpoint blocked), so issues 01–08 were de-duplicated only against #4605's thread, the changelog's "filed" lists through v0.349 and the DECLINED index, not the live tracker; the coordinator should run the five-source check before filing.
- I did not run the lit suite or any gtest; no claim here depends on them.
