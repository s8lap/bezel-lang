# Proposal 0092 — Named-destructure transfer: the `move` slot in an owning match

| | |
|---|---|
| **Status** | under-review — drafted 2026-08-12 (owner-directed, the Train E follow-on questions) |
| **Tracking issue** | [#3087](https://github.com/s8lap/bezel-lang/issues/3087) |
| **Target module(s)** | **spec** — §19.3 (*Named destructuring* gains the marked-slot form; the *Named-slot bindings and ownership* paragraph's "a named slot never transfers" sentence narrows to unmarked slots), §35.5 (*Case patterns* bullet 3 amended; one clause in the owning-match section), §19.6 (one parenthetical: a marked slot is an irrefutable binding), Appendix A.7 (`NamedSlot` gains the optional `"move"` prefix), Appendix C.7 (one invariant row). **rationale** — §3.14's v0.246 note resolves (pointer to the decision record). **guides** — Guide 3 gains the by-name extraction example at graduation. **No stdlib change. No new tokens** (`move` is reserved, Appendix B.1). |
| **Written against** | spec/stdlib v0.248 |
| **Origin** | E-f2, the second of the two owner-confirmed open questions recorded at the v0.246 graduation of proposal 0090 (changelog v0.246; rationale §3.14's note; 0090's inventory row G5; proposal 0065 Q2's "liftable later" record) — directed to a full proposal by the owner 2026-08-12 ("Please work to address all"). |

*Reference convention: unprefixed §-references are language spec references; rationale references are marked; K/A tags are §28.3's kernel ids.*

---

## 0. Thesis

In an owning match (§35.5), the positional slot transfers and the named slot cannot: `Fetched(var r)` hands the arm an owned `Response`, while `Fetched(response = var r)` — the same payload, interrogated by name — binds a borrow and the payload is disposed at construct completion, whatever the arm meant to do with it. The only spelling of extraction is positional, so the by-name form 0065 built — the form that survives payload reordering and reads its field names at the pattern — is locked out of exactly the matches that extract:

```bezel
case client.Fetch(url) of                      // owning match (§35.5 trigger 1)
  Fetched(var r)            => RenderOwned(move r),   // positional: transfers — today
  ...
case client.Fetch(url) of
  Fetched(response = var r) => RenderOwned(move r),   // named: compile error today —
  ...                                                 // r is a borrow; the payload dies
                                                      // at construct completion (§19.3)
```

The restriction was the right cut at v0.224: 0065's Q2 recorded it "liftable later," and the reason it was not lifted then is recorded at v0.246 — a marked named slot would have spelled the marker `owned`, deepening the state/action conflation the marker unification existed to remove. That blocker is gone: `move` is now the one verb for emptying a named location (rationale §3.10), and named slots have a natural transfer spelling for the first time (the E-f2 record's own words).

**Proposed:** a named slot may carry the take-out verb on its **source** — `move Field = var x` — admitted exactly in an owning match, on an obligation-bearing payload field, with a binding RHS:

```bezel
case client.Fetch(url) of
  Fetched(move response = var r, status = var s)   // PROPOSED — r: owned Response in
    => RenderOwned(move r, s),                     // the arm; s: a value copy
  NotModified          => RenderCached(),
  Failed(var s, var m) => ShowError(s, m)
end;
```

Unmarked named slots keep their landed borrow meaning verbatim; positional slots are untouched; no kernel rule changes (§3 shows the derivation — no new K position, no new axiom).

## 1. The exact form

**Grammar delta (Appendix A.7) — one optional token:**

```
/* current (v0.248) */
NamedSlot        ::= Identifier "=" AltPattern

/* proposed */
NamedSlot        ::= ("move")? Identifier "=" AltPattern
                              /* marked slot (proposal 0092): admitted only in an
                                 owning match (§35.5) over a choice scrutinee, only
                                 naming an obligation-bearing payload field (an
                                 `owned` payload or a move-only-typed payload,
                                 §35.4), and only with a top-level binding RHS
                                 (`var x`, v1) — all enforced semantically
                                 (§19.3). `move` is reserved (B.1) and begins no
                                 slot today: one-token commit, no lookahead. */
```

**Parse commitment.** Inside a destructure's parentheses, no current slot begins with `move` (positional slots are patterns; pattern-embedded expressions parse at Shift level and their `move` forms are already semantically rejected under axiom A7's everywhere-else clause), so the marked reading commits at its first token — stronger than the named form's own two-token `Identifier "="` commit. The no-mixing rule is untouched: a marked slot is a named slot; one marked slot makes the list named.

**Why the marker sits on the source, not the binder.** Everywhere the verb exists, its operand is the location being emptied: `move x` (a binding), `move x.f` (a field), `move coll[i]` (a slot). A payload field is a field; `move response = var r` is the pattern-position analog of the field take-out — "empty `response`; it flows into fresh `r`" — reading left-to-right along the slot's existing source `=` disposition shape. The two candidate spellings that put the verb on the receiver are weighed and declined in §7 (both make `move`'s operand the one thing it never is elsewhere: a fresh name).

