# 0118 acceptance audit — v0.281

This is the committed audit artifact proposal [0118](0118-kernel-absorption-completion.md) §4 specifies, for the v0.281 graduation (the §28 kernel-absorption completion). It collects, verbatim, the four audit records the graduation run produced: the three drafting lanes' per-region audits (deleted-sentence → covering-statement maps, genuinely-new-content lists, deliberate-duplication logs, anchor-set diffs, per-region word counts) and the independent kernel-diff verification that checked the zero-semantic-change claim against them (verdict: **MISSING 0** over 112 individually-verified sentences and all 72 consume-table cells; five divergences, every one closed before landing — the closures are recorded in the v0.281 changelog entry). The composition gate's acceptance scorecard: one-definition-site met (5/5 spot-check); terminology greps clean up to the sanctioned "(formerly …)" residuals; §28 zone word count 40,909 → 38,771 (**−5.2%** — reported, not gated, per 0118 §4 item 4; the proposal's 35–45% estimate was not met because the chapter's examples, tables, and machine-checked rosters — the teaching and audit surfaces — were deliberately kept, and the unification added the ~1,100-word consume table, the glossary, and the router).

Line numbers inside the lane records are v0.280's (their drafting base); the satellite and fix line numbers cited by the verification are as of the wave they audited. The files are appended unedited.

---

# Part 1 — Lane K (§28.3: kernel, tables, freeze cluster)

# Lane K audit — spec §28.3 rewrite (proposal 0118, targeting v0.281)

Region: `docs/Bezel_Language_Spec.md` lines **6462–6930** at v0.280 (from `### 28.3` through the `<a id="sec-the-destructor"></a>` line before `### 28.4`). Replacement: `laneK_region.md` in this directory, ready to splice verbatim over exactly those lines.

## 1. Word count

| | words | lines |
|---|---|---|
| original (6462–6930) | **19,817** | 469 |
| replacement | **18,619** | 499 |
| delta | **−1,198 (−6.0%)** | +30 |

The modest raw delta is structural, not timid: this region *houses* the kernel (frozen, K-numbers stable), the axiom register (frozen), the announcement table (already the single definition site), and now the **new consume table (+~1,100 words of table)** that replaces three artifacts and is what lets the *other* lanes' subsections (§28.6/§28.7/§28.8/§28.11, §15.3, §30.5 satellites) collapse to row citations. Net of the added table, prose removed is ~2,300 words. Statement-site reductions (the metric 0118 §1 actually measures) are in §3.

## 2. Block inventory (pass 1, at 0046-audit granularity)

