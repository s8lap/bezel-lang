# The spec-change process

*Supporting record (not a numbered proposal). Written 2026-07-11, distilled from the two whole-language review runs (2026-07-07 → v0.183, 2026-07-11 → v0.184) that produced most of the spec's recent revisions. This document formalizes **how** the normative documents in `docs/` change: the classes of change, the gate each class must pass, the artifacts each leaves behind, and the tooling. It is meta-process — it does not itself change the language.*

Status: proposed (owner review). The rules below describe what the last two runs did *plus* the improvements this document introduces (the change-class gates, the composition gate, the one-command validation, the review queue). Where the runs already worked this way, the doc records the practice; where it improves on it, the improvement is marked **NEW**.

---

## 0. Why write this down

The process worked, but it lived almost entirely in run memory rather than in the repo, and its validation was re-derived by hand each run. Two concrete costs of that:

- **Validation was ad hoc.** Every landing re-wrote the same anchor audit and extraction-parity check as a throwaway script. Twice this cost real defects that a standing check would have caught mechanically — most recently the v0.184 landing bumped the spec version header but not the stdlib/rationale companion headers, found only because this document's new `scripts/check_docs.sh` flagged it.
- **Trains composed only pairwise-by-hand.** The v0.178–v0.183 revisions were designed as semi-independent trains, each internally critiqued, but no step *required* a cross-train check. The 2026-07-11 run had to spend a dedicated "seam audit" lane finding where two independently-true paragraphs contradicted each other. That check is now a required step (§4).

The governing principle is unchanged from `CLAUDE.md`: **the spec is the source of truth; a spec defect is fixed by amending the spec, not by writing code that contradicts it; and decisions get recorded with their reasoning.** This document is the operational form of that principle for the documents themselves.

---

## 1. The ten documents and what "changing the spec" means

The normative language is exactly the ten documents in `docs/`:

| Document | Role | Carries the revision number? |
|---|---|---|
| `Bezel_Language_Spec.md` | normative reference; every syntactic/semantic rule | **yes** — `**Version 0.NNN**` header |
| `Bezel_Standard_Library.md` | every stdlib type/method/constant | **yes** — `**Companion … v0.NNN**` header |
| `Bezel_Design_Rationale.md` | *why* each decision was made; alternatives declined | **yes** — companion header |
| `Bezel_Changelog.md` | the per-revision narrative history; one entry per revision, pinned | **yes** — companion header |
| `Bezel_Guide_01_Memory_and_Ownership.md` | example-first idiom guide | no (revalidation pin only) |
| `Bezel_Guide_02_Errors_and_Nullability.md` | example-first idiom guide | no (revalidation pin only) |
| `Bezel_Guide_03_Modeling_Data.md` | example-first idiom guide | no (revalidation pin only) |
| `Bezel_Guide_04_Concurrency.md` | example-first idiom guide | no (revalidation pin only) |
| `Bezel_Guide_05_Collections_and_Iteration.md` | example-first idiom guide | no (revalidation pin only) |
| `Bezel_Guide_06_Contracts_and_Purity.md` | example-first idiom guide | no (revalidation pin only) |

`docs/proposals/` is **not** normative. A proposal is a design under consideration; it becomes normative only by *graduating* — being amended into the normative documents through this process, at which point the proposal file's `Status` flips to `superseded` with a pointer to the landed revision.

A single **revision** (v0.NNN → v0.NNN+1) is the atomic unit of change: one version-header bump across the four versioned documents, one changelog entry in `Bezel_Changelog.md`, one commit (or a tight sequence pushed together). Everything below is about how a revision is proposed, gated, and landed.

---

## 2. Change classes — different changes earn different gates

Not every change deserves the same ceremony. Classify the change first; the class determines the required gate (§3–§4) and the artifacts (§5). One carve before classifying: a change touching **only the guides** is not a revision and takes no class — see §5 item 5's guide-only-pass rule.