## 2. Semantics

**The normative statement (§19.3/§35.5 amendment sketch):**

> A named slot may carry the transfer marker: `move N = var x`. The marked slot is admitted exactly when **(i)** the match is an **owning match** (§35.5 — either trigger), **(ii)** `N` names an **obligation-bearing payload field** of the matched case — an `owned`-marked payload, or a payload of move-only value type (§15.3, §35.4) — and **(iii)** the slot's pattern is a top-level binding (`var x`; v1). On arm selection the payload transfers into the binding exactly as the corresponding positional slot would: `x` is an owned local (for a move-only payload, an owned move-only local) of the arm body's scope, under §35.5's standard arm-binding rules. An **unmarked** named slot keeps its borrow reading unchanged — borrowed during the arm, the payload disposed at construct completion. The marker elsewhere is a compile error naming the failed condition: on a slot of a **borrowing match** (the scrutinee retains ownership — nothing transfers; match the temporary, or write `case move x of`), on a **value, `string`, or runtime-managed payload** (no obligation to transfer), on a **record or class** pattern head (no obligation-bearing slot can exist there: records carry no `owned` fields, §28.5, and a class getter may not mint an owned value, §28.6/A4 — so marked slots are choice-case-only *by derivation*), or with a **`_` RHS** (`move N = _` is rejected as redundant: a discarded payload is already disposed at construct completion — one spelling per site, the v0.246 Q2 discipline).

Worked consequences, each traced to the machinery that already defines it:

