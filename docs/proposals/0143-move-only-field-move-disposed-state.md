# Proposal 0143 — A whole-value `move` of a move-only field leaves its storage in the disposed state

| | |
|---|---|
| **Status** | graduated-v0.328 — superseded — graduated v0.328 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option (a); critique round 1 applied, 2 blocking and 6 non-blocking findings resolved, shared with the #5629 ruling and the two v0.327 filings ruled beside it (a field at a bare type parameter is set-erasing; a lambda body is outside the `readonly` window); the alias direct read and the tombstone encoding added by that round; landed with the v0.328 changelog entry) |
| **Tracking issue** | none filed before this proposal: the question is the v0.327 `dI` critique's NB5 (`scratch/v0327/dI.critic.md`), recorded in rationale §9.15's #5554 paragraph as "filed, not decided"; the compiler tracker is filed at the v0.328 landing (§7) |
| **Target module(s)** | **spec** — §9.4 (*Move-only receivers*: the disposed-state entry sentence and the overwrite sentence), §15.1.1 (the `move arr[i]` paragraph), §15.3 (*Local lifetime*), §19.3 (*Pattern binding and ownership*), §28.3 (*The consume-site freeze*, *Scope*), Appendix C.5 (the move-only receiver row); **rationale** — §9.15 (one sentence replaced, one paragraph added); **code-generation reference** — §7.3, one *Looks right, is rejected* pair; **`DECLINED.md`** — three `declined-at-proposal` rows |
| **Written against** | spec/stdlib/rationale/changelog **v0.327** (drafted on the composed tree over `c69ff18`; every anchor re-verified at HEAD `478a6de`, v0.327 landed). Section numbers are v0.327's. |
| **Origin** | Proposal 0135's second critique (N7) recorded a hazard v0.321 left to a filing; v0.327 ruled its holder-kill half ([#5554](https://github.com/s8lap/bezel-lang/issues/5554)) and its critic rewrote the adjacent whole-value-move case as a design-change-sized question, recommending option (a) below with (c) as the floor. |

*Reference convention: unprefixed §-references are language-spec references at v0.327 as composed; K tags are §28.3's kernel ids. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

A borrowing-match binding over an owning-inline-array **instance field** (`case h.Slots of var a => …`) addresses the field's storage in place. A whole-value `move` of that field during the arm (`var t := move h.Slots;`, spelled directly or through an alias of the holder) moves the value into `t` and leaves the field moved-from under K9.2's restoration rule; nothing is disposed and no flag is set. The binding keeps reading the field's storage, whose bytes now describe slots `t` owns: a class-typed slot's object is flag-backstopped (K4.1) when `t` is disposed, a boxed value in an interface-typed slot is not (§15.1.1: a released box has no flag). The read is neither rejected, nor backstopped, nor recorded trust: fields are not frozen (K5.6), §19.3's write-point rule reaches payload bindings and not a whole-scrutinee binding over a field (v0.327's #5554 decline), and §28.3's slot-kill-by-move clause defers to the borrow lifetime rules' **domain** clause, which is about another domain.

**Decision (option (a)).** In checked disposal mode a whole-value `move` of a move-only instance field — an owning inline array or an owning choice — leaves the vacated storage in §9.4's **disposed state**, and the restoration store (K9.2) replaces it with a value visible whole. A read through a borrowing-match binding addressing that storage faults as `DisposedAccessException`, as every read of the disposed state does; unchecked mode elides the test. One store per such move, the #5554 encoding reused; K4.1's flag tier at one more site; no new axiom; no compile-time verdict moves; zero tests flip.

---

## 1. The case

### 1.1 What the text fixes