| Class | What it is | Examples | Required gate |
|---|---|---|---|
| **Editorial** | wording, typos, dead cross-references, stale prose that no longer describes any rule | fixing a dangling `§` link; a currency sweep of terminology a prior revision renamed | mechanical gate (§3) only |
| **Clarification** | states an already-implied rule explicitly; adds an example; consolidates text that lived in several places | the v0.184 "Choosing a disposal form" recap of the three disposal patterns | mechanical gate + one reviewer confirms the rule is genuinely unchanged |
| **Ruling** | resolves an underspecified or contradictory case by *deciding* it | who-may-call-`Dispose()`; the construction-window rule for observing `this`; out-parameter copy-out semantics | mechanical gate + **adversarial critique** (§4) + a changelog entry recording the decision, the alternatives, and why |
| **Design change** | changes a settled rule, adds/removes a language feature, or repartitions conformance | removing a member kind; demoting a feature to Extended; a new operator | a **proposal document** first (§6), then — only after owner acceptance — a ruling-class landing |
| **Ratchet** | lands a tightening **whose design a prior revision already ruled** — the rejection was scheduled and the warning was its ramp; the decision is behind it, only the promotion is new | flipping to an error a warning that landed warn-first for exactly that purpose | ruling-class gate (§3–§4) **plus a stated ramp**: the Migration note must give both ends (§5) |

Two rules make the classification honest:

- **When unsure between two classes, take the stricter gate.** A "clarification" that turns out to decide a case is a ruling; gate it as one.
- **A ratchet is a landing, not a decision.** The Ratchet class exists only for a tightening a prior revision already ruled and scheduled; it is lighter than **Design change** because the design question was already answered under the heavier gate. A tightening that is *not* already scheduled changes what compiles on its own authority — that is a design change, and it takes §6 first. Without this line the stricter-gate rule above would route every ratchet to Design change and the class would be dead letter.
- **A change is a design change the moment it contradicts a recorded decision** (a rationale decline record or a `DECLINED.md` entry). Those are reopened only with *new evidence against the stated reasons*, never by re-asking — and reopening is itself a proposal.

---

## 3. The mechanical gate — `scripts/check_docs.sh`

**NEW (this document ships it).** Every revision, of every class, must pass `scripts/check_docs.sh` before it lands. It is the docs-side counterpart to `scripts/gate.sh`: one blessed command, four checks, non-zero exit on any structural failure.

1. **Anchor resolution** — every intra/cross-doc `[…](file#anchor)` link resolves, against both anchor styles the repo uses (explicit `<a id="sec-…">` for cross-references and GitHub heading-slugs for the tables of contents).
2. **Version consistency** — the spec/stdlib/rationale/changelog headers agree on the revision, and `Bezel_Changelog.md` has exactly one `### v0.NNN changelog` entry for it. *(This is the check that caught the v0.184 header drift.)*
3. **§-citation typo lint** (advisory) — spec §-references to section numbers with no heading are flagged; advisory because legitimately-cross-doc citations produce residual noise.
4. **Example-extraction parity** — `scripts/extract_doc_examples.py` runs clean over the *current* docs into a scratch tree, so a malformed code example is a hard failure. It never touches the committed conformance corpus (see §7 on `#1380`).

`scripts/check_docs.sh --quick` runs checks 1–3 (skips extraction) for a fast inner-loop check while drafting.

The mechanical gate is a **lint, not a proof.** It catches structural drift — dangling anchors, stale headers, malformed examples, mistyped section numbers. It says nothing about whether the *meaning* is right. That is §4.

---

## 4. The meaning gate — adversarial critique, and the composition check

Rulings and design-change landings additionally pass a **meaning gate** that the mechanical gate cannot provide.

**Per-change adversarial critique.** Every ruling is designed by an author and then attacked by an independent critic instructed to *refute* it: re-locate every quoted passage, trace every example on every control-flow path, hunt the killer counterexample, and check the ruling against recorded rationale postures. The author resolves every blocking finding (fixing the design, or recording with reasons why the critic is wrong — never a silent drop). This is the designer→critic→finalizer shape both review runs used; it is what turns a plausible-sounding rule into a defensible one.

**The composition gate (NEW as a *requirement*).** When a single run lands more than one ruling — especially as separate "trains" — a cross-cutting check over the *combined* edit set is mandatory before landing, not optional. Two rulings can each be internally sound and still contradict each other at the seam (the v0.180 event-subscription rule vs. the v0.180 capture-admission bound was exactly this). Concretely:

