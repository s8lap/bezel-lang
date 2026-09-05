# Fork-join gate status, 2026-09 — the Stage-2 landing gate (#1725) is not reachable; one owner decision would make it decidable

*Supporting record (not a numbered proposal). Written against spec/stdlib v0.291 and verified 2026-09-05 by source read, no build; spec text is cited by section, anchor and quoted phrase so the record survives line drift. Companion to rationale §12.9 and proposal 0032. Tracking issue: [#1725](https://github.com/s8lap/bezel-lang/issues/1725), which stays open.*

## 1. What the gate says

#1725 names the landing gate for the in-place fork-join extension recorded at rationale §12.9 (*The recorded fork-join extension (designed, not shipped)*, v0.186): "its own ruling-class revision", with five dependencies to clear first. The normative posture today is spec §30.14's *Recorded, not normative* paragraph (`Bezel_Language_Spec.md`, anchor `sec-multiple-domains`): "a **designed future extension** whose rulings and soundness analysis are recorded in the design rationale … Nothing in this revision reserves grammar or adds checking rules for it; it lands, if it lands, through its own ruling-class revision." `DECLINED.md` carries it as `deferred` (*In-place fork-join data parallelism*).

## 2. The five dependencies, verified

| # | Dependency (the issue's words) | Status at v0.291 | Evidence |
|---|---|---|---|
| 1 | the frozen-view / lend-span family design, "final naming checked against landed 0027's `SplitSpans` surface" | **Blocked.** Proposal 0027 has not graduated: `accepted — accepted (2026-07-07, delegated ruling — owner review pending)`; `SplitSpans`/`StringSplitCursor` appear nowhere in the stdlib. | `docs/proposals/0027-splitspans.md:5`; `grep -c 'SplitSpans\|StringSplitCursor' docs/Bezel_Standard_Library.md` → 0; `DECLINED.md` row *Frozen span/view forms* (`deferred`) |
| 2 | the §30.14 location-granular invariant amendment (text pinned at rationale §12.9 item 5) | **Not landed, by design.** §30.14's flagship sentence (anchor `sec-multiple-domains`) still reads "**no mutable state is ever reachable from two domains**", and it has a second carrier — §30.13's memory-model close restates it ("Beyond this enumeration there is nothing left to specify, by construction: no mutable state is ever reachable from two domains"). The pinned replacement differs in three words beyond its exception clause ("mutable memory **location** … **accessible** … two **running** domains"); the landing must reconcile both carriers, not paste. | spec §30.14; spec §30.13 (*Cross-domain and cross-thread visibility*, anchor `sec-cross-thread-visibility`, closing paragraph); `docs/Bezel_Design_Rationale.md:1787-1794` |
| 3 | the fork-extent blocking-acquisition guard enumeration, including §6.11's data-owning locks | **Ingredients exist, enumeration does not.** `Locked<T>`/`RwLocked<T>` shipped (stdlib §6.11, v0.186); §30.12 *Thread synchronization* states the blocking family; no rule anywhere raises `InvalidOperationException` inside a fork extent. | as cited; `grep -c 'fork extent' docs/Bezel_Language_Spec.md` → 0 |
| 4 | the sharpened rule-4-statics fork residual | **Recorded, not discharged.** §22.12 rule 4 (v0.291's verdict-table form: "confining its use to one domain — or ensuring its own internal synchronization — is the programmer's obligation") states the one-domain confinement obligation, and its note (a) names the locks; no fork-extent clause. Rationale §12.9 item 4 records the residual and the obligation the landing must extend. | spec §22.12 rule 4 and note (a); `docs/Bezel_Design_Rationale.md:1782-1786` |
| 5 | a fork-extent memory-model clause | **Does not exist — but its home does.** §30.13's *Cross-domain and cross-thread visibility* is a numbered list of **five** happens-before edges (mutual exclusion; channels; spawn and join; task settlement; static publication), followed by two object-specific guarantees. The clause is a **sixth** edge (fork-extent entry / join completion), not new vocabulary. | spec §30.13, anchor `sec-cross-thread-visibility` |

Beside the five: `Parallel` itself is entirely unimplemented and says so (stdlib §6.12's ⚠ marker, v0.284 Option B, [#3557](https://github.com/s8lap/bezel-lang/issues/3557)); every `Parallel` token in `src/` is a comment (`src/common/BuiltinRegistry.cpp:660-668` names `Parallel.ForEach` only to say the retired instance `ForEach` is unrelated). Stage 2 would be a second unshipped layer over an unshipped one — not a bar (the spec routinely leads the compiler, process §5 item 3), but worth saying before the effort is spent.

## 3. What a ruling-class landing would actually contain

Six normative artifacts, in dependency order, plus the process §9 obligations:

1. A **`concurrently-readable` derived type property** — a new §3.6 row (the `TABLE` check in `check_docs.sh` polices the row's anchor and body), its computation and exclusions (locks, channels, the task family), checked only at fork positions.
2. **Fork-position closure admission** — the `par` marker as a §28.14-family contextual modifier: grammar (Appendix A), the write-restriction rule, the capture freeze, and an **axiom-register** entry (§28.14 carries A10/A11; a fork admission is a new axiom or a derivation, and process §9's kernel line requires the classification to be stated).
3. **Disjoint span lending** — a lend-span type family in the stdlib (no `Buffer` projection, `Slice`-only narrowing) and the provenance-linear `SplitAt` admission; naming checked against 0027 per the gate — which is dependency 1's block.
4. The **§30.14 invariant amendment**, reconciled against both carriers (item 2 above).
5. The **blocking-acquisition guard** — an enumeration over §30.12's whole blocking family as of the landing revision, `InvalidOperationException` inside a fork extent, §6.11's locks included.
6. A **sixth happens-before edge** in §30.13's list (fork-extent entry; join completion).

Obligations: kernel classification (certain §28 contact); body-locality classification (the fork admission consumes cross-body facts — a carve-out or an invariant amendment, proposal 0105's two heaviest classes); `DECLINED.md` rows for the alternatives rationale §12.9 already declines (`parallel for`, move-capturing spawn bodies, user-generic parallel libraries, the runtime send-check, the awaiting `Join` variant — the first three already have rows); Guide 04 revalidation; the conformance corpus for every new fence.

## 4. Change class — a correction to the issue's framing

The issue calls the landing a "ruling-class revision". Items 1–3 and 6 add a language modifier, a §3.6 property, new stdlib types, and a memory-model edge: under process §2 that is a **design change**, which "must already be an accepted proposal (§6)" before a ruling-class landing. Proposal 0032 *is* that accepted proposal (`graduated-v0.186 — superseded`, Stage 2 "recorded design-now/ship-later"), so the framing survives **only if the landing is scoped to exactly 0032's recorded Stage 2 with no new design**. Any deviation — and re-scoping dependency 1 off 0027 is one — reopens §6 with a fresh proposal.

## 5. The owner decision this memo asks for

Dependency 1 makes the gate structurally unreachable until one of two things happens, and neither is a lane's call:

- **(a) Sequence 0027's graduation first.** 0027 has been `accepted — owner review pending` since 2026-07-07; graduating it lands `SplitSpans`/`StringSplitCursor` and gives dependency 1 its naming reference. The Stage-2 landing then follows as 0032's recorded design.
- **(b) Re-scope dependency 1 off 0027.** Design the lend-span family against the current stdlib surface alone. This is a deviation from 0032's recorded Stage 2 and therefore a new proposal under §6 — smaller than 0032 but not a ruling.

Until one is chosen the gate stays where it is, the issue stays open, and nothing in this memo changes a normative document.

## 6. Size, for planning

Assuming (a) and no design changes: 30–50 h of drafting across five normative documents (spec §3.6/§28.14/§30.12/§30.13/§30.14/§22.12 + Appendix A + the axiom register; stdlib §4.14–§4.16 lend-span family + §6.12; rationale §12.9 rewritten from *recorded* to *shipped*; changelog; Guides 01 + 04), plus `DECLINED.md`, the code-generation reference, and the conformance corpus. Under (b), the proposal precedes all of that.