1. **Guards observe borrows; transfer happens once, at selection.** §35.5's rule — "pattern bindings are borrows of the scrutinee's payloads during pattern matching and guard evaluation; ownership transfers into the selected arm's bindings when an arm is selected" — applies to marked slots verbatim: a guard-declined arm transfers nothing (trace (c) unchanged); two arms marking the same payload are two candidate recipients, not two owners.
2. **Partial transfer: the un-transferred remainder rides the existing traces.** Fields not listed (the named form's subsetting), named-but-unmarked fields (borrowed during the arm), and `_`-discarded positional siblings are all disposed **at construct completion** by the machinery both triggers already run — trigger 1's unbound-temporary pass (K8.3), trigger 2's payload-remainder disposal. Trace (b) is the normative trace, unchanged.
3. **Exception edges are trace (d), unchanged.** After selection, a marked binding is an owned local of the arm scope: an exception out of the arm discharges it by K3.15 exactly as a positional transfer's binding; before selection nothing has transferred and the consumed scrutinee's payloads are disposed during unwinding by the standard pass. Mid-destructure faults reduce to these two cases — slot reads themselves cannot fault here (payload reads are field reads, not getters; getters occur only at class heads, where the marker is inadmissible).
4. **The announcement discipline (§28.3) is satisfied where it always was: at the scrutinee.** The owning match's two triggers *are* the announcement — a fresh temporary, or `case move x of`. The slot marker is not a second announcement of the same consume; it disambiguates **two in-construct readings** — the named slot's landed borrow reading versus transfer — which is precisely §35.5's own justification for the trigger-2 marker: *"scrutinee position has an unmarked borrow reading to disambiguate, so the marker carries meaning."* The same sentence, one level down. (Positional slots have no unmarked-borrow reading in an owning match, so they correctly take no marker — writing one there is not even grammar.)
5. **The destructured source's disposal is unchanged in both triggers.** Trigger 1: the temporary's full expression is the whole construct (K8.3); transferred payloads leave it, the remainder and the container are reclaimed at completion. Trigger 2: the named scrutinee is moved-from after `end` (K3.4), its payloads routed or disposed identically. The marked slot changes *routing within an already-consumed value*, never *whether* or *when* the value is consumed.
6. **Exhaustiveness: a marked slot is an irrefutable binding.** It contributes under §19.6's named-destructure clause exactly as `N = var x` does; one parenthetical in that clause's binding list. (RHS is a binding by rule (iii), so a marked slot can never be the refutable slot that voids an arm's contribution.)
7. **Owning vs borrowing scrutinees.** Over a borrowing match — an unmarked named-binding scrutinee, a field read, a parameter, `this` — the marked slot is the consequence-0 error: the scrutinee retains ownership and no slot can mint an obligation from it. This mirrors "the marker on a borrowed scrutinee is an error" one level down, with the same remedy naming (`case move x of`, or match the temporary).
8. **Nesting does not arise (v1).** Rule (iii) fixes the RHS to `var x`; and an `owned` payload is class- or interface-typed (§35.5's existing sentence), so nested destructure-transfer through it cannot be spelled. A move-only-*choice* payload nested deeper stays whatever the positional form is today — out of scope, recorded liftable with typed binders (`var x: T`) in Q2.

## 3. The K-derivation (no new axiom)

| Edge | Covering rule (existing) |
|---|---|
| the consume itself | the owning match's scrutinee consume — trigger 1: K8.3 temporary; trigger 2: K3.4's marked scrutinee (§35.5). **K3's enumeration gains no row** — the marked slot is routing inside an already-enumerated consume, exactly as positional slots are today. |
| transfer moment | §35.5 *Guards: borrow during trial, transfer on selection* — verbatim (consequence 1) |
| the binding's life | §35.5's owning-match arm-binding rules (owned local; must-use; adoption; K3.15 on exception) — verbatim |
| remainder disposal | trace (b)/(d) machinery at construct completion / unwinding — verbatim (consequences 2–3) |
| freeze | unchanged — trigger 2's K5.1 freeze is about the scrutinee binding, marker or no marker on slots |
| no-silent-consume (K3.13) | strengthened in spirit: the unmarked named slot still never transfers; transfer is spelled |
| axiom A7 | untouched — a `NamedSlot`'s `move` is pattern grammar, not an expression-position `MoveExpr`; A7's expression-operand bounds (`??`, operator operands) are not in contact |

The acceptance test, in 0090's terms: every K-tagged rule reads unchanged; the delta is one admission (a second slot spelling routes a payload the construct already owns) plus its rejection set.

## 4. What the marked slot is not

- **Not a way to consume a borrowed value.** Rule (i): owning matches only. The borrowing match's named slots — including over an owned scrutinee read without the trigger-2 marker — stay pure interrogation.
- **Not a second announcement.** Consequence 4: the scrutinee announces the consume; the slot marker selects a reading.
- **Not available to records or classes.** By derivation, not fiat (consequence 0's error taxonomy): no obligation-bearing slot exists outside choice payloads.
- **Not a change to any unmarked spelling.** `Fetched(response = var r)` means at v0.249+ exactly what it means at v0.248 — the additive widening 0065's Q2 record promised.

## 5. Corpus evidence

Counted at v0.248 (committed HEAD):

- **The named-destructure form has zero behavioral corpus.** The compiler does not yet implement it (no named-slot machinery in `src/`; the 0065 impl tracker #1409 is still queued behind the R0 drain), the test corpus has **0** named-slot usages, and the guides carry **no** named-destructure example (the form lives at its spec definition sites: §19.3 ×2 paragraphs, §19.6 ×1, §35.5 ×1, A.7). The lift's entire migration surface is definition text — the cheapest it will ever be.
- **The owning match is small and settled:** `case move` — spec 9 / stdlib 1 / tests 4 (plus 5 pre-#3056 `case owned` lines awaiting the v0.246 respell); the four disposal traces (§35.5 a–d) are the complete behavior this proposal routes through, none modified.
- **`move` in slot position: 0 occurrences** — the marked form does not parse today; fresh spelling, no collision (`move` reserved since the verb's introduction, B.1).
- **The want is recorded, not manufactured:** 0065 Q2 ("a transfer-through-named-slots form is liftable later"), rationale §3.14's v0.246 note, 0090's G5 row, and the v0.246 changelog's E-f2 record form an unbroken paper trail; this proposal is that trail's terminus, not a new direction.

## 6. Migration and compatibility (changelog categories)

- **Newly defined:** the marked named slot (grammar + §2's admission/rejection set).
- **Newly rejected:** nothing that exists — the form does not parse at v0.248.
- **Nothing flips:** every landed spelling keeps its meaning; in particular the unmarked named slot's borrow trace in an owning match is preserved verbatim (§4).
- **Interface carriage:** none — patterns are body content; §2.8.2 is not in contact.

## 7. Alternatives considered

1. **Status quo (positional-only transfer, the current rule restated).** Declined: it survives only on the v0.224 blocker — "no marker spelling exists that would not deepen the `owned` conflation" — which v0.246 dissolved; keeping it now means the by-name form is permanently second-class in exactly the matches that extract, with the workaround (re-order to positional) forfeiting the named form's reason to exist.
2. **Unmarked symmetry** — named slots transfer in owning matches exactly as positional slots do; no new syntax. The strongest rival: fewer spellings, full positional/named symmetry. Declined on three grounds: it **flips a landed defined behavior** (v0.224's named-borrow trace) where the marked form is purely additive — the house migration discipline prefers the widening that breaks nothing; it deletes the named form's per-slot peek (borrow one payload while transferring another — expressible in no other form); and it leaves the reader no cue at the slot, where the marked form makes the one consuming slot visible in a list of interrogations.
3. **`Field = move x`** (the E-f2 sketch's surface form — binding-with-transfer, `x` fresh). Declined: `move`'s operand would be a fresh name — the one place the verb's operand is not the location being emptied — and it collides with 0065's naked-identifier doctrine (after `=`, a bare identifier is a constant reference, never a binding; `move x` would make it one).
4. **`Field = move var x`** (the marker on the binder). Declined: the verb attaches to the receiver; every other `move` empties its operand (§1's placement argument). Same operand inversion as 3, one token later.
5. **`Field = owned var x`** (the `out owned var x` echo). Declined: it reintroduces `owned` at a site performing an action — the exact conflation v0.246 removed. The out-position form survives *because* nothing is consumed at its site (the callee produces into a fresh binding — declaration ↔ declaration, 0090 Q5); a slot form empties an existing payload slot, which is an action, which is `move`'s side of the line.
6. **Rejecting named patterns under owning matches outright** (0065 Q2's stricter alternative). Declined then, declined now: it forbids the harmless peek the borrow reading legitimately provides.

## 8. Implementation sketch

- **parser:** the optional `move` in `NamedSlot` — landed as part of the named-destructure implementation itself (#1409, still queued), not as a delta to shipped code; there is no shipped named-slot parser to amend.
- **sema:** the §2 admission checks (owning match; obligation-bearing payload; binding RHS) on the slot; the marked binding enters the arm through the owned-local path §35.5 already defines for positional transfers; the five rejection diagnostics of §2's error taxonomy, each naming its rule and remedy.
- **codegen:** the positional payload-transfer lowering, keyed by field name instead of position — no new lowering kind.
- **tests** (spec-cited per `tests/README.md`): the transfer trace (owned binding in the arm, adopt idiom, exactly-once); guard-declined (trace c); exception mid-arm and pre-selection (trace d); partial transfer — unlisted, unmarked-named, and discarded siblings disposed at completion (trace b); the marked-slot-in-borrowing-match, value-payload, record-head, class-head, and `move N = _` rejections; trigger-1 and trigger-2 parity; the §19.6 contribution parenthetical.
- **Sequencing:** behind #3056 (the marker respell) and with/behind #1409 (named destructure itself); docs-first graduation is independent of both.
- **Size:** small — one grammar token, one admission check, reuse of every disposal path.

## 9. Questions for the owner

- **Q1** — adopt the source-marked slot `move N = var x` as specified (§1–§2)? Recommended. (The unmarked-symmetry rival is Q1's named alternative — §7.2 — if the owner weighs the flip differently.)
- **Q2** — v1 RHS fixed to `var x` (recommended), with typed binders (`var x: T`, the refutable subtype-test form — transfer-on-selection already handles refutability) and nested move-only-choice routing recorded liftable?
- **Q3** — `move N = _` rejected-as-redundant (recommended — the discard already disposes; one spelling per site) or admitted as an explicit dispose-here marker?
- **Q4** — the marker on a *value-payload* slot: error (recommended — nothing to transfer, matching §22.3's marker-on-borrow error shape) or admitted-but-redundant (the §28.1 move-only precedent — weaker here, since no substitution ever makes it meaningful on a concrete value payload)?
- **Q5** — confirm the borrowing-match rejection's remedy wording names both routes (`case move x of`; match the temporary) — recommended, the teach-the-fix convention.

## 10. Interactions

- **Proposal 0091 (sibling, this train):** both extend the marker's reach; surfaces disjoint (argument roots there; pattern slots here). Jointly they close E-f1/E-f2 with one invariant intact: `move`'s operand is always the location being emptied — 0091 adds no operand kind, this proposal adds the payload field, a field.
- **Proposal 0065 / rationale §3.14:** Q2's liftable is discharged; §3.14's v0.246 note resolves to this record at graduation; 0065's settled forks (the `=` spelling, no-mixing, naked-identifier doctrine) are untouched and load-bearing here (§7.3).
- **Proposal 0090 / axiom A7:** not in contact (§3's last row) — the slot marker is pattern grammar; A7's expression-position bounds and its `??` gate are unmoved.
- **Proposal 0056:** the consume-if-selected precedent (borrow during trial, transfer at selection) is the mechanism consequence 1 rides; nothing re-litigated.
- **#3056 / #1409:** sequencing only (§8).
- **§19.6:** one parenthetical (consequence 6); the one-level-deep, no-value-space posture is untouched.
- **v0.249 train (in flight):** this proposal cites v0.248 texts only and touches no normative file; its graduation targets (§19.3/§35.5/§19.6/A.7/C.7, rationale §3.14, Guide 3) have no overlap with that train's files except the changelog, which is graduation-time work.