- Land the trains' edits together and dry-run them as one sequence (every `old→new` must still apply uniquely against the combined intermediate state).
- Run one reviewer over the union whose sole job is pairwise composition: does every decision compose with every other, and with the already-landed rules the trains built on?
- Resolve seam conflicts *before* the version bump, not in a follow-up revision.
- **A train authored in parallel with another re-verifies every `old` anchor against HEAD immediately before composing, not only at the gate.** Two trains in one run are not two independent edit sets: the first to land moves the text the second quotes. In the 2026-09-15 run a question train authored against v0.298 kept 20 of its 21 anchors when the defect train landed underneath it — but one whole ruling had been *overtaken*, its answer settled the other way by the landed revision, and pressing it would have contradicted a recorded decision. That is a design change under §2's third rule, and the lane caught it only because it re-checked. The anchors are the cheap half of this; the expensive half is that a ruling can become a design change while you are writing it.

**Verification bar for findings.** A finding that a rule is *missing* ("the spec defines this nowhere") carries the highest burden: the claim must name the sections that come closest and quote why each falls short. "I didn't find it" is not evidence of absence. This is the discipline that lets a review trust its own negative-space findings.

---

## 5. Artifacts every revision leaves behind

A revision is not done when the text changes; it is done when the record is complete.

1. **The edit itself**, in the three versioned documents, with the version headers bumped in lockstep.
2. **A changelog entry** in `Bezel_Changelog.md`, `### v0.NNN changelog (v0.NNN-1 → v0.NNN)` — the single source of narrative history. When several trains land in one revision, they share **one** entry (the trains append blocks; there is never a second entry for the same version). The entry states, for each ruling: what was decided, what was declined, and why. It ends with a **Migration** note: what becomes newly-rejected, newly-defined, newly-warned, and what is doc-only. **A ratchet-class change states its ramp there, not just its outcome** (v0.263, from proposal 0106 §4.4): which revision landed the warning, which revision lands the rejection, and what a program that compiles today sees in between. The Migration categories record the *end state* of one revision; a promotion is a two-revision fact, and without the ramp the note cannot express the difference between a rule that has always rejected and one that warned for several revisions first. Do not confuse a ramp with a **spec-ahead-of-impl** delta: a rule the spec states as a rejection and the compiler has not yet enforced is item 3's business, not a ratchet — the compiler catching up is not a revision and has no Migration note to carry a ramp.
3. **A tracking issue for every spec-ahead-of-impl delta.** The spec routinely leads the compiler. Each semantic change that the compiler does not yet implement gets a GitHub issue filed *in the same landing*, citing the section and the revision (`§28.3, v0.184`) and stating what the spec now requires vs. what the compiler does. This is how "the spec is normative and the compiler is behind on exactly these items" stays legible. (`CLAUDE.md`'s spec-citation discipline applies: cite section *and* revision so the reference stays pinned.) A documented-but-unimplemented *stdlib surface* carries an in-document marker — `⚠ **Spec-ahead-of-implementation (#N):**` — naming its tracking issue, in addition to the issue, and the code-generation reference's index entry for it carries the same issue number.
3b. **A tracking issue for every impl-ahead-of-spec delta — and a check before asserting an absence.** Item 3 says *"the spec routinely leads the compiler"*, and that is usually true; it is not always true, and the process had no slot for the mirror case until v0.275. Twice in one week a revision **wrote an absence into a normative document for a capability the compiler ships**: v0.274 deleted `Array<T>`'s parameterless `Sort`/`Sorted` reasoning that an unreachable *extension* meant no capability existed — while sema, codegen, twelve test files and five hardening issues said otherwise — and v0.275 came within one gate finding of doing the same for `OwningArray<T>`.

   The obligation is therefore: **before a revision states that something does not exist, is not admitted, or is a compile error, check the implementation.** A `grep` over `src/` and `tests/` for the member name is enough to catch the class; a claim that survives that check can be written, and one that does not becomes an impl-ahead-of-spec delta with its own issue. Where the check cannot settle it — the environment cannot build, say — **the revision does not assert the absence**: it records what is known, names the probe that would settle it, and files the question. An unverified absence is worse than a silence, because a reader trusts it.

3c. **Every ruling that changes a verdict re-checks the rule corpus, in the same landing.** `tests/conformance/rules/` is one hand-authored case per normative rule, and its load-bearing property is that **a case states the verdict the spec requires and is never edited to match a compiler** — that is what makes its divergence list a spec-catch-up backlog rather than a description of the implementation. The corollary is that when a *revision* changes a verdict, a case still stating the old rule is not merely stale: it is wrong, it will read as a compiler defect at the next classification, and nothing regenerates it. Item 3's `#1380` rule is the mirror image — the *doc-example* corpus must never be blind-regenerated because its cases carry hand-annotations; the *rule* corpus must be hand-swept because nothing generates its cases at all.

   The sweep is per ruling, not per section: search by the rule's own text and by the construct, since a case can state a rule while citing a different section (`manifest.tsv` carries every case's `rule:` field in one grep-able place). Fix the case's `rule:` header as well as its expectation — re-citing without re-reading is how a corpus drifts. A case the revision makes **unwritable** — the rule now has no isolable program — is deleted with its reason recorded in the changelog, not bent into shape; v0.299's §28.6 moved-field arm is the worked instance. Where a ruling *opens* coverage the corpus lacks, record the gap rather than writing cases under landing pressure.