- The binding addresses the field's storage in place: §19.3 *Pattern binding and ownership* ("For a scrutinee of **move-only value type** … the binding is a **borrow** of the value"), §15.3 *Pattern binding*.
- A whole-value `move` out of a field disposes nothing, sets no flag, and leaves the field to the restoration rule: §15.3 *Local lifetime* ("A whole-value `move` out of a **field** is [K9.2]'s restoration rule unchanged"); §28.3 *Scope* ("a slot kill *by move* … disposes nothing and sets no flag, and is governed by the borrow lifetime rules' domain clause above ([K4.3]), a recorded obligation, not by this exemption").
- §9.4's disposed state is entered by "a move-only value disposed in place **with no replacement** — the nullable location holding an owning choice set to null, or the holder of either kind destroyed"; a move is neither.
- The consume-site freeze covers **locals** only (K5.5, K5.6): `case arr of var a => do var t := move arr; … a[0] … end` over a local `arr` is already rejected, the binding being a live local borrow of the consumed binding (§15.3 *Local-to-local assignment*'s scope note).

So between the move and the restoration, in the field case only, `a[i]` reads storage whose contents belong to `t`:

```bezel
class Pool
  public Slots: [4 owned]IConn?;              // interface-typed slots: a boxed record may sit in one (§15.1.1)
  public method Drain(alias: Pool)            // `alias` may be `this`
    case Slots of
      var a => do
        var t := move alias.Slots;            // the value now lives in `t`; the field is moved-from (K9.2)
        try
          Console.WriteLine(a[0] <> null);    // reads the vacated storage
        finally
          alias.Slots := t;                   // restoration, on the read's exception edge too (K9.2)
        end;
      end
    end;
  end Drain;
end Pool;
```

With `t` instead disposed in the arm (dropped at an inner scope end, or passed to a consuming call), `a[0]` reads a released box when the slot held one. The hazard is #5554's (a boxed value behind a binding, no flag of its own) reached by a `move` instead of a holder kill.

### 1.2 Why K4.3 does not close it

§28.3 *Scope* sends a slot kill by move to "the borrow lifetime rules' domain clause above ([K4.3])". That clause (the field-store bullet's clause 4) is the obligation *not to transfer the source's object to another domain*; its recorded trust clauses (field-held, collection-held, comparer-held and frame-held borrows versus domain transfer) name another domain. A same-domain move-then-dispose has no recorded clause naming it, and K4's third clause admits "recorded trust, or nothing": no admission may be made without recording its clause or its backstop. The case therefore has, at v0.327, neither.

### 1.3 Why §19.3's write-point rule does not close it

The rule's subject is "a borrowing-match binding of a payload of **move-only value type**", and "a binding of the whole scrutinee is such a binding too when the scrutinee is itself one". A binding over a *field* scrutinee is outside it, by v0.327's #5554 decision, whose decline row records why: the rule exists for the type confusion a store under a payload binding causes, which an inline array's uniformly typed slots cannot, and at a field — never "provably unwritten" (§19.3's list) — it would reject the binding across every non-pure call.

---

## 2. Options

| | Option | Soundness | Simplicity | Cost |
|---|---|---|---|---|
| **(i)** | **Status quo** — no rule | the read is undefined behaviour in every build mode, with no rejection, backstop or recorded clause: K4's third clause is violated | — | unsound |
| **(a)** | **The move writes §9.4's disposed state into the vacated field storage; the restoration store clears it** (checked mode) — **adopted** | sound: every read through the binding between the move and the restoration faults; after the restoration the binding reads the replacement, visible whole (the posture §9.4 already states for a whole-value overwrite) | one sentence at §9.4's entry list; the #5554 encoding (a sentinel no live slot holds) reused; K4.1's tier at one more site | one store per whole-value move of such a field in checked mode; a read faults at once, even while the taker lives — stricter than a class field's borrow, whose object stays readable in its taker |
| **(b)** | extend §19.3's write-point rule to a whole-scrutinee binding over a move-only field, against a whole-value `move` of the scrutinee field | sound for the direct spelling `move h.Slots`; **unsound for `move alias.Slots`**, which no body-local rule can see | a new clause in a rule #5554 just declined to extend | at a field, never provably unwritten, the rule would reject the binding across every non-pure call |
| **(c)** | a recorded K4.3 trust clause naming the same-domain case, reported by the trust-admission report | admitted by K4's third clause only "where an admission's temporal guarantee can neither be statically compared (K2.3) nor flag-backstopped"; here it can be flag-backstopped, at the cost of a store | one register row | the programmer carries an obligation a store could discharge |
| **(d)** | freeze the field against the direct spelling `move h.Slots` while the binding lives (a K5.6 exception) | sound for the direct spelling; blind to `move alias.Slots` | K5.6's first exception, a kernel amendment | the alias spelling stays open, so (a) or (c) is still needed |

**Evaluation.** *Soundness:* (a) and (c) close both spellings; (b) and (d) close one. *Simplicity:* (a) adds no rule, only an entry to a state §9.4 already defines and checked mode already tests; (c) adds a trust clause where a backstop exists, against K4's own third clause; (b) and (d) each add a static rule that cannot see an alias. *Consistency:* (a) is the posture every borrow of a field's current value already has (§28.3 *Scope*: invalidated under the runtime-checked posture) applied to the one kill by move that leaves storage a borrow can address; a class field's `move box.f` leaves the field null or moved-from, and no in-place borrow of the *field's storage* exists for a class-typed field (a borrow of its value is a borrow of the object, K4.1-backstopped). *Migration reach:* none (§5). **Decision: (a)**, with (c) recorded as the floor the critique named, and declined because the backstop is available.

**Why the fault fires while the taker still lives.** The storage, not its contents, carries the state (§9.4: "The state is the **value's**, not its contents'"). Tracking "the taker's liveness" from the field would need the field to know its taker, which no encoding of a vacated location can; and the reading that would be admitted — `a[i]` returning a borrow of an object `t` owns — is exactly the dangling shape when `t` is dropped a statement later. Reading before the move, or moving into a local and matching the local, is the stated remedy.

---

## 3. The exact normative edits (▲)

Anchors are v0.327 as composed; the coordinator re-takes them.

1. **§9.4, *Move-only receivers*.** After "— is left in a **disposed state**": ▲ ", and so is the storage an **instance field** of either kind vacates under a whole-value `move` (`var t := move h.Slots;` — [§15.3] *Local lifetime*, [K9.2]'s restoration window), from the move until the restoration store replaces the value: the value then lives in the taker, and a borrowing-match binding addressing the field's storage ([§15.3] *Pattern binding*) would otherwise read slots the taker may already have disposed; a read of the vacated field through an alias of its holder, which the moved-from state of [§28.8.1] does not track, reads the same state. (A whole-value move out of a **local** vacates nothing a surviving borrow can address: the consume-site freeze rejects it while such a binding lives, [§28.3].)" And "A whole-value overwrite is not this case: it stores a replacement, visible whole." → ▲ "A whole-value overwrite is not this case, and neither is the restoration store: each stores a replacement, visible whole."
2. **§15.1.1**, after the `move arr[i]` paragraph's last sentence: ▲ "A whole-value `move` of an owning-inline-array **field** (`var t := move h.Slots;`) takes [K9.2]'s restoration rule at the field ([§15.3] *Local lifetime*) and leaves the field's storage in [§9.4]'s disposed state until the restoration store, so that a borrowing-match binding over the field reads the state, not the taker's slots, in checked disposal mode."
3. **§15.3, *Local lifetime*.** "A whole-value `move` out of a **field** is [K9.2]'s restoration rule unchanged" → ▲ "…unchanged, and leaves the field's storage in [§9.4]'s disposed state until the restoration store: a borrowing-match binding over the field (*Pattern binding* below) that survives the move reads that state, which checked disposal mode faults, rather than the slots the taker now owns."
4. **§19.3, *Pattern binding and ownership*.** After "`move s[i]` through the binding is rejected (the binding does not own the array).": ▲ "A binding over a **field** of move-only type that survives a whole-value `move` of the field addresses storage in [§9.4]'s disposed state until the field is restored, and a read through it faults in checked disposal mode ([§15.3] *Local lifetime*)."
5. **§28.3, *Scope*.** After "…a recorded obligation, not by this exemption.": ▲ "A whole-value `move` of a **move-only** field (`move h.Slots`, [§15.3]) is the one kill by move that marks its storage: it leaves the field in [§9.4]'s disposed state until the restoration store, so that a borrowing-match binding addressing the field's storage ([§15.3] *Pattern binding*) is flag-backstopped in checked mode ([K4.1]) rather than left reading the taker's slots — a same-domain case the domain clause does not reach."
6. **Appendix C.5**, the move-only receiver row: ▲ "…a slot read through the binding among them; a move-only instance field's storage is in that state from a whole-value `move` of the field until its restoration store."
7. **Rationale §9.15**: the #5554 paragraph's last sentence ("…is the K4.3 slot-kill-by-move trust clause's, and is filed, not decided here") → ▲ "…is decided below (v0.328)", and one new paragraph recording the four options and the three declines.
8. **Code-generation reference §7.3**: one *Looks right, is rejected* pair (a borrowing `case` over an owning-inline-array field, `move h.Slots` in the arm, a read through the binding marked as the checked-mode fault, the restoration store) with the remedy.
9. **`DECLINED.md`**: three `declined-at-proposal` rows (§4).

The text names **instance** fields: a static owned field admits no `move` (K9.4), a `ref` parameter is never declared at a move-only type (§22.3, v0.322), and `move coll[i]` is admitted only on slots whose element is class-typed (§28.11 *`move` on a collection slot*: the owning collections, `OwningBuffer<T>` and `Span<T>` at a class `T`, and an owning inline array's own slots), so no collection slot holding a move-only value (§28.11's move-only element category) is vacated whole by a `move`; a whole-value overwrite of such a slot is the posture §9.4 already states, a replacement visible whole. The owning choice is covered by "either kind": its field's whole-value `move` is admitted by K9.1 and its state is the one §35.9's null store already defines; a borrowing whole-scrutinee binding over an owning-choice field (`case h.C of var c => …`) is the same shape, and a payload binding is already rejected across the move by §19.3's write-point rule (a `move` out of non-local storage is a write point).

---

## 4. Declined alternatives (one `declined-at-proposal` row each)

| Topic | Reopening bar |
|---|---|
| (b) Extending §19.3's write-point rule to a whole-scrutinee borrowing binding over a move-only field against a whole-value `move` of the field | —(none stated): a field is never provably unwritten, so the binding would be rejected across every non-pure call (the #5554 decline), and the rule cannot see a move through an alias of the holder |
| (c) A recorded K4.3 trust clause for the same-domain case | a measured checked-mode cost of the store that the report mode would rather carry as a named admission |
| (d) Freezing a move-only field against the direct spelling `move h.Slots` while the binding lives (a K5.6 exception) | —(none stated): K5.6's first exception, blind to `move alias.Slots` |

---

## 5. Migration, measured

- **Newly defined (checked mode, spec-ahead-of-impl):** a read through a borrowing-match binding over a move-only instance field, between a whole-value `move` of the field and its restoration store, raises `DisposedAccessException`; after the restoration store the binding reads the replacement. Unchecked mode: the test is elided, as for every disposed-state read.
- **Newly rejected / newly admitted:** none. No compile-time verdict moves: the `move` stays admitted (K9.1, K9.2), the binding stays admitted (§19.3), and the restoration obligation is unchanged.
- **Tests that flip:** none. Census: no test, doc example or corpus case whole-value-moves an owning-inline-array or owning-choice field inside a borrowing `case` over that field (`grep -rn 'move [a-z_]*\.[A-Za-z_]*;' tests/` → the `move box.n` / `move h.n` rejections of `move_non_owned_field_rejected_4284_test.cpp` and the class-typed `move this._res` restorations of the `move_after_move_field_897_898` lit cases, none an inline array or a choice; `tests/ownership/move_only_scrutinee_pattern_binding_borrow_1665_test.cpp` and `borrowing_match_move_only_binding_consume_4803_test.cpp` match owning inline arrays over **locals**, where the freeze already governs). The rule corpus: `s15_3_0001`–`0015`, `s15_1_1_0001`–`0007`, `s19_3_*` state no verdict this proposal changes (`s15_3_0006`/`0007` are the local-freeze cases; `s15_3_0015` is the whole-value move out of a local). No `baseline.tsv` row changes.
- **Doc-only:** the §9.4, §15.1.1, §15.3, §19.3, §28.3 and C.5 sentences; the rationale paragraph; the code-generation pair; the three decline rows.
- **Guides:** none mentions a borrowing `case` over an owning inline array field or a whole-value field move (`grep -n 'Slots\|move h\.' docs/Bezel_Guide_0*.md` → 0); no pin moves for this proposal.
- **Corpus coverage gaps, recorded not written:** the checked-mode fault (`case h.Slots of var a => do var t := move h.Slots; try _ := a[0]; finally h.Slots := t; end; end end;` — run, expect `DisposedAccessException`; the restoration sits in a `finally` because `a[0]` is an inline-array indexer access, throwable under §28.8.1, and K9.2 counts its exception edge); its control reading `a[0]` before the move (run); the restoration control reading `a[0]` after `h.Slots := t;` (run, the replacement); the owning-choice twin over `case h.C of var c => …`.

---

## 6. Kernel and body-locality

**Kernel:** K4.1's flag tier, at one more site — the kill by move of a move-only field, which v0.321/v0.327 left as the one kill of this kind with no flag — and no new axiom; §28.3 *Scope*'s sentence says so beside the slot-kill-by-move clause it qualifies. K5.6 (fields are not frozen) is untouched: the move stays unfrozen, and the backstop is what K5.6 relies on for every other borrow of a field's current value. K4.3's register gains no clause. **Body-locality:** local — a checked-mode runtime test; no compile-time rule is added or altered (proposal 0105 §1's classes do not apply).

---

## 7. Compiler facts and the tracker

- No disposed-state encoding exists for either move-only kind (proposal 0135 §13.6 item 5; v0.321's #5496 item tracks the owning choice; v0.327's #5554 tracker the inline array's holder kill). The whole-value move of a move-only value is lowered as a bitwise transfer with the source marked moved-from (`src/codegen/LlvmBackend_Disposal.cpp:6880–6900`, the #4566 note: "a whole-value move 'leaves the local holding no value rather than a hole, so no pass walks it'"); for a field source the same lowering leaves the field's bytes as they were. The restoration store is §15.3's whole-value reassignment pass (`LlvmBackend_Disposal.cpp:1630`), which at a moved-from field "disposes nothing" (§28.7 *Moved-from and null*).
- **Where the state lives: in the field's own storage, with no layout change.** An owning choice's state is a tag value no case uses. An owning inline array's state is a value written into its first slot, which every form has (`[0]T` is a compile error, §15.1): a reserved address no object occupies (a tombstone the runtime allocates once), at both forms. `null` would not serve even at the non-nullable form `[N owned]C`: a slot moved out by `move h.Slots[0]` and not yet restored may hold it, and a read of another slot through the binding would then fault on a live array. Owning-inline-array elements are class- or interface-typed (§15.1.1), so every slot is a reference-sized word and the tombstone fits. A read tests the first slot, so the move costs one store whatever `N` is, and the test is one compare.
- **Tracker to file (spec-ahead-of-impl, §9.4 / §15.3 / §28.3, v0.328):** at a whole-value `move` of an owning-inline-array or owning-choice instance field, write the #5554 sentinel (above) into the vacated field storage in checked mode; test it on every read through a borrowing-match binding addressing that storage, and on every read of the field itself (an alias of the holder reaches it, §3 item 1) — slot reads, iteration, the compiler-provided operations; for a choice, payload and tag reads — raising `DisposedAccessException`; and have the restoration store (and `Dispose()`'s per-slot pass, should the object die moved-from inside a destructor, K9.3) treat the sentinel as "no live slots". The two probes in §5 are the regression pair; each read between the move and the restoration sits in a `try` whose `finally` restores the field, since the read is throwable (§28.8.1) and a restoration after it would leave the fault's edge unrestored (K9.2). Companion to #5554's encoding item; one encoding serves both.

---

## 8. Self-critique (adversarial pass)

1. *Does the state outlive the restoration?* No: the restoration store is a whole-value store into a moved-from location (§28.7: stores without disposal), and the text says it clears the state ("each stores a replacement, visible whole"). The binding then reads the replacement — the posture §9.4 already gives a whole-value overwrite through an alias.
2. *Does a `move` out of a field inside a `case move h.C of …` owning match reach this?* The owning match consumes the field at arm selection and the construct's bindings are transferred locals (§35.5), not in-place borrows; no binding addresses the field. Outside the rule.
3. *A `move` of the field inside a method called from the arm (`a` live across the call).* The callee's `move this.Slots` is the alias spelling; the state covers it.
4. *A destructor body moving the field.* K9.3 exempts a destructor from restoration; the object dies with the field moved-from, and any binding over it is in the destructor body's own `case`, so the fault is possible in checked mode until teardown. Sound; the tracker says the per-slot pass treats the sentinel as empty.
5. *`move h.Slots[i]` (a slot move) is unaffected:* a slot move leaves a slot moved-from, not the array, and §15.1.1's slot-restoration rule is unchanged; a read of that slot through the binding reads a moved-from slot — the same read a direct `h.Slots[i]` makes, and §15.3's "reading from a moved-from location is a compile error" is a static rule on named locations, not on a binding's slot read. This proposal does not touch it; it is at most a question for #5554's tracker (a null-like sentinel per slot), not a new one, since the slot's object is K4.1-backstopped when the taker disposes it.
6. *The cost claim.* One store per whole-value move of such a field in checked mode, nothing in unchecked mode — measured against zero sites in `tests/` and `docs/`.
7. *History narration (process §7):* the normative sentences state the current rule; "the one kill by move that marks its storage" states a fact of the current rule set, not a prior state. The options and the "filed, not decided" story live here and in the rationale paragraph.
8. *A read of the field itself through an alias* (critic round): `var t := move alias.Slots;` with `alias` the receiver, then `Slots[0]` — the moved-from state is tracked per spelled path (§28.8.1), so the second spelling is admitted and reads the storage `t` took, the same hazard without a binding. The state covers it once the field's own reads test it; §3 item 1 says so and §7 adds the test site. The whole-value store that restores through the other spelling (`Slots := t`) is a drop-on-overwrite of a field the checker believes owned; with the sentinel present its per-slot pass finds no live slots (§7), where the stale bytes would otherwise be disposed a second time. In unchecked mode no sentinel is written, and that overwrite is the pre-existing alias hazard of a field move, outside this proposal.
9. *Composition with v0.327 (dI, #5554):* the §9.4 sentence is placed inside the entry list that #5554 widened and reads "either kind" as it does; the rationale paragraph replaces exactly the sentence #5554 left pointing at a filing. No other v0.327 or v0.326 lane touches §15.3 *Local lifetime*, §19.3's binding paragraph or §28.3 *Scope*.
