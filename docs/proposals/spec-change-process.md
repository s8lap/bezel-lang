# The spec-change process

*Supporting record (not a numbered proposal). Written 2026-07-11, distilled from the two whole-language review runs (2026-07-07 → v0.183, 2026-07-11 → v0.184) that produced most of the spec's recent revisions. This document formalizes **how** the five normative documents in `docs/` change: the classes of change, the gate each class must pass, the artifacts each leaves behind, and the tooling. It is meta-process — it does not itself change the language.*

Status: proposed (owner review). The rules below describe what the last two runs did *plus* the improvements this document introduces (the change-class gates, the composition gate, the one-command validation, the review queue). Where the runs already worked this way, the doc records the practice; where it improves on it, the improvement is marked **NEW**.

---

## 0. Why write this down

The process worked, but it lived almost entirely in run memory rather than in the repo, and its validation was re-derived by hand each run. Two concrete costs of that:

- **Validation was ad hoc.** Every landing re-wrote the same anchor audit and extraction-parity check as a throwaway script. Twice this cost real defects that a standing check would have caught mechanically — most recently the v0.184 landing bumped the spec version header but not the stdlib/rationale companion headers, found only because this document's new `scripts/check_docs.sh` flagged it.
- **Trains composed only pairwise-by-hand.** The v0.178–v0.183 revisions were designed as semi-independent trains, each internally critiqued, but no step *required* a cross-train check. The 2026-07-11 run had to spend a dedicated "seam audit" lane finding where two independently-true paragraphs contradicted each other. That check is now a required step (§4).

The governing principle is unchanged from `CLAUDE.md`: **the spec is the source of truth; a spec defect is fixed by amending the spec, not by writing code that contradicts it; and decisions get recorded with their reasoning.** This document is the operational form of that principle for the documents themselves.

---

## 1. The five documents and what "changing the spec" means

The normative language is exactly the five documents in `docs/`:

| Document | Role | Carries the revision number? |
|---|---|---|
| `Bezel_Language_Spec.md` | normative reference; every syntactic/semantic rule | **yes** — `**Version 0.NNN**` header |
| `Bezel_Standard_Library.md` | every stdlib type/method/constant | **yes** — `**Companion … v0.NNN**` header |
| `Bezel_Design_Rationale.md` | *why* each decision was made; alternatives declined; the per-revision changelog | **yes** — companion header **and** the changelog |
| `Bezel_Guide_01_Memory_and_Ownership.md` | example-first idiom guide | no (revalidation pin only) |
| `Bezel_Guide_02_Errors_and_Nullability.md` | example-first idiom guide | no (revalidation pin only) |

`docs/proposals/` is **not** normative. A proposal is a design under consideration; it becomes normative only by *graduating* — being amended into the five documents through this process, at which point the proposal file's `Status` flips to `superseded` with a pointer to the landed revision.

A single **revision** (v0.NNN → v0.NNN+1) is the atomic unit of change: one version-header bump across the three versioned documents, one changelog entry in the rationale, one commit (or a tight sequence pushed together). Everything below is about how a revision is proposed, gated, and landed.

---

## 2. Change classes — different changes earn different gates

Not every change deserves the same ceremony. Classify the change first; the class determines the required gate (§3–§4) and the artifacts (§5).

| Class | What it is | Examples | Required gate |
|---|---|---|---|
| **Editorial** | wording, typos, dead cross-references, stale prose that no longer describes any rule | fixing a dangling `§` link; a currency sweep of terminology a prior revision renamed | mechanical gate (§3) only |
| **Clarification** | states an already-implied rule explicitly; adds an example; consolidates text that lived in several places | the v0.184 "Choosing a disposal form" recap of the three disposal patterns | mechanical gate + one reviewer confirms the rule is genuinely unchanged |
| **Ruling** | resolves an underspecified or contradictory case by *deciding* it | who-may-call-`Dispose()`; the construction-window rule for observing `this`; out-parameter copy-out semantics | mechanical gate + **adversarial critique** (§4) + a rationale changelog entry recording the decision, the alternatives, and why |
| **Design change** | changes a settled rule, adds/removes a language feature, or repartitions conformance | removing a member kind; demoting a feature to Extended; a new operator | a **proposal document** first (§6), then — only after owner acceptance — a ruling-class landing |

Two rules make the classification honest:

- **When unsure between two classes, take the stricter gate.** A "clarification" that turns out to decide a case is a ruling; gate it as one.
- **A change is a design change the moment it contradicts a recorded decision** (a rationale decline record or a `DECLINED.md` entry). Those are reopened only with *new evidence against the stated reasons*, never by re-asking — and reopening is itself a proposal.

---

## 3. The mechanical gate — `scripts/check_docs.sh`

**NEW (this document ships it).** Every revision, of every class, must pass `scripts/check_docs.sh` before it lands. It is the docs-side counterpart to `scripts/gate.sh`: one blessed command, four checks, non-zero exit on any structural failure.