4. **Comments closing the loop on prior issues** the revision resolves (close with the citation; or comment "landed in <sha>, tracking the impl delta").
5. **Guide revalidation** when a revision changes idiom. The guides carry a revalidation pin, not a version number; bump it and re-trace every example. A guide that teaches a retired idiom is a defect (Guide 02 was 26 revisions stale when the 2026-07-11 run found it).

   **Guide-only passes are unversioned (owner-ruled 2026-07-30).** A change touching only the guides — a restructure, an expansion, an idiom refresh at a fixed normative revision — takes **no version bump and no changelog entry**: the revision number pins the *normative* definition, and it must not churn for prose. The record is the commit message and PR, plus the adversarial meaning gate for substantive passes. The pin keeps its exact meaning — "re-validated against v0.NNN" names the normative revision the guide was last checked against — so it stays put when only guide prose moves, and moves only when the normative documents move and the guide is re-checked. When a guide change rides a normative train, that revision's entry mentions the pin move as before. (The one guide-only train that predates this ruling, v0.236, stands as pinned history.)

---

## 6. Proposals — the design-change lifecycle

Design changes start as a proposal in `docs/proposals/`, one file per proposal, `NNNN-slug.md`, with the header block the folder's `README.md` documents (`Status`, `Tracking issue`, `Target module(s)`, `Written against`). The cardinal repo rule holds inside proposals too: **no invented current stdlib APIs** — everything a proposal builds on must exist in `Bezel_Standard_Library.md` (cited), and everything new must be clearly marked proposed.

**Status lifecycle** (the values the folder uses):

```
draft ──► under-review ──► accepted ──► (graduates) ──► superseded
   │                          │
   └──────────► withdrawn     └──► declined ──► DECLINED.md entry
```

- **draft** — authored, not yet reviewed.
- **under-review** — a critique pass has run; owner decision pending.
- **accepted** — the design is blessed; it may graduate when an implementation/landing run picks it up. Acceptance does **not** land anything in the spec.
- **superseded** — the proposal graduated into the normative docs; the file points at the landing revision.
- **declined** / **withdrawn** — see below.

**Graduation** is a ruling-class landing (§2) of the accepted design: it passes the mechanical gate and the meaning gate, lands as a revision with all §5 artifacts, and flips the proposal to `superseded`.

**Declining** is a first-class outcome, recorded in `DECLINED.md` with the reasons — so the question stays settled rather than perennially reopened. A declined direction is reopened only by new evidence against the stated reasons, filed as a fresh proposal.

Two supporting rules the runs learned:

- **Proposals cite the revision they were written against**, and that pin is re-verified at graduation (sections move; a proposal written against v0.178 must be checked against the current text before it lands).
- **A deletion-shaped proposal must account for the conformance corpus.** The committed corpus under `tests/conformance/` is hand-maintained (`#1380`), so a proposal that removes a construct must grep the corpus for it and list every case to delete-or-convert. This was the systematic blind spot across the 0037–0039 batch until it was made a checklist line.