| Block (v0.280) | Fate in this rewrite |
|---|---|
| §28.3 head | Kept; one added terminology sentence (holder = role, owned/borrowed = classification). |
| Kernel intro | Kept; "as the chapter's subsections are brought under the kernel" migration narration dropped. |
| K1 | Kept; item 4's auxiliary-states run-on paragraph restructured into three bullets (T5); content identical; "kill table's row 1" re-pointed to consume-table rows K3.9–K3.10. |
| K2 | Kept verbatim. |
| K3 twelve-position list + families + principles | **Re-projected into the consume table** (anchor `sec-consume-table`), the single definition site; K3.13–K3.15 kept as principles; K3.13 rewritten in the *marker-free* term. |
| K4 kill table + reading notes | **Deleted; every cell maps to a consume-table cell** (mapping in §4). Bits kept as vocabulary with a recoverability note (each bit read off a table column); "each bit is decisive" consistency proof removed (T1, §6.1); K4.2's closed set + disposal moments now point at the table; slot-kill paragraph deleted as fully covered by K5.6. |
| K5 | Criterion, two failure shapes, override, clauses 5–8 kept; clauses 1–3 shortened to verdict-plus-ground one-liners citing table rows. Rationale §5.1 citation moved onto the criterion head (was on the deleted *explicit-`Dispose()` boundary* paragraph). |
| K6 | Kept; A2-retirement tombstone narration removed (changelog v0.258 carries it); "(v0.258)" bare pin kept on K6.3. |
| K7, K8, K9 | Kept verbatim. |
| Axiom register | All 15 rows kept, ids and Axiom-column rule text unchanged. A7 reason column compressed (v0.209 MarkedTransfer/pinned-test and v0.246-rewording micro-history removed, §6.2); A15 reason compressed (re-listing of the three conjuncts already in the Axiom column removed; v0.266/#3289 pin kept); A15 row's missing trailing `|` fixed. |
| Ownership state of locals | Kept; the ~230-word reassignment/adoption run-on split into **State match** and **Adoption** run-in heads (T5); join paragraph compressed to K7 citation + the two named consequences + the uninitialized-join clause (kept whole); both worked patterns, the marked-arm one-liner, and the Pattern-1 caveat kept. |
| Producers table | **Five call rows collapsed to one row** pointing at the announcement table; `move coll[i]` row's admitted-type enumeration collapsed to a §28.11 pointer (verified: `sec-move-on-a-collection-slot` carries the full enumeration incl. `Span<T>`/`ReadOnlySpan<T>`/`ArraySpan<T>`/`ReadOnlyStringSpan` and value-typed exclusions); null row compressed onto the adoption clause; `??`/if rows compressed onto K7 (each keeps its A7/marked-arm content); all other rows kept. |
| Consuming positions / borrow-consuming list | Kept; "borrow-consuming position" → **borrowing position** throughout; owned-consuming half now cites the consume table's columns. |
| Announcement table | Kept as the caller-side definition site; rows 2 and 5 callee-admission cells compressed to verdict + pointer at the full prose admission (Cite column unchanged); row 4 function-value cell absorbs the deleted producers row's source-set detail; "landed verdict" note trimmed to its normative half ("rows 2 and 3 differ in spelling only: the explicit marker changes nothing") — the seam-Q1 verdict record is in the changelog (v0.259 entry, announcement-table landing). |
| Factory methods and sourceless returns | Kept; "(retiring the treated-owned-with-a-warning classification)" tombstone removed (changelog carries: 0048 train entry, "the retired classification already treated these results as owned"). |
| Calls mint (~1,200-word flow) | Restructured into **8 numbered clauses** (T5); every operative claim kept; "(no axiom — v0.258 retires A2's task/iterable split)" removed. |
| Bare-type-parameter returns | Restructured into numbered clauses: 4 head clauses, 4 caller clauses, 6 callee clauses, plus the Task-family-exclusion paragraph; fence history removed (§6.3); "(Resolved: …)" framing dropped, content kept as callee clause 6; "#1666" dropped, "(v0.235)" pin kept; "(`Take`/`TryTake` through v0.258)" removed (changelog v0.259 carries the Pop rename). |
| Concrete-slot function bodies | Kept, lightly tightened. |
| Iterator and iterable returns | Compressed to a corollary of calls-mint (~200 words); v0.258 retirement narrations removed (changelog v0.258 entry carries both the owned-marker-requirement retirement and the Drain/ReceiveAll affine-carve retirement). |
| Move semantics for owned locals | Restructured (T5); the marker/marker-free placement list now cites the table's Spelling column **once**; the three spelling errors — including the redundant-marker error with its full example list — kept **exactly as strong**; adoption note, both examples, and the `using var` paragraph kept verbatim. |
| Captured owned bindings | Kept verbatim. |
| Borrow lifetime rules | Kept; the ~330-word static-return bullet and the ~400-word borrowed-field-store bullet restructured into numbered sub-clauses (T5); all content kept incl. the domain clause verbatim-in-substance. |
| The consume-site freeze (3 consecutive statement paragraphs) | **Collapsed to one corollary + Scope**: rule = one-line pointer at K5/table; liveness definition now cites K5.5 instead of restating it; the borrow-bearing-record/choice normative clause, the span/`FrozenArray` example, unborrowed-reassignment-stays-legal, the mechanical rewrite idiom, and the whole Scope content (slot exemption, by-move domain routing, the v0.260 inline-array whole-value rule, the §28.14 pointer) kept. |
| Owned-local discharge requirement | Kept; auto-drop/exception-path paragraphs tightened, discharge-form re-list replaced by table pointer; warning-suppression list, ordering, non-applicability list, `finally` composition kept whole. |
| Unbound owned temporaries (+ sub-blocks) | Kept; full-expression-boundary block compressed onto K8.2; case-scrutinee declined-alternative parenthetical replaced by a rationale §5.24 pointer; deferred-activation and returning-a-frame blocks kept with light trims (one consistency-note removal, §6.4). |
| Diagnostics | Kept; A13-retirement narration replaced by the plain current claim (changelog v0.260 carries the retirement). |
| Choosing a disposal form | Kept; the retired-second-carve-out narration removed (same v0.260 carriage). |

## 3. Rule-statement-site reductions (within the region)

- **Freeze criterion: 5 in-region statement sites → 1** (K5) + the table's Frozen column reading it off. (Deleted: kill-table Freeze column + reading notes; "The consume-site freeze" restatement; "The explicit-`Dispose()` boundary"; "Scope of the freeze" restatement — each now a one-line corollary or pointer. The three other chapter sites — §28.7, §28.14, §15.3 — are other lanes'.)
- **Consume-position enumeration: 3 artifacts → 1 table** (K3's list, K4's kill table, K5's placement list K5.1–K5.3 → `sec-consume-table`; K5.1–K5.3 survive as numbered one-liners because the K-numbers are frozen and cited chapter-wide).
- **Site-announced kill set + disposal moments: 2 sites → 1** (K4.2's four-bullet list deleted; the table's K4.2-tagged rows are the definition site; K4.2 keeps the two-conjunct concept definition).
- **Producers call rows: 5 → 1** (announcement table is the classification site).
- **`move coll[i]` admitted types: 2 sites → 1** (§28.11; producers row is now a pointer).
- **K3.13 marker-placement list: 2 → 1** (the table's Spelling column; *Move semantics* cites it).
- **K4 slot-kill exemption: 3 statements → 2** (K5.6 the definition + the freeze subsection's Scope corollary carrying the unique consequences; K4's restatement deleted).
- **Post-kill binding states: 3 sites → 1** (table Post-state column; K1's state definitions remain, as definitions).

## 4. Deleted-sentence → covering-statement table (pass 5)

Every deleted or displaced sentence with normative content, and where it now lives. (Pure connective tissue and verbatim-kept text omitted.)

| Deleted (v0.280) | Covered by |
|---|---|
| K1.4 aux-states paragraph (one flow) | K1 item 4's three bullets, verbatim in substance; "the kill table's row 1" → "consume table rows K3.9–K3.10". |
| K3.1–K3.12 list items | Consume-table Position column, rows K3.1–K3.12, text kept verbatim-in-substance per row. |
| "The twelve positions fall into three families…" incl. "5, 8, 9, 10 are exactly K4.2's site-announced kills, that clause being the one definition site…" | Table family divider rows + reading rules ("The four rows tagged K4.2 are exactly the site-announced kills; this table is the definition site of that closed set and of each kill's disposal moment"). Definition-site ownership deliberately moved K4.2 → table (K4.2 keeps the concept definition). |
| Kill-table row 1 (Dispose/discard: bits, post-state, freeze, moments) | Rows K3.9, K3.10 (post-state, †-unfrozen, moments); bits recoverable per K4's recoverability note. |
| Kill-table row 2 (throw / Pattern B await) | Rows K3.5, K3.8. |
| Kill-table row 3 (every transfer) | Rows K3.1–K3.4, K3.7, K3.12 (frozen, K5.1; "none here" disposal). |
| Kill-table row 4 (drop-on-overwrite) | Row K3.11 (subject = displaced value; RHS-first; K9.5 analog; K5.2/K5.6 split in the Frozen cell). |
| Kill-table row 5 (Pattern A chaining) | Row K3.6 (K5.4 override named in the cell; deferred disposal-moment cell states "no flag the K4.1 backstop can schedule", preserving the row's flag=no bit). |
| Reading note "Row 3's kw column varies … the variation is immaterial: with flag = no the criterion's second conjunct already fails" | K5 clause 1 ("each fails (ii): an obligation-moving kill sets no flag at all"). |
| Reading note "Rows 1 and 2 are exactly K4.2's closed set, and K4.2 remains its one definition site" | Table reading rules + K4.2 (definition site now the table — a deliberate T2 move, not a loss). |
| Reading note "Row 4 is the proof that keyword-visible is load-bearing … row 3 … flag … each bit is decisive in exactly one row, and neither is redundant" | **Removed as design-consistency proof (T1).** The underlying facts survive: K5.2 (flag=yes yet frozen because (i) fails) and K5.1 (kw=yes for `move x` yet frozen because (ii) fails). Changelog draft in §6.1; rationale-destined note there too. |
| Reading note "Row 5 is the table's single override … the landed verdict is unfrozen, for a structural reason…" | K5 clause 4 + row K3.6's Frozen cell. |
| Reading note "Rows 1 and 2's unfrozen is K5.3's verdict … does not hold under the two admissions … K5.7 … K5.8 … each stated at its home" | Table's † footnote in the reading rules. |
| K4 slot-kills paragraph ("outside the freeze entirely … K4.3's domain clause … runtime-checked posture … backstop is real there") | K5.6 (verbatim coverage: subject bound, slot-kill-disposes/backstop-real, by-move → K4.3) + the freeze subsection's Scope clause; K4 keeps a three-word pointer ("outside the freeze entirely (K5.6)"). |
| K4.2's four-bullet closed set with per-kill disposal moments, and the post-kill-state paragraph | Table rows K3.5/K3.8/K3.9/K3.10 columns; K4.2 pointer sentence. |
| "A conditional kill of either kind joins to maybe-consumed (K7) and later use is an error" | Table reading rules ("A conditional kill of any row joins to maybe-consumed (K7.4)") + K7.4's "unreadable". |
| K4 post-state formula "explicitly-disposed when ¬moves ∧ flag, moved-from when moves" | K4's bit-recoverability sentence (obligation-moves ↔ Post-state column) — same content, inverted direction; the verdicts themselves are table cells. |
| K5 clause 1's member list ("moves, marked transfers, owned-slot stores, owned returns — the unmarked mint-announcing returns of K3.2 included — and owning-iterator yield return") | Clause 1's row citation (K3.1–K3.4, K3.7, K3.12) — identical membership; K3.2's mint-announcing note lives in row K3.2. |
| K6.3 "(The former keying to the task family alone was axiom A2, retired v0.258 — the rule is now a pure K6.2 + K6.3 derivation…)" | Removed (T1); changelog v0.258 entry carries the retirement and derivation claim verbatim. "(v0.258)" pin kept on K6.3. |
| A7 reason: "the marker has parsed in these positions since v0.209 (MarkedTransfer was a PrimaryAtom, pinned by `marked_transfer_positions_2080.bzl`) … the v0.246 rewording corrects the row's earlier loose wording, not a v0.246 consequence" | Compressed to "the marker parses in these positions, so the axiom's content is the recorded rejection, not a spelling gap" — the operative claim; history to changelog (v0.246 grammar entry carries the MarkedTransfer landing; §6.2 drafts the residue sentence). |
| A15 reason's re-list "keyed on three conjuncts (the class implements both heads; the member is the protocol's GetEnumerator(); the returned expression is literally this)" | The Axiom column already states all three conjuncts ("a **class** implementing both … may `return this;` from its own `GetEnumerator()` body"); reason keeps "closed three-conjunct admission". |
| Ownership-state join paragraph's re-list of the four meets | K7.1–K7.4 (cited); the two locally-added consequences (K7.2 union note; K7.4 read/back-edge/per-path consequences) kept verbatim. |
| Producers `owned T` call row (+ erasure note) | The single call row + announcement row 1 (whose admission cell carries the erasure note verbatim). |
| Producers plain-`T` call row (incl. "a static callee never reaches this row", row claims, storage-reads note) | The single call row (static rejection, row claims, storage-rows note) + announcement keying rule. |
| Producers function-value invocation row (union; capture-set contribution; adapter-formed references §27.6; parameter-sourced lambda returns; owned/value/runtime/null contribute nothing; "result is never owned"; "factory rule never classifies a function-value invocation"; callee side → Concrete-slot bodies) | Announcement row 4's function-value cell (all sources content, verbatim in substance, incl. adapter parenthetical) + row 4's admission cell (lambda side) + the table keying (fn-value column belongs to row 4, factory rule to the static column). |
| Producers value/runtime call row; move-only call row (discharge enumeration) | Rows 7 and 8 of the announcement table (row 8's admission cell carries the discharge enumeration incl. the holds-no-borrows field bound and consuming match). |
| Producers `move coll[i]` row's admitted-type enumeration (heap types, `Span<T>` view, value-typed exclusion, read-only-span exclusions) | §28.11 *`move` on a collection slot* (verified present, line 7598ff at v0.280, item-for-item); the row keeps the classification + restoration obligation + pointer. |
| "Every admissible static signature is therefore type-announced … and the producers-table row above never classifies a static call" | First half kept; second half superseded (the row it referred to no longer exists — the single call row routes every call through the table). |
| Calls-mint flow sentences | Clauses 1–8, one-to-one; no operative sentence dropped. |
| Bare-T fence history ("the `Task`-family arm of the v0.194 residual was discharged at v0.223", "(v0.258, superseding v0.256's mirrored fence, #3246)", "the iterable arm of the v0.194 residual stays discharged…") | Removed (T1); changelog v0.256 and v0.258 entries carry the full fence-accretion and subsumption record. The operative closure statements (exclusion rule; iterable inhabitant-side closure; "no Task-family value … can inhabit a slot … as lends") kept. |
| "(Resolved: the accumulator shape of §11.8's Sum<T> …)" | Kept as callee clause 6 minus the "Resolved:" framing (the resolution record is the v0.246-era changelog `Sum<T>` entry). |
| Iterator-returns retirement narrations | Changelog v0.258 entry (owned-marker requirement, v0.223 owning-head rejection, Drain/ReceiveAll affine-carve — all named there). |
| Freeze cluster: "The explicit-`Dispose()` boundary (recorded)" criterion restatement; "Scope of the freeze" restatement | K5 + table (criterion); Scope paragraph (uniques kept: legality of unborrowed reassignment, rewrite idiom, slot invalidation posture, by-move routing, inline-array v0.260 rule, §28.14 storage-form pointer). Rationale §5.1 citation moved to K5's head. |
| Freeze liveness "Liveness is the backward dataflow the borrow rules already compute" + "making normative what §9.7's subjection … already implies" | K5.5 (the dataflow statement); the §9.7-implication aside removed as a consistency proof — the normative clause itself ("counts as a live local borrow of every held borrow's source, stated explicitly because stdlib §4.19 … is load-bearing on it") kept. |
| Discharge intro's discharge-form re-list "(… after a discard _ := x; the binding is left explicitly-disposed and after throw x moved-from, K4.2)" | Consume-table Post-state column (pointed at). |
| "This makes a plain var owned local … behave like a using var" / accumulator-intent sentences | Merged into single tightened sentences (auto-drop tail; exception-path tail); recap table unchanged. |
| Unbound temporaries: K8.2 restatement; "(The reject-instead posture of §9.7 was considered and declined here…)" | K8.2 citation; rationale §5.24 pointer (the decline is recorded there — the review's F9 confirms §5.24 argues both stretches). |
| Deferred-activation "the tracing the consume-site freeze already performs … and the transitive judgment the §28.14 sink checks already apply" | Removed as consistency note; the transitive rule itself stated in the same sentence. |
| Diagnostics "(with drop-on-overwrite … since v0.260 … retiring the claim's one recorded carve-out, axiom A13)" and Choosing's "(The former second carve-out … retired at v0.260 …)" | Current-state claims kept ("covers every overwrite — the move-only inline array's whole-value reassignment included, per slot, §15.3"; the Task carve-out note); history at changelog v0.260 (0100–0102 train entry, verified). |
| "Rows 2 and 3 … the visible pair is deliberate — *the explicit marker changes nothing* is itself a landed verdict" | Normative half kept below the table; the verdict-record is the changelog's announcement-table landing entry (seam Q1, verified present). |
| Announcement rows 2/5 callee-admission cell prose | The full admissions in *Calls mint* clauses 5–6 and *Bare-type-parameter returns* callee side (cells keep verdict + named exclusions + pointer; Cite column unchanged). |

**Unmapped residue:** none found. Every deleted sentence maps above; nothing was silently normalized.

## 5. Genuinely-new content that lived only at a formerly-duplicated site — new home

- Kill-table row K3.6's flag-schedule fact → row K3.6's Disposal-moment cell ("no flag the K4.1 backstop can schedule").
- The discard's disposal timing (§17.13) and throw's catch-arm timing (§31.1), formerly in K4.2's bullets → table Disposal-moment cells.
- Producers function-value row's adapter-reference and parameter-sourced-lambda-return notes → announcement row 4's function-value cell.
- The freeze subsection's borrow-bearing-record/choice liveness clause (normative, stdlib §4.19-load-bearing) → kept at the freeze subsection (its only definition site; K5.5 cites the property, the subsection states the counts-as-borrow rule).
- "Unborrowed reassignment stays legal" + the transfer-then-re-borrow rewrite → freeze subsection (only site).
- The inline-array whole-value binding-vs-field split (v0.260) → freeze Scope (only site in region).
- K4's three bits → K4, with the new recoverability note tying each bit to a table column (replaces the per-row bit matrix with no information loss: moves ↔ Post-state, flag ↔ Disposal moment, kw ↔ Spelling).

## 6. Removed narrations — changelog carriage, and drafted v0.281 sentences

Verified carried by the changelog already (no action):
1. A2 retirement + calls-mint unification, v0.223 owning-head rejection retirement, v0.256 fence subsumption (#3246), Drain/ReceiveAll affine-carve retirement — **v0.258 entry** (0096 graduation).
2. v0.246 marker unification, MarkedTransfer atom, hard flip — **v0.246 entry** ("Grammar: the marker unifies as an atom", migration bullet).
3. A13 retirement / whole-value per-slot drop-on-overwrite — **v0.260 entry** (0100–0102 train).
4. A15 / self-returning enumerator — **v0.258 + v0.266 entries** (#3289 pin kept in the register row).
5. treated-owned-with-a-warning retirement — **0048 train entry** ("the v0.60 static-return heuristic is retired … the retired classification already treated these results as owned").
6. `Take`/`TryTake` → `Pop`/`TryPop` — **v0.259 entry** (0098 ii).
7. Static-storage property-getter-only narrowing — **v0.187-era entry** ("Static-storage re-exposure is property-getter-only").
8. #1666 iteration-variable ruling — **v0.235 entry** (pin "(v0.235)" kept in text).
9. The announcement table's seam Q1/Q3 records ("marker-changes-nothing kept visible"; row 6 cannot-arise cells) — **v0.259 entry** (table landing).

Not carried anywhere; drafted sentences for the v0.281 changelog entry (and one rationale-destined note for the composition gate):
- **§6.1 (K4 reading notes):** draft — *"§28.3's K4 kill table and its reading notes are re-projected into the new consume table (`sec-consume-table`), which becomes the single definition site for the twelve K3 positions, the site-announced kill set and its disposal moments, and the per-position freeze verdicts; the reading notes' decisiveness proof (keyword-visible load-bearing exactly at drop-on-overwrite, flag load-bearing exactly at the marked transfers — each bit decisive in exactly one row) moves to the rationale's kernel section, the facts surviving in K5.1–K5.2's grounds and in the table's column-recoverability note at K4."* → the decisiveness proof itself is **rationale-destined** (suggested home: rationale §5.26); flagged for the wave that owns the rationale terminology/kernel note, since docs-lane K does not edit the rationale.
- **§6.2 (A7 reason trim):** draft — *"The A7 register row's reason column is compressed to its recorded grounds; the parsing history (MarkedTransfer a PrimaryAtom since v0.209, pinned by `marked_transfer_positions_2080.bzl`) and the note that the v0.246 rewording corrected the row's earlier loose wording remain recorded here and in the v0.246 entry."*
- **§6.3 (fence history removal):** carried by v0.256/v0.258 entries; suggested v0.281 sentence for grep-trail continuity — *"§28.3's bare-type-parameter fence history (v0.194 residual, v0.223 discharge, v0.256 mirrored fence #3246, v0.258 subsumption) now lives only in those entries; the spec text states the standing rule and its inhabitant-side closure without the accretion narrative."*
- **§6.4:** the two removed intra-spec consistency notes (freeze-tracing note in *Deferred-activation callees*; "§9.7 … already implies" in the freeze) need no changelog carriage (they narrate no history), listed here for the gate's completeness.
- **Terminology (for the changelog's old→new table, T4):** *target-announced* → **marker-free position** (one "(formerly *target-announced*)" parenthetical left at K3.13); *borrow-consuming position* → **borrowing position**; K3.9's list label "target-announced discard" → "marker-free discard"; A4's "target-announced write face" → "marker-free write face". *holder* = role stated at the §28.3 head. No "drop"-as-remove usage existed in this region (that fix is §28.11's lane).

## 7. Anchor-set diff (post-condition)

Original 26 anchors, all preserved, order unchanged; one anchor added:

```
preserved: sec-ownership-kernel, sec-kernel-k1 … sec-kernel-k9, sec-axiom-register,
  sec-ownership-state-of-locals, sec-producers, sec-announcement-table,
  sec-factory-methods-and-sourceless-returns, sec-bare-type-parameter-returns,
  sec-move-semantics-for-owned-locals, sec-captured-owned-bindings,
  sec-borrow-lifetime-rules, sec-consume-site-freeze,
  sec-owned-local-discharge-requirement, sec-unbound-owned-temporaries,
  sec-returning-a-deferred-activation-frame, sec-diagnostics,
  sec-choosing-a-disposal-form, sec-the-destructor
added: sec-consume-table  (required by 0118 T2)
removed: none  (diff of the two extracted sets, modulo the addition: empty — verified mechanically)
```

All 98 distinct spec `#sec-…` targets cited by the replacement verified to exist (in the spec at v0.280, or — for `sec-consume-table` — in the replacement itself); every cited `Bezel_Design_Rationale.md#sec-…` anchor (`sec-owned-not-borrows`, `sec-the-lattice-direction-at-control-flow-joins`, `sec-the-full-expression-boundary`, `sec-owning-collections`, `sec-the-ownership-kernel`, `sec-the-affine-model`) verified against the rationale file.

## 8. Terminology grep of `src/` (before treating renames as doc-only)

- `target-announced`: **0 hits** — doc-only; rename safe.
- `borrow-consuming`: **1 hit**, a comment (not a diagnostic string): `src/ownership/Ownership_Checks.cpp:3726` — *"a local read `r` of an owned local in a borrow-consuming position is a …"*. Report: comment should be updated to "borrowing position" in a trivial follow-up commit; no diagnostic text change, no behavior change. Not done here (docs-only lane).
- `site-announced`, `type-announced`: **0 hits** in src (kept terms anyway).
- `mint-announcing`: **4 hits** in `src/ownership/Ownership.cpp` — two comments and **one diagnostic string** (lines 6986–6989: "a stored view is re-exposed through the mint-announcing … unmarked mint-announcing method return would alias the same …"). Term is *kept* by this rewrite, so no action; noted because wave-2's glossary must keep the spelling stable.
- Related diagnostic strings referencing announcement concepts (e.g. `PureChecker.cpp:852` "the announced consume"; `Ownership_Checks.cpp:6578–6603` "the call site must announce … nothing to announce; remove `owned`") use the *announce* verb generically, not the renamed compounds — unaffected.

## 9. Divergence findings (reported, not fixed)

- **DIV-1 (wording, resolved by the table, flagged for the record):** K3's family-1 label at v0.280 — *"stores into **target-announced** owned destinations (1–4, 12)"* — was loose: positions K3.3 (`M(move x)`) and K3.4 (`move x` transfers) are marker-spelled, not target-announced; the family groups by destination kind, not by spelling. The replacement renames the family *"transfers into owned destinations"* and lets the Spelling column state per-row spelling. No program's acceptance is affected (K3.13 always stated the correct per-position spelling); reported so the gate can confirm the label was descriptive, not rule-carrying.
- **DIV-2 (formatting):** the v0.280 A15 register row lacked its closing `|` (the only malformed row in the region's five tables). Fixed in the replacement; renders identically.
- No kernel-vs-restatement semantic disagreement was found in the region: every restated freeze verdict, post-state, disposal moment, and marker spelling matched its kernel clause cell-for-cell during table construction (the cell-by-cell verification T2 requires). The one candidate (DIV-1) is label-level only.

## 10. Deliberately kept duplications (with reasons)

1. **Announcement-table cells vs. the following prose** (rows 1–8 caller-side verdicts; compressed admission summaries in rows 2/4/5) — the table is the reader's decision procedure (CG §4.3 uses it as such); the prose is the full admission. This is the v0.259 landing's own design, kept.
2. **The freeze one-line corollary at `sec-consume-site-freeze`** — the subsection must remain findable and teachable; it opens with the rule in one sentence + K5/table pointer, then only genuinely-local content.
3. **The domain clause stated in full at the borrowed-field-store bullet** — the review's own load-bearing list (item 10) keeps residual-trust obligations restated at each admission site; this is the site.
4. **K5.1–K5.3 as one-line numbered clauses** despite the table carrying the verdicts — K-numbers are frozen and cited chapter-wide (K5.2 by §28.7/§15.3, K5.3 by §30.5/§30.12, K5.6 by §28.7/§28.11/§28.14), so the numbers must remain resolvable; each is now a verdict-plus-ground line, not a restatement.
5. **The recap table ("Choosing a disposal form")** — EXAMPLES-ONLY lookup surface, the F1 mitigation the review itself prescribes.
6. **Worked examples** — all seven code blocks of the region survive verbatim (Pattern 1/2, marked-arm one-liner, loop accumulator, Wrap/Wrap2, plus inline snippets); the chapter stays teachable.

## 11. Trace pass (pass 4) record

Every code example in the region re-walked against the replacement text — verdicts identical (Pattern 1 accept / Pattern 2 accept; `move cached` arm accept; loop accumulator accept incl. maybe-consumed after the `if`; `Wrap` reject at the `using var` move / `Wrap2` accept; `pool.Add(move item)` under live borrow reject + rewrite accept; `case pool.PopFirst() …` accept with arm-long temporary; `Combine(new Connection("a"), Fail())` unwind-dispose; `Folded(src).Sorted()` accept; chain materializers accept; `Math.Max` borrow-union; `xs.Select(lambda(x) => x.Items)` accept at `U = IEnumerable<Item>`; `Wrap<IEnumerable<int>>(lambda() => Nums())` reject; `Wrap<Object>(lambda() => Work())` reject; `static Pick` reject; `using var t := SumAsync(Nums(3));` reject; `var src := Source(3); await for …` accept; `return Doubled(nums, lambda(x) => scale.Apply(x));` reject; `sink._f := move x;` and family reject — one spelling per site; `buf.AsSpan()` freeze; `prev := cur;` back-edge reject).

CG reference §7.2's ownership cases that resolve via §28.3 re-walked: static `Create` declaration reject (factory rule); fresh-through-plain-return reject (row 4 admission); owned⊓borrowed merge reject and `dest ?? new FileStream(…)` reject; unmarked owned `??` arms = borrow merge consuming neither; marked-arm coalesce accept; the freeze pair; `using var` move-out reject — all verdicts identical. The redundant-`move` error rule and every axiom are unchanged in strength (0090 §Q2 / OWN-F9.1 not touched, per the verification memo's DEAD marking).

---

# Part 2 — Lane M (§28 intro, §28.1–§28.2, §28.4–§28.10)

# Lane M audit — 0118 graduation (v0.281), spec §28 intro/§28.1/§28.2 + §28.4–§28.10

Baseline: `docs/Bezel_Language_Spec.md` v0.280. Region 1 = lines 6367–6461 (`## 28. Resource Management` through the `<a id="sec-local-variables-ownership-and-borrows">` anchor line). Region 2 = lines 6931–7481 (`### 28.4` through the two `sec-collections-do-not-own-elements` / `sec-collection-element-ownership` anchor lines). Replacements: `laneM_region1.md`, `laneM_region2.md` (this directory). No repo file modified; no build run.

## 1. Post-conditions (checked mechanically)

- **Anchor sets**: old∖new = ∅ and new∖old = ∅ for both regions (5 anchors in region 1, 24 in region 2, including the trailing anchors that belong to lane K's §28.3 heading and lane N's §28.11 heading). **Diff empty.**
- **Code examples**: all 24 fenced `bezel` blocks (1 + 23) byte-identical to v0.280. **Every example survives verbatim.**
- **Headings**: the `###`/`####` heading lines are byte-identical to v0.280 (titles and levels unchanged).
- **New-anchor citations**: the only cited anchor that does not exist at v0.280 is `#sec-consume-table` (lane K's new unified consume table; cited twice — §28.7 *Overwrite while borrowed*, §28.8.1 head). All other citations verified against v0.280 anchors.

## 2. Word counts (before → after; code blocks unchanged at 1,013 words total)

| Block | before | after | Δ |
|---|---|---|---|
| §28 intro (pre-table) | 388 | 181 | −53% |
| Disposal-obligations table | 312 | 312 | 0 |
| §28.1 (incl. type requirement) | 960 | 922 | −4% (T5 surgery, content identical by instruction) |
| §28.2 head | 135 | 135 | 0 |
| §28.2 checked disposal | 557 | 551 | −1% |
| §28.4 | 881 | 866 | −2% |
| §28.5 | 491 | 491 | 0 |
| §28.6 head + bullets + examples | 1,206 | 1,101 | −9% |
| §28.6 Who may call Dispose() | 482 | 487 | ≈0 (sentence surgery only) |
| §28.6 Borrowed owning collections | 377 | 325 | −14% |
| §28.6 Methods returning `this` | 177 | 177 | 0 |
| §28.7 | 704 | 610 | −13% |
| §28.8 | 375 | 375 | ≈0 (restructured; examples dominate) |
| §28.8.1 (all sub-blocks) | 2,240 | 2,191 | −2% |
| §28.9 head | 808 | 809 | ≈0 (absorbed the intro's no-GC-unconditional claim, −60 elsewhere) |
| §28.9 trivial-dispose blocks | 1,526 | 1,480 | −3% |
| §28.10 (both blocks) | 1,018 | 1,018 | 0 |
| **Region totals** | **12,637** | **12,031** | **−4.8%** (prose-only −5.2%; 1,013 code words fixed) |

**Why the delta is small here.** This lane's regions are exactly the material the 0046 audit classes OUT-OF-SCOPE (§28.4 both blocks, §28.9 API-surface), EXAMPLES-ONLY (the table, §28.8.1's four idiom sub-blocks), or genuinely-local (the throwable inventory, the who-may-dispose receiver rule, the capture rules, the checked-disposal contract) — plus 1,013 words of examples that survive by hard constraint. The chapter-level 35–45% target is carried by §28.3 (lane K, ~19.8k words) and §28.11–§28.14 (lane N). The review's 90-word sketch for §28.7 (F1) was checked and deliberately **not** followed to the letter: it silently dropped the per-slot exception-edge composition that §28.7 itself marks "stated rather than derived" — precisely the drafted-replacement-drops-non-derivable-content failure mode the verification memo caught in BRD-2/BRD-3/BRD-5. That content survives (compressed ~25%).

## 3. Deletion→survivor mapping (every deleted or displaced normative sentence)

### §28 intro
| Deleted | Survives at |
|---|---|
| Trivial-dispose definition parenthetical (no destructor / no owned fields of non-trivial type / no owning-inline-array fields / trivial base; derived property §3.6) | §28.9 *Trivial-dispose classes* first sentence (unchanged definition site); intro keeps the one-line "provably runs no user code and releases only memory" |
| "Ownership of every trivial-dispose instance is tracked exactly as for any other class … exactly one statically-known owner at every point … compiler inserts a free at that owner's release point" | Intro's retained "exempt from ceremony only, never from lifetime tracking … compiler frees each instance at its holder's release point"; §28.9 trivial-dispose bullet 3 (the release-point enumeration + K1 classification); §28.2 *The compiler-inserted free* |
| "What the classification waives is the ceremony: no `using var` needed, no must-use warning, discard does no cleanup work" | §28.9 trivial-dispose bullets 1–2 and the `using var` sentence (unchanged) |
| "**No runtime lifetime mechanism exists for user-defined classes at all** … no-GC claim unconditional" | **Moved verbatim-in-substance** to §28.9 head ("This tier is exhaustive: …") — relocation, not deletion |
| The "universal contract" sentence (owned location / `using var` / explicit dispose / discard / auto-drop) | The disposal-obligations table's *Classes* row (same five mechanisms, same citations); §28.3 owned-local discharge requirement (K3.14); the trivial-dispose satisfaction clause survives via §28.9 bullet 3 |
| "free — logical disposal followed by reclamation, running no user code" (the free's definition) | §28.2 *The compiler-inserted free* is now the definition site (was already the fuller statement; §28.9 bullet 3 cites it) |
| Intro said "lambda closures" for the runtime-managed tier | Replaced with "function values" — §28.9's own (broader, correct) term; the table row already says "Function values". Flagged here because it is a wording precision, not a 1:1 carry |

### §28.1
| Deleted | Survives at |
|---|---|
| Trailing paragraph "Function-type parameters and return types *do* admit `owned` … part of the function type's identity" | Verbatim duplicate of the two function-type bullets above it (identity rule stated there, §27.1 cited there) |
| Heading narration "(bare-type-parameter arm revised)" and "(superseding the instantiation-site rejection — …)" | Archaeology (see §5 below); the operative cross-reference (§3.6 rule 2 pattern; same split as §28.3's bare-type-parameter rule) is retained in clause 3 |
| "Rejecting these instantiations instead **made**…" (past-tense narration of the superseded rule) | Retained as reasoning, retensed: "Rejecting these instantiations instead **would make**…" (clause 4) — operative claim unchanged |

The type requirement is otherwise a 1:1 re-projection into six numbered clauses; every operative claim of the v0.280 sentence is present (checked clause-by-clause against the original).

### §28.2
| Deleted | Survives at |
|---|---|
| "([§28](#sec-resource-management)'s preamble, …)" citation for the free | §28.2 is now the free's definition site (intro no longer defines it) |
| "no class in the language dangles without this backstop" duplicate phrasing | Sentence retained once (it appeared once; only the preamble citation changed) |

### §28.4
| Deleted | Survives at |
|---|---|
| "(v0.256, **#3245**)" → "(v0.256)" | Changelog v0.256 entry (the review-program defect train names #3245) |
| Heading narration "(the rule [§28.7] has cited, now stated)" | Pure self-narration; the rule statement itself unchanged |
| "(v0.259: the superseded exception is snapshotted into … `SupersededChain` before the drop, …)" mechanism description | §31.2's record-before-drop rule (line 9155, its stated definition site), cited as "its record-before-drop rule included (v0.259)". **Kept**: the N−1 / exception-edge accounting — §22.13's exit-time pass (line 5362) cites "§28.4's no-exception-in-flight accounting" by name, so it must stay here; de-nested into a plain sentence |
| The one-giant-sentence structure of the wrapper-discipline paragraph | Split into sentences; "first and last counts" disambiguated as "no invariant, no `ensures`" against the wrapper-shape list order (the v0.280 sentence's count-mapping was only coherent against that list; content unchanged) |

### §28.5
No deletions (kept whole; it is the tag home for §28.1's derives-tags per the 0046 audit).

### §28.6
| Deleted | Survives at |
|---|---|
| Bullet 1: "may not dispose … may not store in an `owned` location … may not use past the receiver's lifetime" expansion | Compressed to the same three obligations cited as K2.3 + the receiver rule (content identical, ~40% shorter) |
| Bullet 1: "§22.13's exempt `readonly` fields and §22.12 rule 2's admitted mutable statics" gloss | Citations retained (§22.13, §22.12 rule 2); the glosses live at those sections |
| Bullet 2: inline `var taken := move _f; _f := fresh; return taken;` sketch | The `TakeTransactions` example directly below (same shape, line-annotated) |
| Bullet 2: "#1814" | §28.8.1 field-restoration rule ("a terminal move … is rejected, the return edge being itself an unrestored exit") — cited as [§28.8.1](#sec-the-field-restoration-rule); changelog v0.190-era entry carries #1814 |
| Bullet 3: "the bare read lends a borrow sourced at the field's holder, and §28.3's structural site check then classifies by that holder's kind" narration | Compressed into the parenthetical (admissible/rejected holder kinds retained); the rule's definition site is §28.3's borrowed-field store rule (cited) |
| Trivial-dispose application paragraph, ~40% | All three verdicts retained (fresh-store error, A4 getter error + call-bounded-temporary ground, non-`owned` instance-return rejection + type-announced statics) |
| *Borrowed owning collections*: "This is the general receiver rule above at its most consequential instance" (was stated twice — opening and closing) | Stated once, in the opening sentence |
| *Borrowed owning collections*: "so those operations are not part of the surface on a borrow or otherwise" | **Divergence D-M1 — see §7.** Replaced by "deliberately **declare** no value-copy materializers … (stdlib §4.5, which also states the one reachable-by-extension form and its lifetime obligation)". Not a silent normalization — flagged for the composition gate |
| Argument-passing paragraph light trim | All content retained (parameter-sourced borrow, store admission, K4.3 trust on the caller, §28.8 ownership forms) |

### §28.7
| Deleted | Survives at |
|---|---|
| "retiring axiom A13's plain-store carve — the register's tombstone" | §15.3 *Whole-value reassignment* bullet (carries the A13 retirement + tombstone note verbatim); axiom register tombstone (lane K); changelog v0.260 |
| Per-slot mechanism details (same liveness test per slot; moved-from slot contributes nothing / obligation left with the move; restoration obligations discharged by the kill, K9.3 ground; self-assignment no-op) | §15.3 *Whole-value reassignment* states every one of these verbatim — cited as "the definition site for the per-slot details"; "per live slot, in reverse slot order" retained here |
| "(v0.200)" choice pin + "the single-value form applies as written" | §15.3 head ("the owning choice has had it since v0.200, §35.4"); "as a single value — it has no partial states" retained here |
| *Overwrite while borrowed*: "the implicit disposal would silently invalidate the borrow" ground | Retained (unchanged) |
| *Ordering and exceptions* first sentence's RHS-first re-derivation | K9.5 (cited); the consequence sentence retained in one sentence |
| "with each superseded exception recorded on the survivor's `SupersededChain` ([§31.4], v0.259)" | §31.2's record-before-drop rule via "[§31.2]'s ordinary rule" (that rule includes the recording); §28.4's bullet in this lane also states it |
| *Moved-from and null* paragraph | Merged into the head paragraph — all three claims retained (moved-from/null stores without disposal + single-disposal idiom; `using var` non-reassignable; the section's scope list) |

Genuinely-new §28.7 content retained explicitly: **null-guarded, dynamically-dispatched disposal**; the per-slot **exception-edge composition** (multi-value cleanup, abandoned store, RHS as unbound temporary, slots-left-empty unobservable state and its coexistence with §15.1.1's non-nullable invariant, exactly-once per slot object) — §28.7's own text marks this "stated rather than derived", so it is a definition site and survives (compressed, no claim dropped).

### §28.8
| Deleted | Survives at |
|---|---|
| Opening "…to mark ownership transfer across method boundaries" + separate post-example K3.3/K3.2 paragraph | Merged into one opening statement carrying both kernel tags (content identical) |
| "The obligation is satisfied on every path; the warning, not a hard error, marks an implicit drop" | Retained verbatim-in-substance (K3.14 tag kept) |

### §28.8.1
| Deleted | Survives at |
|---|---|
| Head: the positional-admission enumeration ("the RHS root of a binding or assignment, an argument (§22.3), a `case` scrutinee (§35.5), an if-expression branch, an expression-`case`/`when` arm result") | The unified **consume table** ([§28.3](#sec-consume-table), lane K) — the lane instruction's designated single definition site; K3.4/K3's own enumeration (kernel clauses 3–4) carries the same positions. A7 rejection and marker-free-position rejection retained in-line |
| "and at target-announced positions" | Renamed: "at **marker-free positions** (formerly "target-announced" …)" — T4; one first-use parenthetical as licensed |
| `new`-not-throwable paragraph: "#3240"; "That is the reading the Wrong example above **has always encoded**"; "**not evidence that** `new` itself is throwable"; "none of those examples has a call edge … for the hoist to guard" | Self-argument narration (archaeology, §5). The operative reading (the `Wrong` error names only the false branch; in-branch `new` restoration valid) retained in one sentence; the stylistic-uniformity note on the hoisted `new`s retained in one parenthetical. Changelog v0.255 carries #3240 |
| Overload-resolution block: one duplicated sentence ("The field is moved-from from the moment of the move expression's evaluation" restated the preceding comment) | Merged into a single sentence; both examples and the verdict sentence retained |

### §28.9
| Deleted | Survives at |
|---|---|
| Boxed-values bullet: trailing ", v0.255" inside the cross-ref parenthetical | Bare pin dropped as part of a citation list (the three §-cites retained; changelog v0.255 carries the boxing-writes fences). If the gate prefers, restoring the pin is a one-token change |
| Capture paragraphs: restated §28.3 rules ("may not assign to, transfer from, or dispose") | Retained once each with the [§28.3](#sec-captured-owned-bindings) citation (they remain §28.3's rules); the reasoning sentences (why borrow is the only consistent capture form; no-GC-compatibility argument) retained |
| Trivial-dispose head: "(if any, other than `Object`)" phrasing reflowed; "which is why the exemption's boundary … and why it extends no further" defensive framing | Rule + ceremony-only statement + rationale §5.28 citation retained (the task's directed shape); no claim dropped |
| Trivial-dispose bullet 3: the free's "logical disposal followed by reclamation…" definition restated | §28.2 *The compiler-inserted free* (cited in-line); the release-point enumeration retained in full |
| Generic-instantiation closing paragraphs: two paragraphs merged into one | All claims retained (not-elementwise rule, `Array<MyClass>` example, owned-field recursive clause, `OwningBox<T>` per-instantiation verdict, no conditionally-declared structure) |
| "their storage primitive declares a destructor (below)" duplicated by the `OwningBuffer` sentence two lines later | Stated once, at the `OwningBuffer` sentence |
| Intro's no-GC claim **added** here (see §28-intro mapping) | — |

### §28.10
No normative deletions. Kept whole (the pin's axiom home, the cleanup-ordering definition, the adoption/anonymous/compiler-introduced-slot blocks, both examples, the borrow-pin subsection).

## 4. Genuinely-new list (content whose only definition site is this lane's region — verified to survive)

1. §28.2: the checked/unchecked mode contract; the compiler-inserted free's definition (now solely here); the must-emit straight-line use-after-dispose warning; the scoped-callback backstop's checked-mode face.
2. §28.4: the generated-`Dispose()` step sequence; subclass-to-base order; the one-raw-resource-per-owning-wrapper rule and its three residual throw-points; the destructor-throw defined-behavior bullets, including the **no-exception-in-flight N−1 accounting** (cited by §22.13's exit pass — could not be extracted).
3. §28.5: the owned-field restrictions list (tag home for §28.1) and generic-field instantiation check.
4. §28.6: the static-getter-only narrowing's callee-side statement; the mint-announcing stored-view re-exposure remedy (v0.258); the K4.3 "current value, not the holder" precision; axiom A4's content (register home §28.6); the who-may-dispose receiver rule with the declaring-type admission; the element-borrow no-disposal statement; the borrowed-owning-collection surface.
5. §28.7: null-guarded dynamically-dispatched disposal; the per-slot exception-edge composition ("stated rather than derived").
6. §28.8.1: the exit-path precision (fall-through, unreachable exits, no terminal move); the error-message remedy triple; the analysis-throwable inventory (incl. the drop-on-overwrite condition on stores, the compound-assignment classification, `as` and `new` exclusions); the constructor/destructor definite-assignment and discharge specifics.
7. §28.9: the capture rules' teaching statements; move-only-capture borrow form; trivial-dispose definition + coinductive evaluation + release-point enumeration; `OwningBuffer<T>` non-triviality for both element kinds; the API-surface flip guidance.
8. §28.10: establishment-ordered cleanup-stack ordering (incl. finally-at-entry); adoption semantics; the anonymous binding; the compiler-introduced slot; value-type/runtime-managed RHS admission; the borrow-pin.

## 5. Archaeology removals (T1) and changelog carriage

| Removed narration | Where | Already in changelog? | v0.281 entry needed? |
|---|---|---|---|
| "#3245" issue ref | §28.4 wrapper paragraph | Yes — v0.256 entry (review-program defect train) | No |
| "(the rule §28.7 has cited, now stated)" | §28.4 destructor-throw heading | Yes — the v0.NNN entry that stated it (rule now simply stated) | No |
| SupersededChain snapshot-mechanism description "(v0.259: …)" | §28.4 bullet 3 | Yes — v0.259 entry (0097; record-before-drop) | No |
| "(bare-type-parameter arm revised)"; "superseding the instantiation-site rejection" | §28.1 type requirement | Yes — the per-instantiation-erasure entry ("supersedes the v0.193 arm recorded at #1797", changelog line 6765) | No |
| "#1814" | §28.6 bullet 2 | Yes — v0.190-era entry + the terminal-move ruling train (changelog line 7080) | No |
| "retiring axiom A13's plain-store carve — the register's tombstone" | §28.7 | Yes — v0.260 entry (proposal 0101); §15.3 retains the full note | No |
| "#3240"; the has-always-encoded / not-evidence self-argument | §28.8.1 `new` paragraph | Yes — v0.255 entry (#3240) | No |
| ", v0.255" (boxing-writes pin inside a citation list) | §28.9 boxed bullet | Yes — v0.255 entry | No |

Bare pins **kept** (dating rules): v0.256 (§28.4), v0.259 (§28.4 bullet), v0.260 (§28.4 list, §28.7), v0.266 (§28.4 list), v0.258 (§28.6), v0.255 (§28.8.1 ×2).

**Drafted v0.281 changelog sentences** (none strictly required — every removal is already carried; offered for the entry's T1 roll-up):
- "§28's preamble is reduced to an orientation paragraph: the trivial-dispose mechanics it front-loaded now live solely at §28.2 (the compiler-inserted free) and §28.9 (definition, ceremony exemption, release points); the no-GC-unconditional statement moved to §28.9's runtime-managed tier, and the free's definition site is §28.2."
- "§28.1's `owned` type requirement is restructured into six numbered clauses, operative wording unchanged; the superseded-arm narration (v0.193 instantiation-site rejection, revised per-instantiation at the erasure ruling) now lives only in this changelog."
- "§28.4's destructor-throw clause cites §31.2's record-before-drop rule instead of restating the v0.259 snapshot mechanism; its N−1 accounting stays, being the statement §22.13's exit pass cites."
- "§28.7 collapses to the K3.11 corollary plus its genuinely-local content (null-guarded dynamic-dispatched disposal; the per-slot exception-edge composition); the per-slot drop-on-overwrite details are stated once, at §15.3's *Whole-value reassignment* rule."
- "§28.8.1's positional-admission list for `move x` is replaced by a citation of the unified consume table (§28.3)."

## 6. Terminology (T4) in this lane's regions

- **holder** for the role: intro ("its holder's release point"), §28.2 ("the holder's release point"), §28.6 bullets ("the field's holder", "owned by its new holder"), §28.9 bullet 3 ("one statically-known holder", "that holder's release point"), §28.9 API-surface ("the holder's release point"). **owner/owned** left only where it names the classification or the owning party of a specific object ("the actual owner … disposes", "only owners dispose", "the owning object's declaring type") — the glossary's equation (lane K) covers these.
- **marker-free position**: one first-use parenthetical "(formerly "target-announced")" at §28.8.1's head — the only occurrence of the old term in these regions at v0.280.
- **borrowing position** / "borrow-consuming": zero occurrences in these regions at v0.280 (producers-table term, lane K's region).
- **drop**: both uses in these regions (auto-drop, drop-on-overwrite) are disposal-carrying — compliant; no rename needed here.
- **site-announced / type-announced / mint-announcing**: kept as-is per instruction (§28.6 boundary paragraph, §28.6 bullet 2, §28.6/§28.9 type-announced uses).

### src/ grep results (renamed terms in compiler text)
- `borrow-consuming`: **1 hit, comment only** — `src/ownership/Ownership_Checks.cpp:3726` ("a local read `r` of an owned local in a borrow-consuming position is a borrow"). No diagnostic string. Should be retitled "borrowing position" in a follow-up compiler-comment sweep (not this docs train).
- `target-announced`: **0 hits** in `src/`.
- `marker-free`: 2 comment hits (`Ownership_Checks.cpp:754, 1463`) — unrelated sense ("marker-free constructs" = CFG-walk phrasing); no collision with the new term in diagnostics, but worth knowing the word already appears in another sense in comments.
- Adjacent, reported for completeness: the phrase **"announced consume"** appears in one *diagnostic string* — `src/sema/PureChecker.cpp:852` ("`_ := expr` is the announced consume") — and in comments (`LlvmBackend_Setup.cpp:17705`). The discard is a target-announced→marker-free position, so if the glossary retires "announced" for this sense, that one user-visible string needs a follow-up issue; it does not use the exact compound being renamed.
- `holder` already appears in diagnostics consistently with the new role sense (`Ownership.cpp:8630`, `Ownership_Checks.cpp:5128, 5315`).

## 7. Divergences found (reported, not silently normalized)

**D-M1 — §28.6 *Borrowed owning collections* vs stdlib §4.4/#3286 on owning-collection materializers.** v0.280 spec (line 7070): the owning types "deliberately have **no** value-copy materializers (`ToArray` / `Sorted` / `Reversed` / `ToSet` / `ToDictionary`) **at all** … so those operations are **not part of the surface on a borrow or otherwise**." The stdlib (§4.4 Array/OwningArray notes, **v0.266, #3286**; echoed by the changelog's v0.268-era self-correction) rules the opposite for `ToArray`: the owning types *declare* no copy-forms, but LINQ's `ToArray` extension on `IEnumerable<T>` **does resolve** on an owning collection (`pool.ToArray()`), is sound, and returns a *borrowing* snapshot. The spec sentence predates the ruling and overclaims. My replacement says "deliberately **declare** no value-copy materializers … (stdlib §4.5, which also states the one reachable-by-extension form and its lifetime obligation)" — aligned with the landed ruling, zero acceptance change (resolution is §26.5's, not this sentence's). **Gate decision requested:** accept the aligned wording, or revert to the v0.280 sentence and file the contradiction as its own editorial issue. Do not let it land unexamined.

**D-M2 (observation, no text change).** §28.4's destructor-throw accounting is cited by name from §22.13's exit-time pass ("§28.4's no-exception-in-flight accounting", line 5362) — the review's suggestion (F2) to extract the SupersededChain bookkeeping wholesale is not fully executable; only the snapshot-mechanism restatement could move (it did). Lane N / the composition gate should not re-cut it.

**D-M3 (observation).** Intro's "lambda closures" vs §28.9's "function-typed values": the intro's term was the loose one; replacement uses "function values" (the table's and §28.9's term). No semantic change (lambda closures ⊂ function values, and §28.9 was always the normative list).

## 8. Trace results (method step 4)

All 24 in-region examples re-traced against the replacement text: verdicts identical (compiles/warns/errors and the stated reasons). The CG-reference §7.2 cases relevant to §28.2/§28.6/§28.7 re-traced: fresh-through-non-`owned`-return (ERROR, §28.3 row 4 + §28.6 bullet 2), static `Create` declaration rejection (§28.3 via §28.6 bullet 1's citation), `return fresh` out of `using var` (ERROR, A5 — §28.10 unchanged), only-owners-dispose (loop-variable and borrowed-parameter `Dispose()` both ERROR — receiver rule + element-borrow paragraph unchanged), `pool.Clear()`/`Drain` on a borrow (OK — operations list unchanged), field restoration `Wrong` (ERROR naming the false branch — restoration rule + `new`-not-throwable reading unchanged), catch-binding store (ERROR — borrow-receiver list unchanged), freeze at `pool.Add(move item)` under a live borrow (ERROR — K5/lane K; §28.7's local-freeze cross-reference unchanged). **No verdict changed.**

## 9. Deliberate duplications kept (with reasons)

1. §28.4's N−1 supersession accounting — cited by name from §22.13 (D-M2).
2. The K4.3 domain/trust clause echo at §28.6's store-admission bullets — the review's own load-bearing list (item 10) keeps per-site trust statements where the author meets the obligation.
3. §28.6 bullet 2's mint-announcing parenthetical beside §28.3's calls-mint rule — the two rules meet head-on at this bullet (non-`owned`-return error vs type-announced transfer); one sentence here prevents the looks-contradictory reading.
4. §28.9 trivial-dispose bullet 3's release-point enumeration — the free-insertion site list is this section's own content (each site merely cites its home).
5. The disposal-obligations table — EXAMPLES-ONLY lookup surface; the review's F1 mitigation explicitly keeps the recap tables.

---

# Part 3 — Lane N (§28.11–§28.14, satellites)

# Lane N audit — spec §28.11–§28.14 (v0.280 lines 7482–7806) + three satellites, for v0.281 (proposal 0118)

Deliverables: `laneN_region.md` (drop-in replacement for lines 7482–7806, from `### 28.11` through the `<a id="sec-iteration-2"></a>` line), `laneN_satellites.md` (three old→new pairs + one no-edit verdict), this audit. No repo file was modified; no build was run. Line numbers cited as "old L*n*" are 1-based within the region (spec line = 7481 + n).

## 1. Word counts

| Unit | v0.280 | lane N | Δ |
|---|---:|---:|---:|
| §28.11 | 3,352 | 2,874 | −478 (−14.3%) |
| §28.12 | 734 | 736 | +2 (one three-word phrase swap, "dropped" → "with no await"; otherwise verbatim — 0046 classed it OUT-OF-SCOPE, and T1 found no archaeology in it) |
| §28.13 | 982 | 823 | −159 (−16.2%) |
| §28.14 | 3,803 | 3,723 | −80 (−2.1%) |
| **Region total** | **8,704 (wc)** | **7,997 (wc)** | **−707 (−8.1%)** |
| §15.3 satellite bullet | 467 | 365 | −102 |
| §30.5 satellite ¶s (2) | 290 | 208 | −82 |
| **Grand total** | 9,461 | 8,570 | **−891 (−9.4%)** |

The region's small percentage is structural, not shirking: §28.14 is the region's biggest section and is almost entirely §28.14-specific content (0046 audit: the section head DERIVED, the v1 bounds AXIOM A11, the class-typed `scoped` block and type-system block its own) — the only absorbable §28.14 material was the register roster and one erratum narration; §28.12 is disposal-execution mechanics with no ownership restatement (0046: OUT-OF-SCOPE ×2); §28.13's bullets are the compiler-known type's own semantics. The genuinely redundant §28.11 per-family prose took the bulk of the cut (−14.3%) while both tables, every example, and every genuinely-new rule survive.

## 2. Anchor set diff (post-condition)

Old region anchors = new region anchors, **diff empty** (verified by set-diff):
`sec-element-slot-policy`, `sec-borrowing-collections`, `sec-owning-collections`, `sec-move-on-a-collection-slot`, `sec-compatibility-between-borrowing-and-owning-forms`, `sec-asynchronous-disposal`, `sec-shared-ownership`, `sec-scoped-callback-parameters`, `sec-iteration-2`. (The §28.11 head anchors `sec-collections-do-not-own-elements` / `sec-collection-element-ownership` sit at spec lines 7480–7481, one line above the region boundary, and are untouched.)
Every markdown link in the replacement resolves against v0.280's `<a id>` sets in spec/stdlib/rationale (checked mechanically), except the two deliberate `#sec-consume-table` links in the satellites, which land with lane K's table.

## 3. Deleted-sentence → covering-statement map (region)

Every deletion below maps to a landed covering statement; nothing was found unmapped, so nothing is filed as divergence. "Table" = §28.11's operation table (unchanged but renamed cells); "stdlib" citations are confirmed in §5 below.

### §28.11 head (old L3–L9)
| Deleted | Covering |
|---|---|
| "The collection's `Dispose()` is a no-op — borrowing collections are trivial-dispose (§28.9), their backing storage part of the instance's single allocation and released with it by the instance's one free — and elements are **not** disposed" | kept, relocated into the *Borrowing collections* intro (same content, one statement instead of two); also table `Dispose()` row + §28.9 (spec 7374 cites `#sec-borrowing-collections` for exactly this fact — the citation still lands on it) |
| owning bullet's member enumeration ("Every owned-ingress member takes ownership — `Add`/`Insert` on the `IOwningCollection<T>` conformers, `Push`/`Enqueue` on `OwningStack<T>`/`OwningQueue<T>` (the order-disciplined pair declares no `Add`/`Remove`/`RemoveAt`; its egress is `Pop`/`Dequeue`/`Try*`/`Drain`); the indexer setter disposes the previous slot value…; `Remove`/`RemoveAt`/`Clear` dispose…") | table store/`Remove`/`Clear` rows + stdlib §4.2/§4.5–§4.12 declarations; the pair's no-indexer/no-`Add` fact survives once, at *`move` on a collection slot* (old L130, kept verbatim) and in stdlib §4.11's declarations |

### §28.11 element slot policy
| Deleted | Covering |
|---|---|
| "…in this revision, because a user-expressible borrow-storing primitive needs its own borrow-lifetime rules (storing borrows in fields is otherwise rejected, §28.3) — that design is deferred, recorded in the rationale" | rationale §5.20 *The deferred half* carries the full reasoning; replacement keeps "deferred, recorded in the rationale (§5.20)" with the anchor made explicit |

### §28.11 Borrowing collections (old L41–L46 → one paragraph)
| Deleted | Covering |
|---|---|
| "`Clear()` removes all elements without disposing them" | kept ("`Clear()`, `Remove`, and `RemoveAt` remove without disposing") + table `Clear()` row |
| "`Add(item: T)`, `Insert(index, item: T)`, indexer-set accept borrowed references and store them" | table store row (borrow column) + stdlib §4.1 `ICollection<T>.Add(item: T)` (l.1494), `IList<T>.Insert(index: int, item: T)` (l.1502), `Array<T>` surface (l.1581–1586) |
| "`Remove(item)` removes the first equal element (by `Equals`) and returns `bool`. `RemoveAt(index)` removes the element at the given index and returns `()`." | stdlib §4.4 `Remove(item: T): bool` / `RemoveAt(index: int)` (l.1584–1585); equality-keyed removal is the signature's documented behavior there |
| "Neither method disposes the removed element… the caller continues to own it (if it had ownership) or it goes out of scope (if it was borrowed transit)" | kept compressed ("the element's holder, if any, keeps its obligation") + table `Remove`/`RemoveAt` row |
| "The indexer-get produces a borrowed reference whose source is whichever owner the borrow originally came from. The collection is a transit point, not a source." | kept verbatim-in-substance (holder rename) |
| store rule tail "since v0.243 over the full class-typed set, trivial-dispose, `Task`-family, and other non-trivial-dispose arguments alike, the hazard stated per family there" | §15.4 (spec 2989) states the full class-typed trigger set and the per-family hazard normatively; replacement keeps "(v0.243; the hazard is stated per argument family there…)" |
| "(A `DisposeElements()` escape hatch let a borrowing collection's holder dispose elements en masse; it is retired with axiom A8 — the vouch it required is exactly what the owning family expresses structurally.)" | **T1 extraction** — axiom register A8 tombstone; changelog v0.213 (the full retirement record, #2075); rationale §5.1(2)/§5.3; §28.6's own "the retired element-borrow carve is recorded at the axiom register's A8 tombstone" (v0.270 relocation) |
| "For collections where ownership of values *is* the natural model, the owning form (next subsection) is preferred." / "(keys value-like or shared)" / "no manual pass exists or is needed" | head's choose-the-form sentence; "its keys are value-like" kept; cascade sentence + "need no explicit element pass" (kept) |

**Defect fixed, reported (editorial):** old L52's lead-in "The recommended pattern when a class semantically owns a collection-of-resources **but uses a borrowing collection type**:" contradicted its own example, which declares `OwningArray<Transaction>` — a leftover from the pre-v0.213 (A8-era) version of the example. New lead-in drops the stale clause. No semantic change; the example is untouched.

### §28.11 Owning collections (old L71–L93 → four paragraphs + kept lines)
| Deleted | Covering |
|---|---|
| member bullets `Add(owned item: T)` / `Insert(index: int, owned item: T)` / indexer-set requires owned / `Remove`+`RemoveAt` dispose / `Clear()` disposes / `Dispose()` disposes then frees | table rows + stdlib §4.2 (`Add(owned item: T)` l.1519, `Clear()` "disposes every element" l.1520, `Remove(item: T): bool` "disposes the removed element" l.1522, `set(owned)` "takes owned T; disposes previous value" l.1528–1530, `Insert(index: int, owned item: T)` l.1531); one-line summary kept in the new intro |
| "Indexer get `coll[i]` produces a borrow of the slot's value… Reading does not transfer ownership; `move coll[i]` is the explicit-take form (§28.8.1 — see below…)" | table indexer-get row ("borrow out (source: the collection)") + kept clause "(`move coll[i]` is the explicit-take form, below)" |
| drop-on-overwrite paragraph (old L81) details ("the same drop-on-overwrite discipline §28.7 applies to owned-field and owned-local assignment (K3.11): in both forms the compiler disposes the displaced live value before installing the new one… the same live-value guard §28.7 applies to a moved-from field or local") | §28.7 (K3.11 clause, live-value guard, spec 7091) — the kept sentence is the pointer; the slot-level runtime-skip detail survives at *Indexer setter on a moved-from slot* (old L134, kept verbatim — now the single definition site of the slot guard) |
| property-setter paragraph (old L83) details: "declares an owned-taking accessor… (`set(owned)` with body `=> _cache := value;` §23.3 — the default borrow-taking `value` cannot be assigned to an owned field per §28.3's state-match rule)"; "Writing one anyway… is legal but redundant — …§28.7's blessed dispose-then-assign legacy shape"; "The auto-disposal is a property of the owned-field *assignment*, not of the setter wrapper" | §23.3 (accessor forms); §28.3's state-match rule (its home); §28.7's own sentence "An explicit `field.Dispose();` before the assignment remains legal and behavior-preserving — `Dispose` is idempotent" (in-section, verified) and §28.6's "the §28.7 dispose-then-assign legacy" (spec 7057); the assignment-not-wrapper point survives as "behaves identically through the ordinary owned-field assignment in its body" |
| `PopAt`/`PopFirst`/`PopLast` signature bullets (old L87–L88) | stdlib §4.5 (l.1696–1701), §4.7 (l.1922–1926), §4.11 `Pop`/`Dequeue` (l.2331, 2354); the semantic content ("removes and returns owned in a single step, closing the slot; indices shift; dictionary entry removed") is kept as prose |
| `Drain()` bullet (old L89) | **kept in full** (drain semantics are on the genuinely-new list): drains-as-iterated, empty after full iteration, partial drains only what was yielded, owned item + §28.3 discharge + `using var u := move item` + must-use auto-drop — all present verbatim-in-substance |
| default-iteration paragraph (old L93) "(where `coll` has static type `OwningArray<T>`, etc.)… To consume, iterate over `coll.Drain()`, which has static type `IOwningEnumerable<T>` and yields owned values" | kept compressed, with the both-implemented disambiguation delegated to §29.2 (its definition site: "the desugaring uses the `IEnumerable<T>` path — default iteration is borrowing") |
| disposal-order paragraph (old L115) first sentence ("every element it still holds is disposed first, then the backing storage is freed") | kept, re-worded as the table's presentation; the order fact (reverse-insertion / implementation-defined) and the concrete ordered/unordered type membership survive verbatim-in-substance |

### §28.11 `move` on a collection slot + Compatibility
Kept verbatim except one rename (old L157 "(the original owner of each element is the caller's responsibility once drained)" → "(once drained, ownership of each element is the caller's responsibility)" — owner-as-role removal, same claim). This subsection is now the **single definition site** of `move coll[i]`'s admitted-type enumeration; lane K's producers-table row and §28.8.1 point here (their edit, per assignment).

### §28.12
Verbatim except: "(e.g. `using var t := obj.DisposeAsync();` dropped)" → "…with no await)" — "drop"-reservation hygiene, zero semantic content (the sentence already says "abandoned without an await").

### §28.13
| Deleted | Covering |
|---|---|
| "The ownership model is single-owner:" | reworded "gives every disposal obligation exactly one holder (K1.7)" — same claim, role word unified, kernel tag added |
| sendability: "The **synchronized** disjunct admits real synchronization machinery — for the data-owning locks, interior sendable by instantiation (§30.12)" | stdlib §1 synchronized-fact definition + §30.12 (data-owning locks' own section, cited from the roster's §6.11) |
| "the **immutable** disjunct is sound with no machinery at all: no writer can exist after construction (stdlib §4.19's recorded construction obligations, for the disjunct's first member `FrozenArray<T>` — deep immutability rather than machinery)" | stdlib §1 immutable-fact definition + stdlib §4.19's construction obligations (named by the deleted text itself as the carrier); the `Shared<T>`-side half (count + storage tolerance) **kept** — it is §28.13's own soundness statement |
| "The **self-distributing** exclusion keeps out `ChannelSender<T>` and `SharedEventSource<T>`, though both are internally synchronized: each already has its own sanctioned distribution channel — senders are minted per producer (§30.15); static reachability is the shared source's (§22.12 rule 5, stdlib §6.5) — and `Shared<T>` wrapping would add a second (the grounds at each fact line)" | stdlib §1's self-distributing-fact definition ("the type already has its own sanctioned distribution channel, so `Shared<T>` wrapping is excluded — it would add a second") + the grounds on the §6.5/§6.9 fact lines — the deleted sentence's own parenthetical says the grounds live there |
| "Growth is governed by stdlib §1's fact discipline: a derived set grows only by a **fact ruling**, which states the facts it grants and confronts each derived-set consequence in its record" | kept compressed ("the set grows only by a fact ruling under stdlib §1's discipline"); full statement at stdlib §1 |
| "the divergence protections the former per-set clause carried (form-(i) membership never implying membership here, and vice versa) are now structural, held by the **self-distributing** and **displacement-unbounded** facts, the two recorded reasons the sets ever diverged" | **T1 extraction** — changelog v0.245 (0087 Q2: "the old vice-versa protection is carried structurally by the two facts that ever made the sets diverge") |

### §28.14
| Deleted | Covering |
|---|---|
| register bullet detail "including `EventSource<T>.Add`/`AsyncEventSource<T>.Add`" | stdlib §6.5 declarations (`Add(scoped handler: …)` l.3949/3960, `Subscribe(scoped …)` l.3937–3956) |
| register bullet 5's keying detail "declared on `ISharedEvent<T>` or `SharedEventSource<T>` (`Subscribe`/`Add` — whether the receiver's static type is the pair itself or a generic parameter constrained to it)" | stdlib §6.5 *Capture-freedom at every subscribe position* (l.4043: "keyed on the resolved callee… a constrained generic receiver included"); replacement keeps "keyed on the resolved callee (a constrained generic receiver included)" so the formation-bar paragraph's "every register entry" still names a closed set |
| v1 bound 2(a) narration "(v0.273, correcting the v0.180 bound that recorded the implementation's method-wide freeze as normative; that bound was listed among choices 'the original text left silent', but this corner was not silent, and the method-wide reading contradicted this section's own *'Until `L`'s scope exits'*)" | **T1 extraction** → "(v0.273)"; the full erratum story is the changelog v0.273 entry (#3318), verified present (three-to-one split, mis-filing, guides-were-right, Guide-01 re-pin) |
| 2(a)'s "The widening only *admits more* programs, so it breaks no program accepted under the former bound (the recorded-widening-path posture of §15.4)" | **T1 extraction** — changelog v0.273 Migration ("Newly accepted: one shape… Newly rejected: nothing") |
| "*(Liftable: … the field's owner as the contributed source…)*" wording | kept with "the field's **containing object**" — owner-as-role removal, same referent |

## 4. Genuinely-new content, surviving explicitly (the assignment's list + what the pass found)

1. **The store rule's residual-trust clause** (§28.11 *Storing a borrow*) — near-verbatim, including the domain clause and its no-backstop statement.
2. **Drain semantics** — full normative statement kept.
3. **Disposal order** — reverse-insertion (ordered) / implementation-defined (unordered), with the concrete type membership; kept (the stdlib states order only for `OwningBuffer<T>` and `Channel<T>`, so §28.11 is this rule's only definition site).
4. **Per-slot conservative tracking** for non-constant indices + per-kind restoration operations — kept verbatim (K9's own text points here for it).
5. **The borrowing-family store rule's fresh-temporary admission** (literal-only-rejection asymmetry + §15.4 warning) — kept with pointers.
6. `move coll[i]`'s **admitted/excluded type enumeration**, the span delegation semantics, bounds-check, restoration forms — kept verbatim; single definition site.
7. **Indexer setter on a moved-from slot** (runtime empty-slot detection) — kept verbatim; single definition site of the slot-level guard.
8. §28.12 in full (two-phase lifecycle, sync-implicit-discharge, no-destructor warning) — untouched.
9. §28.13's handle semantics, Get()'s accessor dispensation, cycles, `Weak<T>`, aliasing, the immutable-disjunct reclamation-soundness clause, the `Shared<Mutex>` idiom — kept.
10. §28.14 everything except the two extractions and the roster compression: bounding forms 1–5, ordered testing, admission + freeze, registration-lifetime residual, quiescence, call-bounded default, formation bar, both deferred-activation shapes, scoped construction, the asymmetry note, the v1 limitation, all eight A11 bounds (7 listed + the promoted self-capture note, kept), type-system interaction, §30.11 relation.

## 5. Stdlib signature confirmations (every signature deleted from §28.11 prose, grepped in `Bezel_Standard_Library.md`)

| Deleted from spec prose | Confirmed at stdlib |
|---|---|
| `Add(item: T)`, `Clear()`, `Remove(item: T): bool` (borrowing) | §4.1, l.1494/1495/1497 |
| `Insert(index: int, item: T)` (borrowing) | §4.1, l.1502; `Array<T>` l.1583 |
| `RemoveAt(index: int)` (borrowing) | `Array<T>` l.1584 |
| `Add(owned item: T)` | `IOwningCollection<T>` l.1519; `OwningArray<T>` l.1678 |
| `Insert(index: int, owned item: T)` | `IOwningList<T>` l.1531; `OwningArray<T>` l.1680 |
| indexer `set(owned)` "disposes previous value" | `IOwningList<T>` l.1528–1530 |
| `Remove` disposes / `RemoveAt` disposes / `Clear` disposes (owning) | l.1522; `OwningArray<T>` l.1681–1684 |
| `PopAt(index: int): owned T` | l.1696 (+ reference impl l.1790) |
| `PopFirst(): owned T` / `PopLast(): owned T` | l.1697–1698 (`OwningArray`), l.1922–1923 (`OwningLinkedList`) |
| `Drain(): IOwningEnumerable<T>` | l.1701, 1926, 2070 (dictionary: `IOwningEnumerable<DrainedEntry<K, V>>`), 2281, 2336, 2359 |
| `Push(owned item: T)` / `Pop(): owned T` (`OwningStack`) | l.2329, 2331 |
| `Enqueue(owned item: T)` / `Dequeue(): owned T` (`OwningQueue`) | l.2352, 2354 |
| `Try*` egress forms | l.1699–1700, 2333–2334, 2356–2357, 2069, 2280 |
| `OwningStack`/`OwningQueue` declare no `Add`/`Remove`/`RemoveAt`/indexer | §4.11 declarations l.2327–2361 (absent from surface) — the fact also survives in-region at *`move` on a collection slot* |
| `Subscribe(scoped …)`/`Add(scoped …)` incl. `EventSource<T>.Add`/`AsyncEventSource<T>.Add` | §6.5 l.3937–3960 |
| shared-event capture-free admission, resolved-callee keying, constrained-generic inclusion | §6.5 l.4043 |
| `TaskScope.Spawn(scoped body: () -> Task)` | §6.10 l.4544 |

## 6. SETS / FACTS constraint report (§28.13 — what the checks forced me to keep)

`scripts/check_docs.sh --quick` on the **unmodified** tree: all checks clean; `SETS: clean (2 enumeration(s) of the synchronized set across 2 document(s); all agree on 8 members)` — the two sites are spec §28.13 (line 7734) and stdlib §2.16 (line 1040). Reading the check's source shows two hard constraints on my region:

1. **The SETS check anchors on the spec's enumeration**: if no "synchronized set … currently `X`, `Y`, `Z`…" run (3+ backticked names within 700 chars) exists in the spec, the check FAILS with "lost its anchor". Deleting the roster is therefore not an option; **the enumeration stays and the prose around it shrank instead** (this is the constraint the assignment predicted).
2. **The FACTS check (3b)** requires (a) the exact prose formula phrase `declaring the **synchronized** or the **immutable** fact and not the **self-distributing** fact` verbatim in the spec, and (b) exactly three tagged parentheticals matching `derived membership \((…)\), informative — currently … — stdlib`, one per set. Both preserved byte-for-byte in the rewritten bullet.

Both checks were **re-simulated over a patched spec** (region spliced in at 7482): formula phrase present; three parentheticals, one per set; shared roster = the 8 members unchanged; SETS finds the spec enumeration at (new) line 7715. Green.

## 7. Removed narrations and their carriage (T1)

| Narration removed | Where the history verifiably lives |
|---|---|
| §28.11 `DisposeElements()`/A8 retirement story | changelog **v0.213** (full record, #2075 sweep); register A8 tombstone; rationale §5.1(2), §5.3, §5.26 |
| §28.13 former per-set-clause divergence protections | changelog **v0.245** (0087 Q2) |
| §28.14 2(a) v0.273-correcting-v0.180 erratum + recorded-widening-path sentence | changelog **v0.273** (#3318 entry, incl. Migration accounting); bare "(v0.273)" pin stays |
| §15.3 (satellite) "one acceptance change… through v0.259" + closing A13-history italic | changelog **v0.260** (the 0101 study record, in full); A13 tombstone; rationale §5.26 |
| §28.11 borrow-storing-primitive deferral reasoning | rationale **§5.20** *The deferred half* (now cited by anchor) |

Bare pins kept where they date a rule: v0.243 (store warning), v0.260 (§15.3 per-slot rule), v0.273 (§28.14 freeze end, ×2), v0.235/#1666, v0.268, v0.255/#3085 etc. in untouched text. **No carriage gap found — every removed narration is already in the changelog/rationale, so no relocation text needs adding; the v0.281 entry needs only the sentences in §9 below.**

## 8. Terminology sweep + src/ grep results

- **Headline fix (F3.2)**: operation-table cells `drops the copy`/`drops the reference`/`drops the borrow` → `removes …`; `drops all` ×3 → `removes all` ×3. "drop" in the region now appears only in disposal-carrying senses (auto-drop, drop-on-overwrite, "drops the last handle").
- **holder**: table indexer-get cell "the element's original owner" → "original holder"; borrowing prose "whichever owner the borrow originally came from" → "whichever holder…"; §28.13 opener → "exactly one holder (K1.7)"; §28.14 liftable note "the field's owner" → "the field's containing object"; §28.11 compatibility "(the original owner of each element…)" → ownership-of-element phrasing.
- **borrowing position / marker-free position / site-announced**: my region contains **zero** occurrences of "borrow-consuming position" and **zero** of "target-announced" (both live in lane K/M's §28.3 and in §29.2/§30.5 outside my satellites' scope); "site-announced" occurrences in §28.14 are the kept K4.2 term — unchanged by design.
- **src/ grep** (diagnostic strings, then comments):
  - Quoted diagnostic strings: **no hits** for "target-announced", "borrow-consuming", "drops the copy/reference/borrow" — the renames are doc-only at the diagnostic-string level. (`ExprTypecheck_Calls.cpp:13443` "silently drops the write" is a §23-area message about property writes, unrelated sense, no action.)
  - Comments (report-only, no compiler work in 0118): `src/ownership/Ownership_Checks.cpp:3726` uses "borrow-consuming position"; `src/codegen/LlvmBackend_Disposal.cpp:141` uses "drops the borrow"; `Ownership_Checks.cpp:754/1463` use "marker-free" already in a *different* sense ("marker-free constructs") — worth a note in the rename's changelog old→new table so grep-trails disambiguate.
- **Residuals reported for the composition gate** (not mine to fix): CG reference l.552 table cell "drops the borrow" (CG §4 sync is T6's); stdlib l.2324/2347 comments "drops all elements; disposes nothing"; Guide 05 l.21 "drops the reference"; §30.5's "Disposing an unconsumed `OwningTask<T>`" uses "dropped" for abandonment (outside my satellite scope); §28.14's kept "binding it and dropping it undisposed-to-settlement" (deferred-activation ¶) — flagged for lane K's glossary to reconcile or the gate to sweep.

## 9. Drafted v0.281 changelog sentences (lane N's contribution to the entry)

> **§28.11 (T3).** The per-family prose collapses onto the element-slot tables it was disclaimed as presenting: the borrowing and owning member-by-member listings become pointers at stdlib §4 (every deleted signature verified present there), and the surviving prose is the genuinely-new normative content — the store rule's residual-trust clause, drain semantics, element-disposal order, the moved-from-slot indexer guard, and per-slot conservative tracking. `move coll[i]`'s admitted-type enumeration now lives once, at §28.11 *`move` on a collection slot*; §28.3's producers row and §28.8.1 point here. One stale lead-in ("…but uses a borrowing collection type", contradicting its own `OwningArray` example since the v0.213 rewrite) is corrected. The `DisposeElements()` retirement narration leaves the section — the v0.213 entry and the A8 tombstone carry it.
> **§28.13/§28.14 rosters (T3).** The sendability bullet keeps its normative formula and the machine-checked membership parenthetical — `check_docs.sh`'s SETS check anchors on the spec-side enumeration and 3b on the tagged parenthetical, so both stay — and sheds the per-disjunct grounds narration onto the stdlib fact lines that the bullet itself named as the grounds' carrier; the former per-set-clause divergence history is the v0.245 entry's. §28.14's stdlib retention register compresses to the signature rule plus per-position pointers (the roster is the standard library's); v1 bound 2(a) keeps its rule and its v0.273 pin, the erratum narration being the v0.273 entry's.
> **Terminology (T4, this region's share).** §28.11's operation table now says **removes** in its non-owned columns — *drop* is reserved chapter-wide for disposal-carrying discard, and the same table's owned column disposes. *Holder* is the role word throughout §28.11–§28.14 (the table's "original owner" cell, §28.13's opener, two prose sites). Old→new for grep-trails: "drops the copy/reference/borrow" → "removes the …"; "drops all" → "removes all"; "the element's original owner" → "original holder". (Compiler comments at `Ownership_Checks.cpp:3726` and `LlvmBackend_Disposal.cpp:141` still use the old terms; `Ownership_Checks.cpp` also uses "marker-free" in an unrelated sense — noted for trail continuity.)
> **Satellites.** §15.3's whole-value-reassignment bullet points its freeze/drop-on-overwrite statement at the consume table and §28.7, keeping its own per-slot facts (reverse slot order, the slot's-object freeze extension, per-slot-state travel, the self-assignment no-op, the static carve); its pre-v0.260 acceptance-change narration is the v0.260 entry's. §30.5's Pattern-B context enumeration and the bare-`var` scope note become consume-table/§28.3 pointers plus their Pattern-B-specific residue; §29.2's yield-ownership subsection was audited and left as-is (its restatements are its own rules' operative content; K6 appears there only as cross-references).

## 10. Divergence findings & flags (none blocking; all report-only)

1. **No unmapped deletion** — every cut maps in §3; zero semantic delta intended or found on re-trace.
2. **Example/looks-right re-trace**: BankAccount, UseEach, ReplaceAt, ReportEach(+Generic), the `Shared<Connection>` block, ProcessAsync — verdicts identical under the new text. CG §7.2 collection cases (element-borrow `Dispose` rejection; `pool.Clear()` cascade; fresh-store warn on `tasks.Add(…)`; literal hard-reject; `move`+restoration; consume-site freeze at `pool.Add(move item)`) — each still traces to the same rule at the same (or pointed-to) site; verdicts identical.
3. **Flag (wording slip, not fixed silently)**: §30.5's deleted scope note said a named-owned-binding read in binding position "is a borrow unless marked **`owned`**" — the binding-position transfer marker is spelled `move` (§28.3, §22.3). The satellite replacement states the direction-neutral form ("an unmarked read … staying a borrow"); the slip is noted here rather than corrected as if it were mine to rule on.
4. **Deliberate duplications kept, with reasons**: (i) the domain clause inside §28.11's store rule — the review's own load-bearing list item 10 ("each site is where an author meets the obligation") and 0046's K4.3 classification keep it per-site; (ii) the store rule's structural-check + residual shape parallel to §28.3's borrowed-field rule — same posture stated at both admission sites, per the same reasoning; (iii) the freeze verdict summarized inside §15.3's satellite (one clause) because the slot's-object extension is unintelligible without it; (iv) the spec-side synchronized-set roster beside the stdlib's — machine-forced (SETS anchor + FACTS parenthetical), reported per assignment; (v) `Drain()` semantics in §28.11 while stdlib §4.5 presents them per type — §28.11 is the definition site, the stdlib's are its per-type presentations (the table-authority sentence covers this).
5. **Lane-K dependency**: the two `#sec-consume-table` links (satellites a, c1) resolve only once lane K's table lands; composition gate should verify.

---

# Part 4 — Independent kernel-diff verification

# v0.281 kernel-diff audit — independent adversarial verification

Scope: spec §28 zone, HEAD~1 (`v0.280`) lines 6367–7806 vs working tree lines 6367–7808,
plus the three satellites (§15.3 line 2902; §30.5 lines 8375, 8419) and every out-of-zone
citation into the rewritten structures. Read-only; no builds, no edits.

Method: I did **not** start from the lane tables. I mechanically split both zone texts into
prose sentences (1,080 old / 1,022 new), normalized markdown links and emphasis, and fuzzy-matched
every old sentence against the whole new zone (`sentdiff.py`, `sentmap.json`). **179 old sentences**
fell below the 0.72 similarity floor — the candidate-deletion set. I inspected all 179 by heading and
verified **112 of them individually** against the new text (weighted to rules; pure narration
sampled), plus a cell-by-cell audit of the new consume table, an anchor diff, a code-block diff,
and a cross-file citation sweep. The lane tables were read only afterwards, for comparison.

---

## 0. Headline counts

| | |
|---|---|
| Old zone / new zone | 40,909 → 38,409 words (−6.1%) |
| Anchors lost | **0** (`comm` over `<a id=…>`: zero removed, one added — `sec-consume-table`) |
| Code blocks in zone | **35 old / 35 new, byte-identical set** (zero added, zero removed, zero altered) |
| Candidate deletions examined | 179 flagged / 112 individually verified |
| **MISSING** (a rule with no covering statement anywhere) | **0** |
| **DIVERGENCE** | **5** (D1–D5; two of them are the lanes' own D-M1 and the §30.5 slip) |
| **SOFT** (covered, but attenuated or over-claimed) | **5** (S1–S5) |
| Consume-table cells audited | 72 (12 rows × 6 columns) — 71 sourced to an old sentence, 1 soft (S1) |

**Verdict: the zero-semantic-change claim holds inside the zone.** No rule, verdict, post-state,
disposal moment, freeze verdict, or admitted-type enumeration was lost. The five divergences are
(a) three classes of *stale citation left outside the zone* by the rewrite, (b) two *deliberate
corrections of landed defects* that must be recorded rather than absorbed by a "zero semantic
change" claim.

---

## 1. Consume-table cell audit (task 2)

Every cell of `sec-consume-table` was sourced to an old statement. Old kill-table row → new rows:
row 1 → K3.9/K3.10; row 2 → K3.5/K3.8; row 3 → K3.1–K3.4, K3.7, K3.12; row 4 → K3.11; row 5 → K3.6.
K3 list items 1–12 → the twelve Position cells verbatim (only K3.3 re-split: "an `owned` argument
**under the call-site marker**" → Position "an `owned` argument" + Spelling "`move x` at the call site").

**Bit recoverability.** New K4 replaces the old `moves`/`flag`/`kw` columns with a recovery recipe.
I checked the recipe on all twelve rows:

| bit | recipe | rows where I could not reproduce the old value |
|---|---|---|
| obligation-moves | Post-state column (*moved-from* ⇔ moves) | none (K3.11 explicitly exempted by K4's own "outside the post-state question by its subject") |
| flag-set | Disposal-moment column | none — K3.6's cell says outright "no flag the K4.1 backstop can schedule" |
| keyword-visible | Spelling column ("a keyword, the marker, or the `Dispose()` call — not a bare declared-type announcement, and not reassignment") | **K3.6** — see S1 |

**No cell states anything the old text did not.** Spot-checks of the least-obvious cells:

- K3.5 disposal moment "at the await, on all outgoing edges including fault and cancel" ← old K4.2 bullet 4 verbatim.
- K3.8 "after the matching catch arm completes, or at termination if uncaught" ← old K4.2 bullet 3 verbatim.
- K3.11 Frozen "frozen at a binding (K5.2); slot overwrites are outside the freeze (K5.6)" ← old row 4 `Freeze: frozen` + old K4 slot paragraph.
- K3.11 moment "after the RHS is evaluated (RHS-first; K9.5 is the slot analog)" ← old row 4 verbatim.
- K3.12 Spelling "none (§15.3's scope note…)" ← old reading note ("K3.12 unmarked under §15.3's scope note").
- The four K4.2 tags = {K3.5, K3.8, K3.9, K3.10} = old "5, 8, 9, and 10 are exactly K4.2's site-announced kills". ✔
- Family membership: F1 = {K3.1–K3.4, K3.12} = old "(1–4, 12)"; F2 = {K3.5–K3.10} = old "(5–10)"; F3 = {K3.11} = old "(11)". ✔

Deleted reading notes → coverage:
- "Row 3's `kw` varies … the variation is immaterial" → **superseded structurally**: row 3 is now six rows each carrying its own Spelling and its own `frozen (K5.1)`. CONFIRMED-COVERED (strictly better).
- "Rows 1 and 2 are exactly K4.2's closed set, and K4.2 remains its one definition site" → new reading rules + K4.2 both say the table is the definition site; internally consistent. CONFIRMED-COVERED.
- "Row 4 proves `kw` load-bearing; row 3 proves `flag` load-bearing; neither bit redundant" → **removed**; reconstructible from K5.1 ("fails (ii)") + K5.2 ("fails (i)"). CONFIRMED-COVERED (see S4).
- Old K4.2's four disposal-moment bullets → the table's Disposal-moment column, all four moments present. CONFIRMED-COVERED.

---

## 2. Divergence findings

### D1 — DIVERGENCE (out-of-zone, dangling): Appendix C still cites "the K4 **kill table**'s row N"

The kill table no longer exists; its rows were renumbered into the consume table. Three live
Appendix C rows still point at it:

- spec:11901 — "…([K3.10](#sec-kernel-k3), the [K4](#sec-kernel-k4) **kill table's row 1**)" → now row **K3.10**.
- spec:11918 — "…joining the consume-site freeze (the [K4] **kill table's row 3**; [K5.1])" → now rows **K3.1–K3.4, K3.7, K3.12**.
- spec:11982 — "…a site-announced kill (the [K4] **kill table's row 2** — no marker, no freeze, disposed-flag backstop; [K3.5] + K4.2)" → now rows **K3.5/K3.8**.

`grep -rn "kill table" docs/` finds no other live referrer (changelog and proposals are pinned
history and correctly keep the old name). **Not listed in any lane's audit table.** Fix: re-point
the three rows at `sec-consume-table` and its K-row ids.

### D2 — DIVERGENCE (out-of-zone, terminology): 15 sites still use *target-announced*, 9 of them citing K3.13 as its definition site

Inside the zone the term is retired to two parentheticals (see §6 below). Outside it, the term is
still used normatively at spec lines **2949, 3692, 4512, 5681, 5726, 5782, 7924, 7934, 8381, 9121,
10259, 10313, 11481, 11918, 11919**. Nine cite the definition site by tag or link — e.g. 8381
"the position is **target-announced** ([K3.13](#sec-kernel-k3)'s set)"; 10259 "in the [K3.13] sense";
9121 "the keyword is itself the target announcement ([K3.13])" — and K3.13 now defines
**marker-free position**, naming *target-announced* only as "(formerly …)". A15's own register row and
A4's row were updated in-zone; their out-of-zone counterparts were not. This is presumably wave-2's
"router/CG/Guide sync" work, but §29.2, §30.5, §31.1 and §35.5 are *satellites of §28*, not routers,
and lane N's satellite set covered only §15.3 and §30.5×2. Flagging so the gate does not assume
wave 2 has it.

### D3 — DIVERGENCE (deliberate defect fix): D-M1, §28.6 value-copy materializers

Lane M's flag is correct and its rewording is the right alignment; see §4 below for the full verdict.
It is a divergence from the *zero-semantic-change* claim (it corrects a landed normative sentence),
not from the stdlib.

### D4 — DIVERGENCE (deliberate defect fix): §30.5's "borrow unless marked `owned`" slip

Lane N's flag is correct; see §4 below. The correction is right; the process obligation is to record
it, not to absorb it.

### D5 — DIVERGENCE (cross-doc pointer, minor): §3.6's trivial-dispose row still names "§28 intro" as a definition site

`docs/Bezel_Language_Spec.md:524` (the §3.6 derived-type-property table) reads
"**trivial-dispose** | [§28](#sec-resource-management) **intro**, [§28.9](#sec-runtime-managed-types) |
classes whose disposal provably runs no user code and releases only memory: no destructor, no …".
The rewrite moved the four-conjunct definition **out of the intro entirely** (into §28.9), and the
new §28.9 paragraph does **not** carry the reciprocal "this is a §3.6 derived type property" tag the
old intro carried. Net: §3.6 points at text that no longer holds what §3.6 reproduces, and the
§28→§3.6 back-link is gone. Fix: drop "§28 intro" from the §3.6 row, or restore the derived-property
tag at §28.9's opening.

---

## 3. Soft findings (covered, but worth recording)

**S1 — K3.6's `keyword-visible` bit is not reproducible from K4's stated recipe.**
Old kill-table row 5 gave `kw = **yes** (the form)`. New K3.6 Spelling cell reads "none — the form
announces", and K4's recovery recipe admits only "a keyword, the marker, or the `Dispose()` call —
not a bare declared-type announcement, and not reassignment". "The form announces" is in neither
list. **Immaterial to every verdict** (K3.6's freeze is K5.4's vacuity override, and `flag = no`
already decides the criterion — the old table said so itself), but K4's "each bit is recoverable from
it" is, as written, false for one cell. One-word fix: `"none — the `return await` form announces (keyword-visible)"`,
or add "or a keyword-bearing form" to the recipe.

**S2 — §28.11 lost "on the types that declare them".**
Old: "`Remove`/`RemoveAt`/`Clear` dispose the affected elements **on the types that declare them**",
with the explicit negative "OwningStack/OwningQueue declare none of `Add`/`Insert`/`Remove`/`RemoveAt`".
New reads universally ("`Remove`/`RemoveAt`/`Clear` dispose what they remove"), with the caveat
delegated ("which types declare `Add`/`Insert` and the indexer … is declared in the standard library").
I verified the delegation resolves: stdlib §4.11's `OwningStack<T>`/`OwningQueue<T>` declare neither
`Remove` nor `RemoveAt`, and do declare `Clear`/`Drain`; the "no indexer" half survives verbatim at
§28.11 *`move` on a collection slot*. CONFIRMED-COVERED-BY-DELEGATION, but the sentence over-reads.

**S3 — §28.13's derived-set divergence protection is stated nowhere normative now.**
Deleted: "the divergence protections the former per-set clause carried (form-(i) membership never
implying membership here, and vice versa) are now structural, held by the **self-distributing** and
**displacement-unbounded** facts". It is a direct arithmetic consequence of the two formulas that
*are* stated (§22.12 rule 2(i) = synchronized ∧ ¬displacement-unbounded; §28.13 = (synchronized ∨
immutable) ∧ ¬self-distributing), and lane N extracted it to changelog v0.245. CONFIRMED-COVERED
(derivation), recorded because it is the only such claim in the zone with no surviving prose home.

**S4 — the "each bit is decisive in exactly one row" non-redundancy argument is removed.**
Reconstructible from K5.1/K5.2's "fails (ii)"/"fails (i)". Rationale-grade content; CONFIRMED-COVERED.

**S5 — `SupersededChain` dropped by name at §28.4 and §28.7.**
Both now say "the §31.2 `finally`-supersession discipline, its record-before-drop rule included".
§31.2 (spec:9157) states the record-before-drop rule and §31.4/§22.9/§22.13 name the field
(11 occurrences file-wide). CONFIRMED-COVERED. The N−1 / N accounting itself is fully preserved at
§28.4, only re-punctuated — important, because §22.13:5362 cites "§28.4's no-exception-in-flight
accounting" by name (lane M's D-M2 observation is correct).

---

## 4. Verdicts on the three flagged items (task 4)

### D-M1 — lane M's §28.6 materializer alignment: **CORRECT — accept the aligned wording, with one nit, and record it as a correction**

Old (v0.280, §28.6 *Borrowed owning collections*):
> "The owning collection types deliberately **have no** value-copy materializers (`ToArray` / `Sorted` /
> `Reversed` / `ToSet` / `ToDictionary`) **at all** … so those operations are **not part of the surface
> on a borrow or otherwise**."

That is **false as landed**, and has been since v0.266 (#3286). Stdlib §4.4 (line 1653):
> "`OwningArray<T>` **declares** no copy-forms of its own — no `Reversed`, no `Sorted`, no `ToArray` …
> **That is a statement about the type's declared surface, not about what is reachable on a receiver of it.**
> … `ToArray` is different: §5 declares it as an extension on `IEnumerable<T>`, which `OwningArray<T>`
> implements … so `pool.ToArray()` **resolves and is sound**, returning a **borrowing** snapshot."

and stdlib §4.5 (line 1721) states the form, the borrow-slot classification, and the lifetime obligation
("this array must outlive every use of the snapshot"), closing with "The same reachability and the same
obligation hold for `ToSet()`, `ToDictionary(…)`, **and every other §5 operator** applied to an owning receiver."

Lane M's replacement — "deliberately **declare** no value-copy materializers … (stdlib §4.5, which also
states the one reachable-by-extension form and its lifetime obligation)" — adopts stdlib §4.4's exact
declared-surface framing and points at the right home. **Verdict: correct alignment.** Two riders:

1. **Nit:** "the **one** reachable-by-extension form" undercounts. §4.5 extends the reachability and the
   obligation to `ToSet()`, `ToDictionary(…)` "and every other §5 operator". Read it as
   "the reachable-by-extension **forms** and their lifetime obligation".
2. **Process:** this is not editorial. It replaces a normative claim ("not part of the surface … or
   otherwise") whose acceptance content the stdlib refutes. Under `docs/proposals/spec-change-process.md`
   it is at minimum a *corrected-in-passing* item and arguably a clarification; it must appear in the
   v0.281 changelog entry citing v0.266/#3286, not be absorbed by the zone's zero-semantic-change claim.
   Lane M's "gate decision requested" is the right posture. Acceptance itself is unchanged (resolution
   is §26.5 rule 1's, not this sentence's).

### The §30.5 slip — **the deletion is right; the right fix is to state the marker positively and record the correction**

Old §30.5 (HEAD~1:8417), *scope note*:
> "a read of a named owned binding in a binding position is a borrow **unless marked `owned`** — §28.3."

`owned` is not a binding-position marker at all — §28.1 does not admit `owned` on a local, and the
transfer marker in binding/assignment position is `move` (`var y := move x;`, K3.4; §22.3). The old
sentence therefore described a form that does not exist. New §30.5 (8419):
> "(`var x := <owned producer>` is an owned-consuming binding and a Pattern B await is such a producer —
> §28.3's binding rules govern, **an unmarked read of a named owned binding staying a borrow**.)"

Assessment: **content is covered** — §28.3 *Ownership state of locals* states the positive half
("A read of a *named owned binding* on the right-hand side is a **borrow** unless it carries the explicit
transfer marker — `var y := move x;`"). **No program's verdict changes** (the described form was
unwritable). But the new wording keeps only the *negative* half and drops the "what marking does make it
a transfer" answer that the sentence existed to give.

Right fix, two parts:
1. **Text:** name the marker rather than only negating it — e.g. "…§28.3's binding rules govern: an
   unmarked read of a named owned binding stays a borrow; the transfer spelling is `var y := move x;`
   ([K3.4](#sec-kernel-k3))". This restores the sentence's job while fixing the marker name.
2. **Process:** record it. It is a stale-marker-name correction surviving the v0.246 marker unification,
   so it belongs in the v0.281 changelog's corrected-in-passing bullet with the §22.3/K3.4 citation —
   not folded into a restatement-only claim. Lane N was right to flag rather than fix; the gate should
   rule and record.

### DIV-1 — lane K's family-1 relabel: **FAITHFUL, and it silently corrects an old defect**

Old: family 1 = "**stores into target-announced owned destinations** (1–4, 12)".
New: "**Family 1 — transfers into owned destinations** (K3.1–K3.4, K3.12)".

Membership is identical ({1,2,3,4,12} = {K3.1,K3.2,K3.3,K3.4,K3.12}) — verified cell by cell.
The relabel is **more accurate than the old one**, exactly as lane K says: K3.3 (`M(move x)`) and
K3.4 (`var y := move x;`) are marker-spelled, not target-announced, so the old label mis-described two
of its own five members; and "stores" mis-described K3.3 (an argument bind) and K3.4 (a binding). Per-row
spelling now lives in the Spelling column, which is the correct single definition site. **No acceptance
change** (K3.13 always carried the correct per-position spelling). One rider: like D-M1 and the §30.5 slip,
this is a *correction of a landed label*, so DIV-1 belongs in the changelog's corrected-in-passing record,
not merely in a lane audit file. Lane K's "reported so the gate can confirm the label was descriptive,
not rule-carrying" is the right framing — and I confirm it was descriptive: no rule anywhere keys on the
family names (the freeze keys on K5's criterion, the marker on K3.13/the Spelling column).

---

## 5. Cross-lane seams (task 3)

| Seam | Verdict |
|---|---|
| K's *Producers* row `move coll[i]`: "Which collection types admit the form — and the per-family exclusions — is stated once, at [§28.11](#sec-move-on-a-collection-slot)" | **CONFIRMED-COVERED.** N's §28.11 *`move` on a collection slot* is byte-identical to v0.280: admitted set (`OwningArray<T>`, `OwningDictionary<K,V>`, class-typed `OwningBuffer<T>`, class-typed `Span<T>` with its delegation + bounds-check + `span[i] := null` restoration, `[N owned]T?` and `[N owned]T`) and every exclusion (borrowing collections, `ArraySpan<T>`/`ReadOnlyArraySpan<T>`, `ReadOnlyStringSpan`, value-typed `OwningBuffer`/`Span`, `ReadOnlySpan<T>` at any T, and the four indexer-less owning types) all present. |
| M's §28.7 → `sec-consume-table` ("not statically frozen ([K5.6]; the consume table's frozen column)") | **CONFIRMED.** K3.11's Frozen cell says "frozen at a binding (K5.2); slot overwrites are outside the freeze (K5.6)" — exactly what the citing text promises. |
| N's §15.3 satellite → `sec-consume-table` ("governed by the consume table … and §28.7 like any other overwrite — at a local, frozen …; at an instance field, §28.7's owned-field overwrite regime") | **CONFIRMED.** K3.11 delivers both halves. The deleted "not statically frozen (K5.6)" and "invalidated under the runtime-checked posture" are both re-stated at §28.7 *Overwrite while borrowed* (verbatim posture sentence) and at §28.3 *Scope*. |
| N's §30.5 satellite (8375) → `sec-consume-table` ("the consume table's positions … not re-listed here") | **CONFIRMED.** Every item the deleted inline list named maps: owned-slot/`owned`-field store → K3.1; `owned` argument → K3.3; `return` under `owned T` → K3.2 (and the "wraps `owned T`" instance is *kept* as Pattern-B-specific); owning `yield return` → K3.7 (§29.2 linked from the cell); discard → K3.9; the owned binding forms are kept in place. |
| §15.3 satellite | Deletions all covered except two records: "This is the one acceptance change the v0.260 rule makes" and the A13-tombstone rationale paragraph → changelog + the A13 tombstone row. Also drops the "exactly as the owning choice's reassignment is (§35.4)" analogy — a lost cross-link, no rule. |
| §30.5 satellite (8419) | See D4. |

---

## 6. Terminology sweep inside the zone (task 6)

| Term | Old zone | New zone | Verdict |
|---|---|---|---|
| `target-announced` | 10 | **2** | Both are the allowed "(formerly *target-announced*)" parentheticals — K3.13 (§28.3) and §28.8.1's `move` opening. **2 lanes used their one allowance; within budget.** ✔ |
| `borrow-consuming` | 2 | **0** | ✔ (replaced by "borrowing positions") |
| "drop" for non-disposing removal | 4 slot-policy cells ("drops the copy", "drops all", ×2 each) | **0** | ✔ — all four now "removes …". Remaining `drop` uses are all disposing: `auto-drop` (17), `drop-on-overwrite` (23), "drop of" (K3.14's lint), "drops the last handle" (§28.13). |
| owner-used-as-role | 37 `owner` + 10 `owner's` | **28 `owner` + 4 `owner's`**; `holder` 26 → **39** | Partially converted. The remaining role-uses are §28.6's *Who may call `Dispose()`* / *Borrowed owning collections* ("out from under its owner", "lets the owner dispose", "the actual owner (the `using var`, the owned local, or the object whose field this is)") — three sites, all in M's region, all reading naturally and all pre-existing. **Not a violation to block on; flagging the count.** The role-critical sites *were* converted: "at its **holder's** release point" (×3), "the field's **holder**", "the storage's **holder**", "the field's **containing object**". |
| `kill table` | 6 in zone | **0** in zone | ✔ — but 3 live referrers survive in Appendix C (D1). |

---

## 7. Trace sample (task 5)

All 35 fenced code blocks in the zone are byte-identical between v0.280 and the working tree, so any
verdict change would have to come from a rule change. I re-traced ten of the trickiest against the
*new* text only:

| # | Example | Verdict under new text | Governing rule, new site |
|---|---|---|---|
| 1 | Pattern 1 / Pattern 2 loop-and-branch accumulator (`var x: Stream;` equalized to borrowed / to owned) | identical — compiles; the `using var scratch` branch still does **not** equalize | adoption clause + K7.1–K7.4 (§28.3 *Ownership state of locals*, restated as "K7.1–K7.4" — every sub-clause present at K7) |
| 2 | `ReplaceAndReturn` (build-first, `move _resource`, restore, return) | identical — compiles | K9.2 restoration + §28.8.1; `new` still outside the throwable inventory |
| 3 | `LeakTransactions` (`return move _transactions;` through non-`owned`) | identical — ERROR | §28.6 bullet 2, unchanged; the "terminal `return move _f;` rejected on its own return edge" clause survives (its `#1814` citation became a §28.8.1 anchor link) |
| 4 | `Wrong` / `WrongOrder` field restoration | identical — ERROR naming the false branch only | §28.8.1 *Exception paths are not exempt*; the `new`-not-throwable reading sentence survives, only the illustrative hoist gloss was cut |
| 5 | `TransferToHelper` (`Helper.TakeOwnership(move _resource)` then restore) | identical — ERROR (exception edge is an unrestored exit) | §28.8.1 *Interaction with overload resolution*, re-punctuated into one sentence, both clauses intact |
| 6 | `var s := buf.AsSpan();` freezing `buf` against `FrozenArray<T>.From(move buf)` | identical — ERROR while `s` is live | §28.3 *The consume-site freeze*, borrow-bearing-record clause verbatim |
| 7 | `pool.Add(move item); var b := pool[…]; b.Configure();` (the freeze's mechanical rewrite) | identical — the pre-transfer-borrow shape is rejected, the rewrite compiles | K5.1 (F1 rows frozen) — the rewrite sentence is kept verbatim |
| 8 | `UseEach` — default iteration borrows, `pool.Drain()` + `using var c := move conn;` | identical | §28.11 slot-policy `indexer-get/default iteration` owned column + the `Drain()` paragraph (kept in full) |
| 9 | `ReplaceAt` (`move pool[index]` → `pool[index] := newValue` → `old.Dispose()`) | identical — compiles; the moved-from slot's setter disposes nothing | §28.11 *Indexer setter on a moved-from slot*, byte-identical |
| 10 | Pattern B `using var x := await someTask;` / bare `var x := await someTask;` / `_ := await …;` | identical — all three compile; bare `var` carries the must-use warning if undischarged | §30.5 satellite → consume table positions; K3.5 row + K3.9 row; §28.3 discharge rules |

Also re-checked: `using var` pins (A5 unchanged, "never movable out"), `sb.Append(…)` chaining and the
`new StringBuilder().Append(…)` chain root (§28.6 *Methods returning `this`*, unchanged), and the
`case pool.PopFirst() of var f: … end` K8.3 boundary (unchanged). **No verdict changed on any trace.**

---

## 8. Comparison against the lane tables

Read after the independent pass. The three tables are honest and I found no misclassified mapping in
them. Differences:

- **Everything the lanes claim as covered, I confirmed covered** — including the compressions I was
  most suspicious of: K4/K5's collapse into the table, §28.11's per-family bullets → tables + stdlib
  delegation, §28.13's sendability trims, §28.14's retention register, and §28.6's leak-prevention bullets.
- **The lanes' three flagged items are all correct calls** (D-M1, the §30.5 slip, DIV-1) — verdicts above.
- **Four findings are mine, in none of the lane tables:** D1 (Appendix C's three dangling kill-table
  citations), D2 (the 15 out-of-zone `target-announced` sites, 9 citing K3.13), D5 (§3.6's stale
  "§28 intro" pointer + the lost derived-property tag), and S1 (K3.6's `kw` bit vs K4's recovery recipe).
  Lane N flagged adjacent out-of-zone residuals (CG l.552, stdlib l.2324/2347 "drops") but not these.
- Lane N's item 5 ("the two `sec-consume-table` links resolve only once lane K's table lands") —
  **resolved**: the anchor is present and both links resolve at HEAD.

## 9. Recommendation to the composition gate

Zone-internal: **pass**. Before landing, close D1 (3 one-line re-pointings) and decide D2's ownership
(wave 2 or now — it is 15 sites, 9 of them citing a renamed definition). D3/D4/DIV-1 are three
corrections of landed text: accept them (all three are right), apply D-M1's "one form" → "forms" nit and
D4's positive-half restoration, and record all three in the v0.281 changelog's corrected-in-passing
bullet — a rewrite advertised as zero-semantic-change must not carry unrecorded rulings. D5 and S1–S5
are cheap and can ride the same pass.