1. **Anchor resolution** — every intra/cross-doc `[…](file#anchor)` link resolves, against both anchor styles the repo uses (explicit `<a id="sec-…">` for cross-references and GitHub heading-slugs for the tables of contents).
2. **Version consistency** — the spec/stdlib/rationale headers agree on the revision, and the rationale has exactly one `### v0.NNN changelog` entry for it. *(This is the check that caught the v0.184 header drift.)*
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

**Verification bar for findings.** A finding that a rule is *missing* ("the spec defines this nowhere") carries the highest burden: the claim must name the sections that come closest and quote why each falls short. "I didn't find it" is not evidence of absence. This is the discipline that lets a review trust its own negative-space findings.

---

## 5. Artifacts every revision leaves behind

A revision is not done when the text changes; it is done when the record is complete.

1. **The edit itself**, in the three versioned documents, with the version headers bumped in lockstep.
2. **A rationale changelog entry** `### v0.NNN changelog (v0.NNN-1 → v0.NNN)` — the single source of narrative history. When several trains land in one revision, they share **one** entry (the trains append blocks; there is never a second entry for the same version). The entry states, for each ruling: what was decided, what was declined, and why. It ends with a **Migration** note: what becomes newly-rejected, newly-defined, newly-warned, and what is doc-only.
3. **A tracking issue for every spec-ahead-of-impl delta.** The spec routinely leads the compiler. Each semantic change that the compiler does not yet implement gets a GitHub issue filed *in the same landing*, citing the section and the revision (`§28.3, v0.184`) and stating what the spec now requires vs. what the compiler does. This is how "the spec is normative and the compiler is behind on exactly these items" stays legible. (`CLAUDE.md`'s spec-citation discipline applies: cite section *and* revision so the reference stays pinned.)
4. **Comments closing the loop on prior issues** the revision resolves (close with the citation; or comment "landed in <sha>, tracking the impl delta").
5. **Guide revalidation** when a revision changes idiom. The guides carry a revalidation pin, not a version number; bump it and re-trace every example. A guide that teaches a retired idiom is a defect (Guide 02 was 26 revisions stale when the 2026-07-11 run found it).

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
- **History narration creeps into normative text.** The five documents state *current* rules only; the rationale changelog is the sole home for history. A bare landing pin — `(v0.NNN)` — is acceptable where it dates a rule. An explanatory narration of a superseded state is a defect wherever it appears: "previously this section permitted…", "no longer…", "this sentence formerly asserted…", "(v0.NNN — the old rule said X, which was impossible because…)". The correction's story belongs in the changelog entry that landed it; the normative text states only what the rule *is*. This applies to grammar comments in Appendix A and to stdlib prose as much as to spec sections. (The 2026-07-14 sweep removed ~140 accumulated instances; the owner's review had flagged the pattern in §2.5 — write the rule here so it doesn't regrow.)

---

## 8. The owner-review queue

**NEW.** Owner decisions currently scatter across issue threads, proposal headers, and run logs. This document designates one queue so everything awaiting the owner's judgment is visible in one place: **the `docs/proposals/README.md` index is the review queue.** Its `Status` column is the ground truth for where each proposal stands, and any `accepted (delegated)` ruling, `under-review` proposal, or open RFC is — by being in that table with that status — an item on the owner's queue. A run that makes a delegated ruling records it in the proposal file *and* reflects it in the README status in the same commit, so the table never lies about what is settled vs. pending.

The paired records that support the queue:

- `docs/proposals/README.md` — the index and status of every proposal + the supporting records.
- `docs/proposals/DECLINED.md` — the declined register, with reasons.
- `docs/proposals/implementation-roadmap-2026-07.md` — the sequenced work-list for turning accepted proposals and landed-but-unimplemented rulings into compiler work.

---

## 9. Checklist (the whole process on one screen)

For a **ruling** or a **design-change graduation** (the full path):

- [ ] Classify the change (§2). Design change → it must already be an accepted proposal (§6).
- [ ] Author the ruling with exact `old→new` edits; cite every `§` against the current heading.
- [ ] Adversarial critique; resolve every blocking finding (no silent drops) (§4).
- [ ] If multiple trains in one revision: run the composition gate over the union (§4).
- [ ] Dry-run all edits as one sequence — each `old` unique, applies clean.
- [ ] Bump the three version headers in lockstep; write the single shared changelog entry with its Migration note (§5).
- [ ] `scripts/check_docs.sh` passes (§3).
- [ ] File a tracking issue for every spec-ahead-of-impl delta; comment/close the issues the revision resolves (§5).
- [ ] Revalidate the guides if idiom changed; bump their pins (§5).
- [ ] Commit (one revision = one commit or a tight pushed sequence), push, and — for a graduation — flip the proposal to `superseded` and update the README status (§8).

For **editorial** / **clarification** revisions, the path is the same minus the critique/composition steps: still classify, still `check_docs.sh`, still a changelog entry (editorial batches may share one), still push.