---

## 7. Standing hazards (learned the hard way)

- **`#1380` — never blind-regenerate the conformance corpus.** The committed cases under `tests/conformance/` carry hand-annotations; `extract_doc_examples.py` output has drifted from them. The mechanical gate extracts into a *scratch* tree and compares; it never overwrites the committed corpus. A deliberate regen is its own task with hand-annotation preservation, not a side effect of a docs edit.
- **Edit-set JSONs may contain their own changelog-insertion edit.** When a landing is assembled from machine-actionable edit lists, check whether the changelog insertion is already *in* the edit list before composing one externally — the v0.184 landing nearly double-inserted the changelog this way (caught pre-push).
- **Version headers drift silently.** Three documents carry the revision in two different header formats. Bump all three; the mechanical gate now enforces it.
- **Impl divergence is not a spec defect.** The compiler implements an older revision than the spec by design. A probe disagreeing with new spec text proves nothing by itself — it is expected impl lag, tracked by the issue filed at landing. Only report a *spec-text* problem as a spec defect.
- **Verify every `§`-citation against the actual heading before writing it.** Section numbers move between revisions; a cited number is only as good as its last check.
- **History narration creeps into normative text.** The normative documents state *current* rules only; the changelog (`Bezel_Changelog.md`) is the sole home for history. A bare landing pin — `(v0.NNN)` — is acceptable where it dates a rule. An explanatory narration of a superseded state is a defect wherever it appears: "previously this section permitted…", "no longer…", "this sentence formerly asserted…", "(v0.NNN — the old rule said X, which was impossible because…)". The correction's story belongs in the changelog entry that landed it; the normative text states only what the rule *is*. This applies to grammar comments in Appendix A and to stdlib prose as much as to spec sections. (The 2026-07-14 sweep removed ~140 accumulated instances; the owner's review had flagged the pattern in §2.5 — write the rule here so it doesn't regrow.)

---

## 8. The owner-review queue

**NEW.** Owner decisions currently scatter across issue threads, proposal headers, and run logs. This document designates one queue so everything awaiting the owner's judgment is visible in one place: **the `docs/proposals/README.md` index is the review queue.** Its `Status` column is the ground truth for where each proposal stands, and any `accepted (delegated)` ruling, `under-review` proposal, or open RFC is — by being in that table with that status — an item on the owner's queue. A run that makes a delegated ruling records it in the proposal file *and* reflects it in the README status in the same commit, so the table never lies about what is settled vs. pending.

The paired records that support the queue:

- `docs/proposals/README.md` — the index and status of every proposal + the supporting records.
- `docs/proposals/DECLINED.md` — the declined register and the decline index: the register carries this folder's own declined-with-reasons entries, and the index above it is the single ground-truth for *where* every recorded decline, deferral, non-goal, and retirement lives across all five homes (it points; it never restates a reason).
- `docs/proposals/implementation-roadmap-2026-07.md` — the sequenced work-list for turning accepted proposals and landed-but-unimplemented rulings into compiler work.

---

## 9. Checklist (the whole process on one screen)

For a **ruling** or a **design-change graduation** (the full path):

