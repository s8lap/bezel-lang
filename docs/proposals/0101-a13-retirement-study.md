# Proposal 0101 — Study: retiring axiom A13 — does owning-choice drop-on-overwrite extend to owning-inline-array whole-value reassignment?

| | |
|---|---|
| **Status** | accepted — ruled under the owner's 2026-08-18 delegation; awaiting graduation train. Charter ruling C11 directs a **study**, in the 0089 mold: the outcome is **not pre-committed**. This document inventories the question, itemises the new evidence, confronts the recorded blockers, works three candidate rules, and ends in §7 with a recommendation and a stated confidence level — not with a fait accompli. |
| **Tracking issue** | coordinator files |
| **Target module(s)** | If adopted: **spec** — §28.7 (*Assignment to an owned field or owned local disposes the displaced value* — the carve-out clause), §15.3 (the *Whole-value reassignment* bullet — the warning-backed manual discipline), §15.1.1 (the owning-inline-array lifetime paragraph), §22.13/§28.3's form-(iv) static-field admission (which restates the carve), §28.3's exactly-once claim and its parenthetical, §28.11 (per-slot tracking at a reassignment point), the **axiom register (A13 → tombstone)**, Appendix C.3, Appendix D.2 (the Extended-level feature note). **rationale** — §5.26's A13 row. If declined: doc-only — §15.3 and the register gain the re-examined-and-kept record with this study as its evidence. |
| **Written against** | spec/stdlib **v0.255** (`git show HEAD:docs/*.md`, HEAD = `5fa5a95b`) |
| **Origin** | Charter C11. The direct antecedent is the 2026-07-21 ownership-kernel review, which ranked this as **C9, RECOMMEND** — *"the only shortlist item that improves the language rather than its text"* ([#1385](https://github.com/s8lap/bezel-lang/issues/1385) comment, 2026-07-21) — and the v0.252-era owning-choice ruling, which took the automatic behaviour A13 declines and said in its own text why. |

*Reference convention: unprefixed §-references are language-spec references; K/A tags are §28.3's kernel ids. Every quotation is from the v0.255 snapshot.*

---

## 1. The axiom, and what it costs

**A13, as the register states it (v0.255):**

| # | Axiom | Home | Recorded reason |
|---|---|---|---|
| A13 | Move-only inline-array whole-value reassignment does not dispose displaced slots (warning-backed) | §28.7, §15.3 | Recorded debt: interacts with the open inline-array-disposal design; **the one carve-out on the exactly-once claim** |

**The rule it licenses (§15.3, *Whole-value reassignment*):**

> `arr := newValue;` on a move-only local or field: the assignment is plain — **the compiler does not insert automatic cleanup of the previous slots. If the LHS holds any slot that is not in moved-from state at the assignment site, those slots leak.** The programmer satisfies the discipline by first emptying every owned slot of the LHS … The compiler emits a warning when reassigning …

**Its cost, counted in the spec's own hedges.** The exactly-once claim is the chapter's headline and A13 is the asterisk on it, restated at four sites:

1. §28.7 — *"the §28.3 claim — every owned value is disposed exactly once on every path — holds with **a single recorded carve-out** (axiom A13)"*.
2. §28.3 — *"These rules ensure that every owned class instance is disposed **exactly once** … (with drop-on-overwrite, the claim has **one recorded carve-out**: whole-value reassignment of a move-only inline array does not dispose the displaced slots)"*.
3. §28.3's disposal-form table footnote — *"Two carve-outs live outside this table: `Task`/`Task<T>` may not be discarded … and whole-value reassignment of a move-only inline array does not dispose the displaced slots."*
4. §22.13's form-(iv) static-field admission — *"whole-field reassignment keeps §15.3's warning-backed manual discipline (never auto-disposing displaced slots …)"*.

Plus §15.3's own asymmetry sentence and the register row. **Retiring A13 makes "disposed exactly once on every path" true without qualification** — the single sentence the memory model is sold on.

## 2. The new evidence: the owning-choice precedent

v0.200 introduced choices; the owning classification's rules now state (§35.4):

> - **Drop-on-overwrite is defined and automatic** (derives: K3.11). Whole-value reassignment of an owning-choice local or field *disposes the displaced value* per §28.7, like an owned class binding — **not** like §15.3's owning-inline-array reassignment rule (the warning-backed manual discipline): **that rule exists because an inline array's slots can be individually moved-from, leaving a partial state the compiler cannot cheaply dispose, and a choice has no partial states, so the sound automatic behavior is available and taken.**

And §28.7 states the same asymmetry from the other side:

> Owning-classified **choices** deliberately do *not* share the carve-out: a choice has no partial states, so its whole-value reassignment takes this rule's automatic drop-on-overwrite.

**Why this is new evidence and not merely a neighbouring rule.** When A13 was recorded (v0.161), no other move-only value type existed; the carve looked like *"move-only values are hard"*. Since v0.200 there are **two** move-only value kinds (§15.3: *"The move-only value type kinds in this specification are two: the owning inline array … and the owning-classified choice"*), and one of them **does** take automatic drop-on-overwrite. So A13 is no longer "the move-only rule"; it is a per-kind exception whose recorded reason has narrowed to exactly one claim:

> **the compiler cannot cheaply dispose a partial state.**

That claim, and only that claim, is what this study has to test. It is testable, because §15.1.1, §15.3, and §28.11 all state precisely what a partial state is, when it can exist, and what the compiler statically knows about it.

The kernel review reached the same reading a month before this study and recorded it on the issue: *"v0.200's owning-choice ruling already proved the drop-on-overwrite pattern and explicitly declined to share A13's carve"* (#1385, 2026-07-21).

## 3. The blockers, confronted

### 3.1 #747 — closed, and reclassified from blocker to dependency

[#747](https://github.com/s8lap/bezel-lang/issues/747) is the R42 adversarial-discovery batch (33 confirmed findings). Its relevant finding: *"`[N owned]T?` inline array slots not auto-disposed when array goes out of scope"* — i.e. the **scope-end** disposal machinery. #1385's opening text made A13's retirement conditional on it: *"the slot-wise displaced-value disposal interacts with the open #747 design (slot disposal registration, capture-boxing hazards, transfer walker). **Once #747's disposal machinery lands**, extend §28.7's rule."*

**Status at HEAD: #747 is closed (completed, 2026-06-17), and the machinery it gated is in the spec as a shipped rule.** §15.1.1: *"When an owning inline array's lifetime ends, the array's auto-dispose pass **disposes each non-null slot**. For locals, this happens at scope end via the standard owned-local discharge; for class fields, the enclosing class's destructor invokes the disposal."* §15.3 states it a second time under *Local lifetime*. So the disposal mechanism A13 was waiting for exists, is normative, and is the exact operation a drop-on-overwrite would perform — **one array, one non-null-slot pass** — differing only in *when* it runs.

The kernel review made the sharper point, which this study adopts: *"the recorded #747 interaction is a dependency of disposal **mechanism**, not of this rule — slot-wise overwrite disposal is a client of whichever mechanism #747 lands, so #747 should be checked at implementation time, not treated as a blocker"* (#1385, 2026-07-21). **Confronted and cleared**, with one implementation-time verification item retained (§6 item 1): the study asserts the *rule* is unblocked, not that the *codegen* is free.

### 3.2 #1385 — closed while the axiom it tracked is still live

[#1385](https://github.com/s8lap/bezel-lang/issues/1385) is the issue that tracked exactly this extension. It is **closed as completed (2026-07-22)** — the day after the kernel review's RECOMMEND comment — and **A13 is still in the v0.255 register.** The study reports this rather than resolving it: an issue tracking a debt was closed while the debt stands. The three recorded comments are consistent with a consolidation close (the v0.183 honesty parentheticals landed; the v0.188 graduation moved the category to Appendix D.2 Extended; the review re-ranked the work onto its own shortlist) rather than with the work having been done. **The graduation train must not read #1385's closed state as evidence that the question was settled**; nothing in the register, §28.7, §15.3, or §28.3 changed.

### 3.3 The stated blocker itself: "partial state the compiler cannot cheaply dispose"

This is the only substantive obstacle, and it decomposes into three separate facts the spec already fixes.

**(a) When can a partial state exist at a reassignment point?** Only via `move arr[i]` without restoration. §15.3's restoration obligation is stated *per exit edge*, not per statement (*"any slot moved-from via `move arr[i]` must be reassigned **before any exit edge**, because auto-dispose on a moved-from slot is not defined"*), so a whole-value reassignment can indeed be reached with slots moved-from. §15.3's own carve text concedes it (*"If the LHS holds any slot that is **not** in moved-from state at the assignment site, those slots leak"*) — i.e. the rule was written knowing both states occur.

**(b) What does the compiler statically know there?** §28.11, quoted:

> The compiler tracks moved-from state **per slot when the index is a compile-time constant or a local read**. For variable indices that the compiler cannot prove are constant within the method, the analysis is **conservative**: after `move coll[i]` with a non-constant `i`, the *entire collection* is treated as having one or more slots in moved-from state.

So the static picture at a reassignment point is total and two-valued: either a precise per-slot moved-from set, or "conservatively, unknown-which".

**(c) Why "cheaply" is the operative word, and what it actually costs.** A moved-from slot has **no runtime representation** — moved-from is per-method-body-static (the same fact K9.4 uses to ban `move` on static owned fields, and the same fact v0.223 used to refuse a terminal `return move x.f;`). So a runtime pass that disposed "every non-null slot" would double-free a moved-from-but-unrestored slot, and a pass that consulted a per-slot flag would need a flag word per slot that the type does not have. **That is the real content of the blocker, and it is correct as far as it goes.** What it does not establish is that the *rule* must therefore be manual — only that the rule must not require runtime moved-from state. §4 works three rules; two of them require none.

## 4. Three candidate rules

### Rule A — reassignment is an exit edge for the restoration obligation, then dispose every non-null slot

Extend §15.3's restoration rule so that a **whole-value reassignment of the LHS** counts as an exit edge for that location: every slot moved-from via `move arr[i]` must be restored before the reassignment. Then the store performs exactly the existing auto-dispose pass — *"disposes each non-null slot"* — on the displaced value, in reverse slot order, RHS-first per K9.5.

- **Runtime state required: none.** The pass is the one §15.1.1 already defines and #747's machinery already implements; it is sound because the restoration obligation guarantees no slot is moved-from at the store.
- **Static analysis required: none new.** The restoration check is the existing per-slot/conservative analysis of §28.11, run at one more program point.
- **Migration:** the moved-from-at-reassignment case becomes a **compile error** (today: a silent leak with a warning) whose remedy is one line — restore the slot, exactly as before any other exit. The non-moved-from case becomes **automatic disposal** (today: a leak with a warning).
- **Non-constant-index case:** conservatively "one or more slots moved-from" ⇒ restoration unproven ⇒ rejected, with the diagnostic naming the conservative tracking and the remedy (§28.11's own restoration operations). This is the rule's one sharp edge: a program that moves out at a computed index and then reassigns the whole array is rejected even where it is in fact safe.
- **Kernel:** pure K3.11 + K9.2 derivation. The reading is that an overwrite *is* an escape in K9's sense — *"the object must never escape the method with a hole a destructor could observe"* — and drop-on-overwrite is the destructor that observes it.

### Rule B — dispose exactly the statically non-moved-from slots (#1385's own sketch)

#1385's text: *"reassigning a `[N owned]T` location disposes **each non-moved-from slot** of the displaced value (reverse slot order), then stores."*

- **Runtime state required: none**, *provided* the disposal set is baked in statically at each store site (the compiler knows which slots to dispose and emits that exact list).
- **Static analysis required:** the per-slot set must be **precise**, so the non-constant-index case cannot be handled by disposing nothing (that leaks) or everything (that double-frees) — it must be rejected, exactly as in Rule A. So B degenerates to A's edge case while additionally requiring per-store-site slot-list codegen.
- **Migration:** strictly gentler than A — the moved-from case keeps compiling and disposes the rest.
- **Cost:** a second, weaker notion of "restored" enters the language (a slot may be permanently moved-from and simply skipped), which sits awkwardly beside §15.3's *Local lifetime* rule that a moved-from slot must be restored before scope end anyway. A program that is legal under B at the store is illegal three lines later at the exit edge, for the same slot.

### Rule C — status quo: A13 stands

Keep the warning-backed manual discipline; keep the asterisk. Costs: the four restatements, the register row, the asymmetry sentence, and the fact that the language's headline invariant is true "with one carve-out" that a reader must chase across two chapters to understand.

## 5. What the study can and cannot settle

**Settled by the texts, with confidence:**

- The blocker's *mechanism* half is discharged: the disposal pass exists and is normative (§3.1).
- The blocker's *partial-state* half is real but does not force manual discipline: two of three candidate rules need no runtime moved-from state (§4).
- The precedent is genuine and the asymmetry's own stated ground is exactly the claim under test (§2).
- The corpus cost is small: the shape A13 governs — a whole-value reassignment of an owning inline array — is rare, and the move-only value category is **Extended-level** (Appendix D.2, since v0.188), so Core conformance is unaffected either way.

**Not settled here, and deliberately left to the graduation train or to a follow-up:**

- **The measured corpus count.** This study did not enumerate `[N owned]T` whole-value reassignment sites in `tests/`, because the shape is not greppable with confidence (it is an ordinary `x := y;` whose types decide it) and a wrong number is worse than none. **§6 item 3 makes it a gate item**: the number must be produced by the implementation lane from the type checker, not by grep.
- **Whether the non-constant-index rejection is acceptable in practice.** It is the only place either candidate rule makes a legal program illegal for conservatism rather than for unsoundness. With no external users there is no evidence either way.
- **The `[N owned]T` (non-nullable) versus `[N owned]T?` (nullable) split at the store.** §15.1.1 restores a nullable slot with `null` and a non-nullable slot with a fresh value, so the non-nullable form has *no* representable empty state and its "dispose each non-null slot" pass is unconditional. Whether that makes the non-nullable case strictly easier (no null test) or introduces an asymmetry worth stating is a drafting question, not a design one.

## 6. Gate items for whoever executes this

1. **Verify the disposal mechanism at the store site.** §3.1 asserts the overwrite pass is the scope-end pass relocated. Confirm against the shipped codegen that it is literally reusable (the "slot disposal registration, capture-boxing hazards, transfer walker" list #1385 named) rather than a second implementation.
2. **Verify K9.5's RHS-first ordering composes.** §28.7's rule is RHS-first (*"drop-on-overwrite (K3.11) is RHS-first, so a throwing RHS leaves the slot untouched and still-owned"*). For a move-only RHS the store is itself a move, so the study flags but does not resolve: does `arr := other;` where `other` is a move-only local leave a coherent state if the disposal pass throws mid-way? §28.4's multi-value cleanup rule appears to govern (the pass is a multi-value cleanup), and proposal 0097's supersession snapshot would record the losses — **state the composition explicitly rather than deriving it.**
3. **Produce the corpus count from the type checker**, not from grep (§5).
4. **Check self-assignment and aliasing.** `arr := arr;` on a move-only value: the RHS is a move of the LHS. Today the store is plain and the question is invisible; under A or B the disposal pass would run against a value that has just been moved into the same location. Rule it explicitly (the natural answer: the RHS move leaves the LHS moved-from, so the restoration obligation is unsatisfied and A rejects it — but say so).
5. **Register arithmetic, coordinated.** At v0.255 the register is *"Twelve axioms (and two tombstones)"* — twelve live. Retiring A13 gives **eleven live, three tombstones**. **Proposal 0096 retires A2 on the same programme**; if both graduate, the count is **ten live, four tombstones**. The two trains must not both write "eleven live". *(Note: #1385's 2026-07-21 comment says retirement "drops [the register] to twelve live axioms" — that was the count under the then-current register; it is stale arithmetic, not a discrepancy.)*
6. **Appendix D.2.** The category is Extended-level; whichever way this goes, the level annotation and the "Core's affine claim is clean by exclusion" reading recorded at v0.188 must stay true.

## 7. Recommendation, with confidence

**Recommendation: adopt Rule A — reassignment counts as an exit edge for the restoration obligation, then the existing non-null-slot disposal pass runs on the displaced value — and retire A13 to a tombstone. Confidence: medium-high.**

The reasons, in order of weight:

1. **The recorded reason has been tested and does not survive as stated.** A13's ground is *"the compiler cannot cheaply dispose a partial state."* Rule A never disposes a partial state: it requires the state not to be partial at the store, using an obligation the language already imposes at every other exit edge, and then runs a pass that already exists. The blocker is answered rather than worked around.
2. **The precedent is exact and is the language's own.** The owning choice takes automatic drop-on-overwrite for the stated reason that it has no partial states; Rule A removes the inline array's partial states at the one point that matters. After it, the two move-only kinds agree, and §15.3's asymmetry sentence deletes rather than being explained.
3. **The payoff is the headline invariant.** *"Every owned value is disposed exactly once on every path"* becomes true with no asterisk, at four spec sites plus Appendix C.3 — and the register loses its only row whose recorded reason is the word *debt*.
4. **The timing is the cheapest it will ever be.** Zero external users; an Extended-level feature; a shape that is rare in the corpus.

**Why not higher than medium-high**, stated so the ruling is made with the doubt visible:

- **Rule A newly rejects a class of program** (moved-from-at-reassignment, and every non-constant-index case) that today compiles with a warning. Turning a warning into an error is a real tightening, and with no users there is no evidence about how often the conservative case is hit. Rule B avoids the first half of that at the cost of a second notion of restoration; the study prefers A for coherence but does not think B is wrong.
- **Gate items 1, 2, and 4 are unresolved.** None of them looks likely to overturn the recommendation, and any of them could change the rule's exact statement — particularly item 2, where a throwing disposal mid-pass interacts with §28.4 and with proposal 0097.
- **The corpus count is unmeasured** (§5), and a recommendation that has not counted its own migration is one honest number short of complete.

**If the ruling is instead to decline**, the study is its record: §15.3 and the register gain a *re-examined-and-kept* note naming the choice precedent as considered, the mechanism blocker as cleared, and the conservative-rejection cost as the surviving reason — so that the next reviewer starts from Rule A versus the conservative-rejection cost rather than from the mechanism question, which is finished.

## 8. Interactions

- **Proposal 0096 (this programme)** — the register arithmetic (§6 item 5). No text overlap: 0096 is §27/§28.3/§29 announcement machinery, this is §15.3/§28.7 store semantics.
- **Proposal 0097 (this programme)** — §6 item 2: a throwing disposal inside the overwrite pass is a §28.4 multi-value cleanup, and 0097's `SupersededChain` is what preserves the earlier failure. The two should cite each other if both land.
- **Proposal 0036 (graduated v0.188)** — moved the move-only category to Appendix D.2 Extended; #1385's second comment records the interaction and the reading that survives either outcome.
- **§35.4 / the owning-choice classification** — the precedent; untouched by either outcome.
- **§22.13 form (iv)** (owning-inline-array static fields) — restates the carve and must be edited in step under adoption; note that its *own* disposal story is the program-exit pass, which is unaffected.
- **[#747](https://github.com/s8lap/bezel-lang/issues/747), [#1385](https://github.com/s8lap/bezel-lang/issues/1385)** — both closed; §3 records what their closure does and does not establish.
