# Proposal 0087 — Re-deriving the mutable-statics taxonomy from per-type facts

| | |
|---|---|
| **Status** | graduated-v0.245 — superseded — graduated v0.245 (owner-ruled 2026-08-11, all questions as recommended; doc-structural, no compiler work — the Q3 check_docs check landed with the train) |
| **Tracking issue** | [#3053](https://github.com/s8lap/bezel-lang/issues/3053) |
| **Target module(s)** | **spec** — §22.12 rules 2(i) and 4 (the enumerations become derived sets), §28.13 (the sendability bullet's set-arithmetic sentence replaced by a derivation). **stdlib** — §1 (the facts vocabulary defined once), per-type fact lines at §4.19, §6.2, §6.3, §6.4, §6.5, §6.9, §6.11. **rationale** — a short decision record (the vocabulary and the reproduction proof). **guides** — Guide 04 §8 simplification at graduation. **This is doc-structural: NO behavior changes** — every membership, warning, admission, and rejection is reproduced exactly (§3's table is the proof), and any delta the reproduction had surfaced would have been flagged as a finding, not silently fixed. |
| **Written against** | spec/stdlib v0.241; re-verified at v0.244 before graduation (§22.12/§28.13 byte-identical across v0.242–v0.244; only line anchors moved — rule 2(i) 5029→5088, rule 4 5048→5107, rule 5 5049→5108, §28.13 bullet 7439→7502; the 10×3 reproduction table reproduces with zero delta) |
| **Origin** | Elegance review 2026-08-11, finding 6: §28.13 must define its synchronized set as — quoting the spec — form (i) "**minus `ChannelSender<T>`** … **minus `SharedEventSource<T>`** … **plus the three members**." When a normative document states set differences between its own allowlists, the taxonomy has outgrown its presentation. Each *membership* is a recorded ruling; the three-list structure itself is defended nowhere. |

*Reference convention: unprefixed §-references are language-spec references; stdlib references are marked.*

---

## 0. Thesis

Three curated type lists govern statics and cross-domain sharing, maintained in parallel and related by prose set arithmetic. This proposal replaces the *presentation*: four orthogonal per-type facts, declared once at each stdlib type, from which all three lists are **derived** by three one-line formulas. The facts are where the "grown only by recorded ruling" discipline attaches; the lists become computed views. Guarantees, memberships, warnings, and store semantics are unchanged — the reproduction table (§3) is the proposal's evidence core.

One deviation from the review is reported up front: the critic conjectured **three** facts would suffice (*internally-synchronized / deeply-immutable / distribution-mechanism*). They do not — the derivation was attempted and fails on `Locked<T>`/`RwLocked<T>`, which are internally synchronized yet deliberately excluded from the mutable-static form for a recorded fourth reason (the payload-scaled displaced-value leak under rule 2's overwrite-never-disposes). A **fourth fact** is required; §2 defines it. Three facts would have silently moved the data-owning locks into form (i) — a behavior change; the fourth fact is what keeps the re-derivation exact.

## 1. The inventory — the current full membership of every list

Extracted exhaustively from the v0.241 text; citations are section + the governing sentence (line numbers against v0.241: §22.12 rule 2(i) at spec line 5029, rule 4's suppression list at line 5048, rule 5 at 5049, §28.13's sendability bullet at line 7439).

**List 1 — §22.12 rule 2 form (i): the internally-synchronized-handle set** (mutable statics admitted warning-free):

| Member | Cited at |
|---|---|
| `Mutex` | §22.12 rule 2(i), line 5029; stdlib §6.2 |
| `Semaphore` | ib.; stdlib §6.3 |
| `Atomic<T>` | ib.; stdlib §6.4 |
| `AtomicInteger<T>` | ib.; stdlib §6.4 |
| `ChannelSender<T>` | ib.; stdlib §6.9 |
| `SharedEventSource<T>` | ib. ("the set's one non-sendable member, safe to reach from every domain by static reachability"); stdlib §6.5; rule 5 |

Explicit non-member with recorded reason: `Channel<T>` (*"the channel object is the receiving end, single-consumer by ownership; a process-global receiving end would readmit the multi-consumer receive stdlib §6.9 explicitly defers"* — line 5029).

**List 2 — §22.12 rule 4's warning-suppression set** (`static readonly` class-typed fields that do *not* draw the domain-reachability warning): *"suppressed by the form-(i) set, and (the suppression list, grown only by recorded ruling) by the data-owning locks `Locked<T>`/`RwLocked<T>` … and by `FrozenArray<T>`"* (line 5048):

| Member | Suppression ground recorded |
|---|---|
| the six form-(i) members | carry the guarantee themselves |
| `Locked<T>` | "internally synchronized and safe to reach from every domain by construction" (line 5048; stdlib §6.11, §30.12) |
| `RwLocked<T>` | ib. |
| `FrozenArray<T>` | "deeply immutable, hence safe to reach from every domain with no machinery" (line 5048; stdlib §4.19) |

Explicit still-warned member with recorded reason: `static readonly Channel<T>` (line 5048).

**List 3 — §28.13's `Shared`-payload synchronized set** (`Shared<T>` sendable iff `T` in this set; *"this seven-type enumeration is normative"*, line 7439):

| Member | Admission ground recorded |
|---|---|
| `Mutex`, `Atomic<T>`, `AtomicInteger<T>`, `Semaphore` | synchronization machinery (stdlib §6.2–§6.4) |
| `Locked<T>`, `RwLocked<T>` | own recorded ruling ("real synchronization machinery, interior sendable by instantiation", §30.12) |
| `FrozenArray<T>` | own recorded ruling — "the set's first member admitted by **deep immutability** rather than machinery" |

Explicit non-members with recorded reasons: `ChannelSender<T>` ("senders are minted per producer from the channel, so `Shared<ChannelSender<T>>` would add a second distribution mechanism"), `SharedEventSource<T>` ("static reachability is already its distribution mechanism, and `Shared<SharedEventSource<T>>` would add a second") — both at line 7439.

The set-arithmetic sentence this proposal retires, quoted whole (§28.13, line 7439): *"Its relation to §22.12 rule 2's form-(i) set, restated: this set is form-(i) **minus `ChannelSender<T>`** (excluded below) and **minus `SharedEventSource<T>`** (`SharedEventSource<T>` is in form-(i) but stays out of this set: static reachability is already its distribution mechanism, and `Shared<SharedEventSource<T>>` would add a second; stdlib §6.5) **plus the three members**, which are deliberately not form-(i) — no mutable statics of them; their `static readonly` forms are covered by §22.12 rule 4's suppression list instead."* — and the reason those three are not form-(i), recorded at §22.12 rule 4 (line 5048): *"The three suppression-list types are deliberately not form-(i) — no mutable statics of them: rule 2's overwrite-never-disposes store semantics would leak a displaced value of unbounded payload (a whole frozen array; a lock plus its entire interior) until process exit, a payload-scaled cost the `Task?` cache's acceptance does not cover, and no use case demands static reassignment of them (a data-owning lock's mutability lives behind its guards; a frozen array has none)."*

(Out of scope, stated: §22.12's forms (ii)–(iv) — the `Task?` cache, warned value statics, and the owning-inline-array pool — are *shape*-keyed admissions, not curated type lists, and are untouched. Rule 4's base warning, rule 3, and every store-semantics clause are untouched.)

## 2. The fact vocabulary — four orthogonal per-type facts

Each fact is a normative, per-type declaration in the standard library, grown only by recorded ruling (the discipline transfers from the lists to the facts):

- **synchronized** — the type's operations are internally synchronized **and** the instance is safe to reach from every domain (the conjunction §22.12 rule 2(i) already states: `Channel<T>` fails the second half — its receive side is single-consumer by ownership and must stay confined — which is exactly how the current text excludes it).
- **immutable** — deeply immutable after construction: no mutation surface, so cross-domain reach needs no machinery (`FrozenArray<T>`'s recorded ground, stdlib §4.19's construction obligations).
- **self-distributing** — the type already has its own sanctioned distribution channel, so a second one (static reassignment slots aside, specifically `Shared<T>` wrapping) is excluded by ruling: `ChannelSender<T>` (minted per producer from the channel, stdlib §6.9), `SharedEventSource<T>` (distributed by static reachability, §22.12 rule 5).
- **displacement-unbounded** — the value a rule-2 mutable static would strand on overwrite (overwrite-never-disposes, §22.13's exemption) is of unbounded, payload-scaled size: `Locked<T>`/`RwLocked<T>` (the lock plus its entire interior), `FrozenArray<T>` (the whole array). This is the fourth fact the critic's conjecture lacked; its content is verbatim the recorded reason the three suppression-list types are "deliberately not form-(i)" (line 5048/7439).

**The three derivations** (the replacement for the curated enumerations):

1. **form (i)** = synchronized ∧ ¬displacement-unbounded
2. **rule-4 suppression set** = synchronized ∨ immutable
3. **`Shared`-payload set** = (synchronized ∨ immutable) ∧ ¬self-distributing

## 3. The reproduction table — the proof of behavior-invariance

Every stdlib type any of the three lists mentions, including the recorded exclusions; facts per §2, derived memberships computed by the three formulas, compared against §1's inventory:

| Type | sync | immut | self-dist | displ-unb | form (i): derived / current | suppression: derived / current | Shared-set: derived / current |
|---|---|---|---|---|---|---|---|
| `Mutex` | ✓ | — | — | — | **✓ / ✓** | **✓ / ✓** | **✓ / ✓** |
| `Semaphore` | ✓ | — | — | — | **✓ / ✓** | **✓ / ✓** | **✓ / ✓** |
| `Atomic<T>` | ✓ | — | — | — | **✓ / ✓** | **✓ / ✓** | **✓ / ✓** |
| `AtomicInteger<T>` | ✓ | — | — | — | **✓ / ✓** | **✓ / ✓** | **✓ / ✓** |
| `ChannelSender<T>` | ✓ | — | ✓ | — | **✓ / ✓** | **✓ / ✓** | **✗ / ✗** |
| `SharedEventSource<T>` | ✓ | — | ✓ | — | **✓ / ✓** | **✓ / ✓** | **✗ / ✗** |
| `Locked<T>` | ✓ | — | — | ✓ | **✗ / ✗** | **✓ / ✓** | **✓ / ✓** |
| `RwLocked<T>` | ✓ | — | — | ✓ | **✗ / ✗** | **✓ / ✓** | **✓ / ✓** |
| `FrozenArray<T>` | — | ✓ | — | ✓ | **✗ / ✗** | **✓ / ✓** | **✓ / ✓** |
| `Channel<T>` | — | — | — | — | **✗ / ✗** | **✗ / ✗** | **✗ / ✗** |

Thirty of thirty derived memberships match the current normative text exactly; there is **no delta to flag**. Two rows deserve their notes: `FrozenArray<T>` is excluded from form (i) twice over under the facts (¬synchronized *and* displacement-unbounded) where the current text records only the displacement reason — an over-determination, not a divergence (the current text's "no mutable statics of them" outcome is identical); and `Channel<T>`'s exclusions all fall out of ¬synchronized (the confinement half), matching each of its three recorded per-list exclusion sentences.

## 4. Where the facts live — the notation

**Definition once, stdlib §1** (drop-in sketch):

> **Statics-and-sharing facts.** Certain library types carry up to four normative per-type facts — **synchronized** (internally synchronized and safe to reach from every domain), **immutable** (deeply immutable after construction), **self-distributing** (has its own sanctioned distribution channel), and **displacement-unbounded** (a stranded overwrite-displaced value is payload-scaled) — from which spec §22.12's form-(i) and warning-suppression sets and §28.13's `Shared`-payload set are derived by the formulas stated there. A fact is granted or withdrawn only by recorded ruling; a type declaring no facts is in none of the derived sets.

**Per-type lines** (one sentence in each type's stdlib section, replacing nothing — the surrounding prose already argues each fact's content):

```
stdlib §6.2  Mutex               — Facts: synchronized.
stdlib §6.3  Semaphore           — Facts: synchronized.
stdlib §6.4  Atomic<T>, AtomicInteger<T> — Facts: synchronized.
stdlib §6.9  ChannelSender<T>    — Facts: synchronized; self-distributing (senders are minted per
                                    producer from the channel). Channel<T> — Facts: none (the
                                    receiving end is single-consumer by ownership and domain-confined).
stdlib §6.5  SharedEventSource<T> — Facts: synchronized; self-distributing (distributed by static
                                    reachability, spec §22.12 rule 5).
stdlib §6.11 Locked<T>, RwLocked<T> — Facts: synchronized; displacement-unbounded (the lock and its
                                    entire interior).
stdlib §4.19 FrozenArray<T>      — Facts: immutable; displacement-unbounded (the whole array).
```

**Spec side:** §22.12 rule 2(i) restates as *"the field's type declares the **synchronized** fact and not the **displacement-unbounded** fact (stdlib §1; currently: Mutex, Semaphore, Atomic\<T\>, AtomicInteger\<T\>, ChannelSender\<T\>, SharedEventSource\<T\>)"* — the formula normative, the parenthetical membership **informative and derived**, regenerated whenever facts change. Rule 4's suppression clause and §28.13's bullet restate the same way with their formulas; §28.13's minus/minus/plus sentence and rule 4's "deliberately not form-(i)" justification collapse into the facts they were paraphrasing (the *reasons* stay, attached to each fact grant at its type). Every store-semantics clause, warning text, and non-set rule is untouched.

**Future additions — the payoff, worked:** admitting a hypothetical new synchronized handle today takes three co-ordinated edits (rule 2(i)'s list, rule 4's implied suppression via form (i), §28.13's arithmetic sentence and its cross-clauses) plus vigilance that the three stay consistent. Under the vocabulary it is **one fact ruling** at the type ("Facts: synchronized"), and the three memberships follow by formula — with the exclusion questions (`self-distributing`? `displacement-unbounded`?) posed *by the vocabulary itself* at granting time, which is exactly the checklist the historical rulings each rediscovered ad hoc.

## 5. Governance — the no-silent-growth clause, restated not weakened

§28.13 currently states: *"A type later joining the form-(i) set still does not join this set without a separate ruling, and vice versa."* Under the derivation, one fact grant can flow into two sets — that is the feature — so the clause transforms rather than survives verbatim: **sets are grown only by fact rulings, and a fact ruling must state which facts it grants and confront each derived-set consequence in its record.** The vice-versa protection the old clause bought (form-(i) membership never implying `Shared`-set membership) is now carried structurally by the `self-distributing` and `displacement-unbounded` facts — the two recorded reasons the sets ever diverged — rather than by a standing warning. Q2 asks the owner to confirm this restatement; the alternative (keep per-set ratification as a second gate on top of the facts) preserves the letter at the cost of re-introducing exactly the parallel-bookkeeping the proposal removes.

## 6. Alternatives considered

1. **Status quo.** Three parallel lists, prose set arithmetic in normative text, and a per-addition consistency burden. Declined: this is checker taxonomy leaking into the user's mental model (a user declaring one `static` field can encounter a hard error, two independently co-firing warnings, or silence, depending on which of three lists the type is in — and nothing at the type says which).
2. **Three facts** (the critic's conjecture). Refuted by construction — §0/§2: `Locked<T>`/`RwLocked<T>` force the fourth fact; without it the derivation admits them to form (i), a behavior change.
3. **Two lists with one derived** (derive only §28.13 from form (i) + local annotations). Declined: it keeps the arithmetic for the suppression set and leaves the fourth-fact content as an unexplained exclusion — the halfway point has most of the cost and half the benefit.
4. **A user-declarable language-level trait** (classes opt into `synchronized` etc.). Out of scope and declined for now: the sets are compiler-known *trust grants* (each membership is a soundness ruling about a specific implementation, the axiom-A10 family's shape), not structural properties a declaration could self-certify; opening them to user types is a real language design with its own verification questions, named here so it isn't smuggled in as notation.
5. **Regenerate the lists mechanically but keep them normative** (facts as source-of-truth in tooling only). Declined: two normative statements of one truth is the defect class `check_docs.sh` exists to catch; the facts must be the single normative carrier or nothing changed.

## 7. Migration & compatibility (changelog house categories)

- **Doc-only.** No behavior changes; §3's table is the proof. No program compiles differently, no warning appears or disappears, no store semantics move.
- **Newly rejected / newly warned / newly accepted:** nothing.
- **Compiler:** no changes required or permitted by this proposal; the implementation's existing hard-coded sets (#1393's enforcement) remain correct because the memberships are unchanged — an implementation may later choose to read the sets from a table shaped like §3's, which is an internal refactor outside this proposal.
- **Tests:** no behavior tests change. One documentation-consistency check is offered to `scripts/check_docs.sh` at graduation: recompute the three informative membership parentheticals from the stdlib fact lines and fail on divergence — mechanizing exactly the consistency the old prose maintained by hand (Q3).

## 8. Open questions

- **Q1 — fact names.** `synchronized` / `immutable` / `self-distributing` / `displacement-unbounded` as proposed? The fourth name is the weakest; alternatives considered: `payload-bearing` (too vague — every generic carries payload), `strand-costly`. Recommendation: keep `displacement-unbounded` (it names the exact recorded cost: the overwrite-displaced value's unbounded size).
- **Q2 — governance restatement.** Confirm §5's transformed no-silent-growth clause (facts as the sole ruling surface), or retain per-set ratification as a second gate? Recommendation: the former; the record requirement ("confront each derived-set consequence") preserves the deliberation the second gate existed to force.
- **Q3 — mechanical check.** Add the check_docs.sh consistency check (recommendation: yes — it converts the one new failure mode, fact/parenthetical drift, into a gate failure)?
- **Q4 — informative parentheticals.** Keep current-membership parentheticals beside each formula in the spec (recommendation: yes — the reader should not need the stdlib open to know today's answer), or formulas only?

## 9. Interactions

- **Guide 04 §8:** currently re-teaches the three lists in list form; at graduation it teaches the four facts and the three formulas — strictly less to hold in mind (the finding's own metric).
- **0088:** no contact (different §28.3 machinery).
- **0090:** no contact (no consume positions involved).
- **Front-half siblings:** none touch §22.12/§28.13 per the coordinator's index.
- **Future stdlib proposals** (0060's channel work; any new synchronized type): inherit §4's one-ruling addition path — the proposal's beneficiaries. 0003's `Locked<T>`/`RwLocked<T>` graduation record and 0032's `FrozenArray<T>` record are the provenance for those types' fact grants and are cited at the fact lines rather than re-argued.