- [ ] Classify the change (§2). Design change → it must already be an accepted proposal (§6).
- [ ] If the change touches §28/§30.5: state its kernel classification — derivation (name the K's), axiom extension (name the axiom), or new axiom (add the register row with its recorded reason) (v0.198).
- [ ] If the change adds or alters a compile-time rule: state its body-locality classification (proposal 0105 §1) — **local** (no cross-body information), **fact-discharged** (names the per-body facts it consumes), **carve-out** (generic instantiation, confined to its two clauses), or **invariant amendment** (explicit, with the recorded reason) (v0.262).
- [ ] If the change records a new decline, deferral, non-goal, or retirement — or graduates or declines a proposal: add or update its row in `DECLINED.md`'s decline index in the same commit, with its verdict class, its authoritative record (file + section), and its reopening bar or `—(none stated)`. This binds a decline recorded **outside** a graduation train too — a per-section decision record, a rationale amendment, a gate-round ruling — since that is how most declines actually arrive (v0.262-era, owner-commissioned 2026-08-20).
- [ ] Author the ruling with exact `old→new` edits; cite every `§` against the current heading.
- [ ] Adversarial critique; resolve every blocking finding (no silent drops) (§4).
- [ ] If multiple trains in one revision: run the composition gate over the union (§4).
- [ ] Dry-run all edits as one sequence — each `old` unique, applies clean.
- [ ] Bump the four version headers in lockstep; write the single shared changelog entry with its Migration note (§5).
- [ ] `scripts/check_docs.sh` passes (§3).
- [ ] File a tracking issue for every spec-ahead-of-impl delta; comment/close the issues the revision resolves (§5).
- [ ] **If any ruling changes a verdict: sweep `tests/conformance/rules/` for cases stating the old rule and fix them in the same commit** (§5 item 3c). Search by rule text and construct, not by section number; fix each case's `rule:` header too; delete rather than bend a case the revision makes unwritable, recording why.
- [ ] **Before asserting any absence** — that something does not exist, is not admitted, or is a compile error — grep `src/` and `tests/` for it (§5 item 3b). If the implementation provides it, that is an impl-ahead-of-spec delta with its own issue; if the check cannot settle it, record the question and the probe rather than the absence.
- [ ] Revalidate the guides if idiom changed; bump their pins (§5).
- [ ] **If the revision changes syntax, the stdlib surface, or an ownership rule: update [`Bezel_For_Code_Generation.md`](../Bezel_For_Code_Generation.md) and move its pin, in the same commit.** That document is written to be *copied from* by a model that will not read the spec, so a stale line there is wrong code rather than stale teaching — which is why `check_docs.sh`'s PIN check **fails** it past three revisions of staleness while merely reporting the guides'. Its fenced examples are extracted and checked with the rest, so a stale *example* fails the gate mechanically; its prose is the part a human has to re-read.
- [ ] If the revision ships something the rationale §18 deferred list carries: annotate that entry with the shipped marker in the same revision (v0.201 rule; the ReceiveAll staleness is the cautionary case).
- [ ] Commit (one revision = one commit or a tight pushed sequence), push, and — for a graduation — flip the proposal to `superseded` and update the README status (§8).

For **editorial** / **clarification** revisions, the path is the same minus the critique/composition steps: still classify, still `check_docs.sh`, still a changelog entry in `Bezel_Changelog.md` (editorial batches may share one), still push.

---

## 10. The subtraction review

**Owner-ratified 2026-08-20 (proposal 0106 §2.5).** Every removal the language has shipped — the `event` member kind, object-initializer blocks, extension-specificity rules 2 and 3, the generic-leaves determination rule, `ReadOnlyAsciiByteSpan`, four axioms — was triggered by a defect or by a proposal about something else. None came from anyone asking of the shipped surface, on a schedule, *what is no longer earning its place?* That is Oberon's transferable half, and it is the half a language with a thick decline register can still miss, because a register records what was never added rather than what was added and is now carried. A **periodic subtraction review** is therefore standing practice, and it is recorded with a cadence rather than an intention, since an unset schedule reproduces the failure this paragraph names: **every twenty revisions** — owner-ruled 2026-08-20, revision-counted rather than calendar-counted because the project's revision pace, not the calendar, is what accumulates the surface a pass reviews; the owner may reset the clock, change the interval, or skip a pass. The clock starts at v0.263, so the first pass is due at **v0.283**. Each pass is one pass over the *shipped* surface whose only question is what to delete, taking the same evidence bar a removal takes anywhere else (spec §2.8.5's "real programs hitting real walls", inverted — a construct's keep is defended, not assumed). Its output is a proposal or nothing; it mints no removal by itself, and a pass that concludes "nothing goes" is a successful pass, recorded as such. It is deliberately not a checklist line in §9: it is not a step in any one revision's path, and attaching it to revisions is exactly what made every removal so far incidental to some other errand. Implementation state is not a removal ground: a documented surface the compiler has not yet implemented is kept and marked (§5 item 3), and is reviewed, like any other, on its design (owner, 2026-10-08).
