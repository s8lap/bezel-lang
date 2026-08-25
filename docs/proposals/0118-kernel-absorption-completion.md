# 0118 — Completing the kernel absorption: one definition site per ownership rule

**Status:** accepted — owner-directed 2026-08-25; graduation targeted at v0.281
**Tracking issue:** [#3368](https://github.com/s8lap/bezel-lang/issues/3368)
**Target module(s):** Docs-only. Spec [§28](../Bezel_Language_Spec.md#sec-resource-management) (all subsections), the [§30.5](../Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal) tagged satellites, Appendix C.3/C.6 (sync), rationale (terminology note), Guide 01 revalidation, `Bezel_For_Code_Generation.md` §4 sync. **No grammar change. No stdlib API change. No compiler work.**
**Written against:** v0.279

## 1. What this proposes

Finish what proposal 0046 started. That proposal's own plan defined **absorption** — *"the rule statement collapses to a kernel cross-reference, keeping its examples and remedies"* (0046 §5, block table) — and claimed the consolidation *"shortens the chapters (the four residual-trust restatements collapse to one)"* (0046 §9). What landed at v0.198–v0.199 was the kernel (K1–K9, the axiom register) and the **tag pass**: the acceptance audit classified every block, and 56 blocks took a `derives:` tag while keeping their full prose; 16 were absorbed. The chapter has since grown, not shrunk: §28 stands at ~40,900 words, of which the kernel is ~4,160, and the 2026-08-25 elegance review measured the cost — the consume-site freeze is stated at eight sites, drop-on-overwrite at six, auto-drop at five, with the spec itself disclaiming the restatements as "consequences, not a second list", "stated once there", "presentations of this table".

This proposal completes the absorption: **every ownership rule gets exactly one definition site** (the kernel clause, the axiom register row, or — for genuinely local rules — its home subsection), and every other site becomes a cross-reference, a table, or a worked example. Alongside the absorption, four clarity items the same review verified:

1. **Terminology unification** (one glossary, four drift classes closed — §3 T4).
2. **Consume-table unification** — K3's twelve positions, K4's kill table, and K5's placements re-projected as one table (§3 T2).
3. **A reader router** at the head of §28 — the API-consumer subset (~10 rules) named and separated from the API-author surface (call classification, callback bounding), so most readers can stop early (§3 T5).
4. **History-narration extraction** — §28 carries ~40 inline `v0.NNN` narrations, tombstone discussions, and consistency proofs of its own design; [spec-change-process §7](spec-change-process.md) already rules these defects in normative text. They move to the changelog and rationale (§3 T1).

The intended semantic delta is **zero**: no program's acceptance changes, no K is renumbered, no axiom is added or retired. The owner directed the work 2026-08-25: *"clarity of the writing is important to the spec, and its memory management rules especially… we should write the section clearer and shorter if we can."*

## 2. Relationship to recorded decisions

0046 §13 declined **"full rewrite in one revision (delete-and-replace)"** — *"maximizes the semantic-drift risk §9 ranks first, defeats bisection of any discovered divergence, and makes the critique pass review everything at once"* (also `DECLINED.md`, *Ownership-kernel consolidation form*). This proposal does not refute those reasons; it inherits them as constraints:

- **The kernel is now a fixed referee.** The decline predates the kernel's landing: a rewrite then had no stable text to audit cuts against, so drift had no detector. Every cut this proposal makes is diff-audited against a landed, frozen kernel clause or axiom row — the referee 0046's own risk table wished for and then built.
- **Staged trains with per-train audits, exactly 0046's method** (§3): every deleted sentence maps to the covering clause, or it is not deleted; a sentence that maps to nothing is by definition either genuinely-new content (it stays, and the train's notes list it) or a divergence (filed as its own issue, never silently normalized — 0046 §9's standing rule, kept verbatim).
- **One revision, audited internally.** The trains land as one revision (v0.281) with one composition gate over the union — the shape every multi-train revision since v0.270 has taken. What the decline rejected was an *unaudited monolith* introducing kernel and rewrite simultaneously; what lands here is a sequence of small audited subtractions against a fixed reference, reviewed together.

Two adjacent recorded decisions are explicitly **out of scope** (checked before drafting): the redundant-`move` severity is owner-ruled (proposal 0090 §Q2, rationale §3.10, `DECLINED.md`) and no edit here touches it; the `using var := await` asymmetry is recorded (proposal 0082 §10, `DECLINED.md`) and stays. The review finding that proposed each was killed in verification.

## 3. The work, staged

- **T1 — Archaeology extraction.** Every inline revision narration, tombstone discussion, self-correction, and design-consistency proof in §28/§30.5 moves out: rule-relevant *reasons* to the rationale, history to the changelog (verified present there, added to the v0.281 entry where absent — content is moved, never dropped). Bare `(v0.NNN)` pins stay where they date a rule. Estimated −15–20%.
- **T2 — Table unification.** One **consume table**: rows = K3's twelve consume positions grouped by its three families; columns = marker spelling, subject (binding vs slot), post-state, frozen?, disposal moment, kernel tag. It becomes the single definition site for what K3's list, K4's kill-table rows, and K5's placement list each state today; K4 keeps its three posture clauses, K5 keeps the two-conjunct criterion, its two recorded failure shapes, and the one override. The producers table's four call rows collapse to one row pointing at the announcement table; `move coll[i]`'s admitted-type enumeration lives once, in §28.11.
- **T3 — Absorption.** Subsection by subsection (§28.6, §28.7, §28.8, the freeze cluster in §28.3, §28.11's per-family prose, §28.13/§28.14's stdlib rosters → one formula + a stdlib-doc pointer): each tagged restatement becomes a one-to-three-sentence corollary citation plus the subsection's genuinely-new content and its examples. Per cut, a diff-against-kernel check; details the kernel does not carry (e.g. §28.7's null-guarded, dynamically-dispatched disposal) are enumerated in the train notes and survive verbatim.
- **T4 — Terminology.** A four-line glossary at the head of §28.3, then the renames: *holder* for the role, *owner/owned* reserved for the classification (the two are currently interchangeable); *drop* reserved for disposal-carrying discard — §28.11's operation table says *removes* in its non-owned columns (today "drop" means dispose in §28.7 and not-dispose three sections later); *borrowing position* replaces "borrow-consuming position" (which consumes nothing); the "-announced" family keeps **site-announced** (K4.2) and renames **target-announced** to **marker-free position**, with the glossary distinguishing all of it from **type-announced** and **mint-announcing**. Each rename is swept across spec, stdlib, CG reference, and guides in the same train, greps `src/` for diagnostic-text uses before claiming the term is doc-only, and the changelog carries the old→new table for grep-trail continuity.
- **T5 — Router and sentence surgery.** A head-of-§28 paragraph: which rules an API *consumer* needs (the Guide 01 subset) vs. which are API-*author* surface (call classification, §28.14 bounding). The five 300–1,200-word single-sentence rules (the `owned` type requirement, calls-mint, bare-type-parameter returns, the borrowed-field store bullet, the sendability bullet) restructured into the kernel's own numbered-clause style, wording of operative claims unchanged.
- **T6 — Closing audit.** The acceptance test below; Appendix C.3/C.6 wording sync; Guide 01 re-trace + pin; CG §4 sync + pin; the 0046-style audit artifact regenerated (`0118-audit-v0.281.md`) so the next consolidation has provenance.

## 4. Acceptance test

1. **One definition site:** re-run the 0046 audit over the rewritten text; every former DERIVED-with-prose block is now ABSORBED, or its residue is listed in the train notes as genuinely-new content with its home.
2. **Zero semantic delta:** every deleted sentence maps to its covering clause (the per-train audit tables); every §28/§30.5-derived conformance-corpus case re-traced against the new text with identical verdicts; `check_docs.sh` extraction parity clean.
3. **Terminology:** the four drift classes are closed (grep: no "borrow-consuming position"; "drop" absent from non-owned table columns; owner-as-role absent outside the glossary's equation; "target-announced" absent outside the changelog).
4. **Measured, reported:** §28's word count before/after in the changelog entry (target 35–45% reduction; the number is reported, not gated — clarity wins over the target where they conflict).
5. **Gates:** the composition gate over the union; ruling-class ceremony despite the zero-delta intent (0046's own reasoning: a kernel restatement is the highest-leverage place for an accidental ruling to hide).

## 5. Risks

0046 §9's table applies unchanged (semantic drift → per-train audit + corpus re-trace + file-never-normalize; tag noise → absorption shortens). New here: **a restatement quietly carrying a detail the kernel lacks** — mitigated by the per-cut diff and the explicit genuinely-new lists; **renames breaking external grep-trails** (issues, PRs, memory) — mitigated by the changelog's old→new table; **the reviewer-burden inversion** — a 35–45% cut is a large diff even when every hunk is a deletion-with-citation, so the gate reviews train-by-train with the audit tables in hand, not the raw diff alone.

## 6. Out of scope

No rule changes (the two review candidates that were substantive are separately recorded, §2). No K renumbering (frozen at 0046's landing). No axiom additions or retirements. §30.13–§30.15 (domains/sendability/channels beyond §30.5's tagged paragraphs) stay untouched — 0046 §11's scope seam, unchanged.
