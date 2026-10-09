# Proposal 0166 — Move-only substitutions: record components, borrowed-state copies, enumeration elements and event payloads

| | |
|---|---|
| **Status** | graduated-v0.334 — superseded — graduated v0.334 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues" and the 2026-10-09 direction "resolve all findings autonomously": options R1, B1, E1 and S1 — a move-only substitution has no copy: a generic record's component at one, a copy out of a binding held in the borrowed state at one, the four borrowing enumeration interfaces and the two event sources at one are rejected at the instantiation site; critique round 1 applied, shared with the lane's five rulings, which corrected B1's operand classes (an unmarked field read lends), the decline labels and the needle flip of one rejection test; landed with the v0.334 changelog entry) |
| **Tracking issue** | [#5772](https://github.com/s8lap/bezel-lang/issues/5772) (RC34, a generic record's component at a move-only substitution), [#5769](https://github.com/s8lap/bezel-lang/issues/5769) (RC31, a copy from an `as` result or pattern binding at a move-only substitution), [#5771](https://github.com/s8lap/bezel-lang/issues/5771) (RC33, a move-only element type at the iteration interfaces), [#5770](https://github.com/s8lap/bezel-lang/issues/5770) (RC32, a move-only event payload — the recorded deferral); one `spec-tracking` compiler tracker is filed at landing (§7) |
| **Target module(s)** | **spec**: §7.10 (a new paragraph before *`?` is idempotent under substitution*), §9.7 (one clause in the first paragraph), §14.1 (*Instantiation and its site*, the list of rules that fire at every site), §15.3 (*Field placement*, the record sentence), §28.11 (one paragraph after the element-category table), §29.2 (one sentence in the value-typed yield bullet), §29.13 (one parenthetical), Appendix C.1 (one bullet). **stdlib**: §4.12 (one sentence), §6.5 (the move-only interaction bullet becomes the rule; the open-question paragraph). **rationale**: §10 (one dated paragraph, shared with the lane's rulings). **code-generation reference**: §3.3 (two table rows), §7.5 (one *Looks right, is rejected* pair). **`DECLINED.md`**: four `declined-at-proposal` rows; the `deferred` event-payload row is annotated (the move-only leg is taken). |
| **Written against** | spec/stdlib **v0.333** at `429fd72` (main). Section numbers are v0.333's. |
| **Origin** | The generics matrix's first run (`docs/research/generics-matrix/runs/v0.333.json`), cells R5/C1 (L1 G3), R5/C6 and R7/C6 (L4 G3), R5/C13 and the route R5/C13·event (L5 G2, L5), consolidated as root causes RC34, RC31, RC33 and RC32 of `scratch/matrix2/ROOTS.md`. A proposal is needed because the decision adds instantiation-site rejections to standard-library types the text says are "rejected at an instantiation only by [their] declared constraints and the instantiation-site rules stated for [them]" (§3.6 rule 2), and because it takes a leg of a recorded deferral (`DECLINED.md`, *Restricting `EventSource<T>`/`AsyncEventSource<T>` payloads*). |

*Reference convention: unprefixed §-references are language-spec references at v0.333; "stdlib §" references are to the standard library companion. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

A generic body is checked once, under §28.1 clause 3's class-or-interface assumption, and §15.3 makes a move-only value — an owning inline array or an owning choice — a value no operation may copy. Four constructs copy a `T`-typed value without any rule saying what happens when `T` is substituted by a move-only type:

1. **A record component** (`record Box<T>(V: T)`): §15.3's "a move-only value may not be a field of a record" is stated for written types, and no instantiation-site rule carries it to a substitution, so `Box<[1 owned]Conn?>` is admitted and a record copy duplicates the owned slot (R5/C1).
2. **A copy out of a borrowed binding** (`var r := v as T; _f := r;`): §17.4 makes every `as` result a borrow and §7.10's copy demand is stated inside the `where T: value` paragraph; at `T = Slot` the store either rejects (copy semantics required) or moves out of a borrow while the parameter still owns the payload (R5/C6).
3. **The borrowing enumeration interfaces** (`IEnumerable<Slot>`, a generic iterator at `T = Slot`): §29.2 calls a value-typed yield "a copy" and has the enumerator "not dispose anything"; `IEnumerator<T>.Current` becomes a property of move-only type, which §23.5 admits for no declared type (R5/C13).
4. **The event sources** at a move-only payload: stdlib §6.5 states the by-value-move interaction and records the restriction as deferred until "a rule that actually states it" exists (R5/C13·event).

**Decision.** One mechanism, stated at four sites. (R1) §15.3's record ban reads a component's declared type after substitution: a component whose type is or contains a type parameter is rejected at every instantiation at which that type is move-only, at the instantiation site through any chain (§15.1's pattern). (B1) §7.10's copy demand is read at every move-only substitution of any parameter whose constraints admit one; an operation written as a copy is a move where its operand is a named owned binding the body may consume, and is rejected at the instantiation where its operand is a binding held in the borrowed state — an `as` result, a pattern binding, a capture, a `ref` referent. (E1) `IEnumerable<T>`, `IEnumerator<T>`, `IAsyncEnumerable<T>` and `IAsyncEnumerator<T>` admit no move-only value type as `T`, an instantiation-site rule stated beside §28.11's move-only row, every naming a site, read through §3.6 rule 2's chain for a generic iterator or a generic type forming one. (S1) The same rule at `EventSource<T>` and `AsyncEventSource<T>`; `IEvent<T>`/`IAsyncEvent<T>` carry no rule, a user implementation's raise being checked where it reads the payload.

No new syntax, no new keyword, no new mechanism: §3.6 rule 2's per-instantiation reading of written-type bans, which §15.1 already uses for an inline array's element, applied to the record component; the copy demand that already exists, with its operand class named; and the stated-instantiation-site-rule form §14.4 and §28.11 already use for the collections. Newly admitted: nothing. Newly rejected in `tests/`, `docs/` and the corpora: zero sites; one existing rejection test (`borrowing_match_move_only_binding_consume_4803`'s `Iter(): IEnumerable<Outer>`) is rejected first by E1, so its ownership-pass needle flips (§3, §7).

---

## 1. The problem

The four programs, each at spec v0.333 (`Conn` a class with a destructor, `Slot` the owning choice `Full(owned c: Conn), Empty`):

```bezel
record Box<T>(V: T);
static class Use
  public static method Run()
    var a: [1 owned]Conn? := [new Conn()];
    var b := new Box<[1 owned]Conn?>(a);   // no rule rejects the instantiation
    var c := b;                           // a record copies: b.V and c.V own one Conn
  end Run;
end Use;
```

```bezel
class Hold<T>
  private _f: T?;
  public method Take(v: T)
    var r := v as T;   // §17.4: a borrow of v, whatever v's state
    _f := r;           // class assumption: a borrow stored; at T = Slot, a copy or a move out of a borrow?
  end Take;            // v is disposed here at T = Slot — twice, under the move reading
end Hold;
```

```bezel
static class Gen
  public static method Once<T>(owned x: T): IEnumerable<T>
    yield return x;   // class S: a frame-owned re-borrow; at T = Slot the unmarked read moves (§28.10, §15.3)
  end Once;
end Gen;
// using var e := Gen.Once<Slot>(Slot.Full(new Conn())).GetEnumerator();
// var a := e.Current; var b := e.Current;   // Current: a property of move-only type; §29.2: "a copy"
```

```bezel
class Feed<T>
  private owned readonly _ev: EventSource<T> := new();
  public method Fire(v: T) _ev.Raise(v); end Fire;   // at T = Slot: by-value-move to each subscriber (stdlib §6.5)
end Feed;
```

What the text says at each:

- §15.3 *Field placement*: "A move-only value may not be a field of a record (records support only copy semantics, not move semantics)." §9.7: "Records also cannot have inline-array fields whose slots carry the `owned` marker." Both read written field types. §3.6 rule 2 names "an instantiation-site rule's such as [§14.4]'s, [§15.1.1]'s or [§28.5]'s" and §14.1 lists the rules that fire at every site; the record ban is in neither list, where §15.1 carries its element rule explicitly ("so is a type parameter `T` substituted by one, at the instantiation site").
- §7.10, inside the `where T: value` paragraph: "any operation the generic body uses that requires copy semantics (e.g., reading T twice without an intervening assignment) is rejected at instantiation with a message naming the move-only type." §17.4: `as` is "a *borrow* of `x` … regardless of whether `x` is owned or borrowed". §28.3's producers row: "The cast does not move from `x`".
- §29.2: "The yield is a copy (for value types) … The enumerator does not dispose anything for these yield types"; §29.1: "Multiple reads between `MoveNext` calls produce additional borrows of the same value"; §23.5: "Properties and indexers whose declared type is a move-only value type … are not admitted". §28.11 and §14.4 reject move-only elements for the collections and the buffer family only, and §3.6 rule 2 says a stdlib type "is rejected at an instantiation only by its declared constraints and the instantiation-site rules stated for it".
- stdlib §6.5: "A move-only payload is passed by value-move (§15.3), leaving the argument moved-from after the call, which does not compose with more than one subscriber", listed as an interaction, not a rule; `DECLINED.md`'s deferred row: "the move-only leg and the synchronous source stay deferred", reopenable by "a ruling that derives the restriction from a rule that actually states it".

### 1.1 What the compiler does today

Source-read at `429fd72`; no build exists in this checkout, so nothing was probed. The record ban is declaration-time only (`TypeResolution_Checks.cpp:2405–2440` reads `fieldDeclTypeOf` for `[N owned]` and owning-choice components); no per-instantiation record-component check was found. No diagnostic text matching §7.10's copy demand was found in `src/sema`. No move-only exclusion at the enumeration interfaces or the event sources was found; `Channel<T>`'s element check (`Sendability.cpp`) rejects move-only types through `sendable` (`send_600_value_types__move_only_value_type_as_channel_element_rejected_30_14_15_3.bzl`), which `IEnumerable<T>` has no analogue of.

---

## 2. Options

**R — the record component.**
- **R0** Status quo: no rule at a substitution; `Box<[1 owned]Conn?>` admitted and copyable.
- **R1** The ban reads the declared type after substitution: rejected at the instantiation site through any chain (§15.1's pattern).
- **R2** A generic record becomes move-only at a move-only substitution (a per-instantiation copyability).

**B — the borrowed-state copy.**
- **B0** Status quo: §7.10's sentence stated under `where T: value` only, with the reading undecided.
- **B1** The copy demand is read at every move-only substitution whatever the constraints; a copy out of a binding held in the borrowed state is rejected at the instantiation (reading 1).
- **B2** The copy out of a borrow is a move (reading 2).

**E — the enumeration interfaces.**
- **E0** Status quo: no rule; `IEnumerator<Slot>.Current` undefined.
- **E1** The move-only exclusion is an instantiation-site rule of the four borrowing enumeration interfaces, every naming a site, read through the chain.
- **E2** A borrowing `Current` over a frame-owned move-only slot, the frame disposing the pending value; a third yield regime.

**S — the event sources.**
- **S0** Status quo: the interaction stated, the deferral standing.
- **S1** The same instantiation-site rule at `EventSource<T>` and `AsyncEventSource<T>`; `IEvent<T>`/`IAsyncEvent<T>` unrestricted.
- **S2** The rule at the interfaces too.

---

## 3. Evaluation

**Soundness.** R0, B0/B2, E0 and S0 each leave a program that compiles under the single check and duplicates or double-disposes an owned payload (§1's four programs; none is flag-backstopped — a record copy and a value-typed yield set no disposed flag). R1, B1, E1 and S1 close each at a stated site. R2 closes R but opens every record-copying rule to a per-instantiation reading (§9.4's value receivers, `with`, equality, serialization) for a kind §15.3 says the language does not provide. E2 is sound but adds a regime: a borrowing `Current` over a move-only slot still types `Current` at a move-only type, which §23.5 forbids for a reason that holds here (a getter never transfers out, and a borrowing read of a frame-owned move-only value is exactly §19.3's borrowing-match binding, with its write-point rule to carry).

**Simplicity.** R1 is one clause in §15.3 and one in §9.7, in the words §15.1 already uses. B1 is one paragraph that states where a demand already in the text is read and names its operand class; it also closes L1's editorial finding that the demand sits inside the `value` paragraph (RC11, lane W5's, whose restatement composes with this paragraph by referring to it). E1 and S1 are one paragraph beside §28.11's table plus one sentence at each stdlib declaration, the shape §14.4 and §28.11 already take for the collections and the buffers. E2 would need a frame-slot semantics in §29.1/§29.2 and an exception to §23.5; S2 would restrict an interface whose implementations are user code checked by ordinary rules (stdlib §6.5's recorded reason for declining the holds-borrows leg there: "a user implementation's own raise is checked where it retains").

**Consistency.** R1 is §15.1's element rule at the record component; §3.6 rule 2 names that pattern. B1 is what §15.3's *Pattern binding* bullet already says of a borrowing-match binding ("consuming operations through the binding … are rejected") read at a generic body's `as` result and capture. E1 and S1 follow §28.11's move-only row, which says of the collections what the issue asks of the interfaces, and the owning interfaces are class-only already (stdlib §4.13). S1 meets the deferral's own exit criterion: the restriction derives from a rule that states it (E1's, at a sibling position), and the interaction stdlib §6.5 records becomes the rule's ground.

**Migration reach** (measured over `tests/` gtest literals, lit, the conformance corpora and `docs/`; `scratch/v0334/w6work/`):
- R1: generic records named at `[N owned]…` or at an owning choice — **0** (`\w+<\[\d+ owned\]` hits are `OwningBuffer<[5 owned]R>`, `Channel<[2 owned]Node>` and dispatched-method cases, each already rejected).
- B1: a generic body with `as T` or a typed pattern instantiated at an owning choice — **0** (the one file with both, `static_receiver_generic_instantiation_4582_test.cpp`, casts an `Object` at `T = ()`).
- E1: `IEnumerable`/`IEnumerator`/async forms at an owning choice or owning inline array — **1 program**, `tests/ownership/borrowing_match_move_only_binding_consume_4803_test.cpp` (`Iter(): IEnumerable<Outer>`), an existing rejection test (its needle, `cannot move the \`case\` arm binding`, is the ownership pass's: a yield consuming a borrowed binding). Under E1 the iterator's declared return type `IEnumerable<Outer>` is rejected first, and since no admitted program can yield a move-only value any more, the shape the test guards has no host left: the test **flips its needle** to E1's diagnostic (a behavior flip the implementing tracker names), it does not merely gain a ground. *(critic correction)* Docs and corpora: 0.
- S1: `EventSource<…>`/`AsyncEventSource<…>` at a move-only type — **0**.

---

## 4. Decision

**R1 + B1 + E1 + S1.** A move-only substitution has no copy, and every position that copies rejects it at the instantiation: the record component through §15.3's ban read after substitution; the generic body's copy out of a borrowed binding through §7.10's copy demand, read whatever the constraints; the four borrowing enumeration interfaces and the two event-source classes through an instantiation-site rule stated at §28.11 and at their declarations.

Declined with reasons: R2, B2, E2 and S0 (§9); S2 stays declined on proposal 0154's row (`IEvent<T>`'s implementations are checked where they read).

---

## 5. The exact normative edits (▲)

The edit set is `scratch/v0334/W6.json`, ids `W6-0166-*`; each `old` applies once at `429fd72`. In brief:

- ▲ **§15.3** *Field placement*, after "records support only copy semantics, not move semantics": "the ban reads a component's declared type after substitution (§3.6 rule 2): a record component, positional or body, whose declared type is or contains a type parameter is rejected at every instantiation at which that type is a move-only value type, at the instantiation site through any chain of user declarations (§14.1 *Instantiation and its site*; the diagnostic names the component and the substituted type), so `record Box<T>(V: T)` is rejected wherever `Box<[2 owned]Conn?>` or `Box<Slot>`, for an owning choice `Slot`, is named."
- ▲ **§9.7** first paragraph: one clause pointing at the rule.
- ▲ **§7.10**, a new paragraph *Move-only substitutions and the copy demand* directly after *The copy demand.* (#5753's paragraph, which states the demand's scope; composed at v0.334 — this paragraph then opens "At a move-only substitution …" and does not restate the scope): the demand is read at every move-only substitution of any parameter whose constraints admit one; a copy is a move where its operand is a named owned binding the body may consume, and is rejected at the instantiation where its operand is a binding held in the borrowed state (an `as` result, a borrowing-match or typed-pattern binding, a capture, a `ref` referent, or a local bound to one); the diagnostic names the binding, the substitution and the operation; the `Hold<Slot>` example.
- ▲ **§28.11**, after the element-category table's note: the move-only row is also an instantiation-site rule of the borrowing enumeration interfaces and the event sources; the grounds (§23.5, §29.2, by-value-move); every naming a site; the chain reading; the owning forms class-only already; a user `IEvent<Slot>` implementation checked where it reads.
- ▲ **§29.2** value-typed yield bullet: "A **move-only** value type is not among these cases: it is admitted as the element type of no enumeration interface (§28.11), so an iterator declared over one, or over a type parameter at a move-only substitution, is rejected at the instantiation site." ▲ **§29.13**: the parenthetical names the async iterator's element type.
- ▲ **§14.1** *Instantiation and its site*: the list of rules that fire at every site gains §15.3's record-component rule and §28.11's enumeration/event rule.
- ▲ **Appendix C.1**: one bullet after the inline-array element bullet.
- ▲ **stdlib §4.12**: one sentence after "`IEnumerable<T>` is covariant in T." ▲ **stdlib §6.5**: the interaction bullet becomes the rule ("A move-only payload is not admitted at the source classes"), and the open-question paragraph names the first interaction only.
- ▲ **rationale §10**: one dated paragraph (shared with the lane's rulings). ▲ **code-generation reference** §3.3: two rows; §7.5: one pair.

---

## 6. Measurement

Recipes in `scratch/v0334/w6work/` (a corpus walk over gtest C++ with its string literals unescaped, lit, the doc-extracted and rule corpora, and `docs/`). Owning choices are found per file by a payload marked `owned`; the patterns are `I(?:Async)?Enumerable<Name?>`, `I(?:Async)?Enumerator<Name?>`, `(?:Async)?EventSource<Name?>`, `\w+<\[\d+ owned\]`, and `as T` beside an owning-choice instantiation. Results: §3. The rule corpus has no case at a move-only element or payload and no generic record at a move-only argument; `s15_3_0002` (`record Wrapper(Items: [2 owned]…)`, written type) and `s35_4_0009` keep their `reject` verdicts and gain nothing; `s15_3_0003` (`accept`: a move-only value carrying no borrows may be a class field) is unaffected.

---

## 7. Compiler facts and the tracker

- Record component: declaration-time check only (`src/sema/TypeResolution_Checks.cpp:2405–2440`, `fieldDeclTypeOf`); no per-instantiation arm. **Spec-ahead-of-impl.**
- Copy demand: no diagnostic text matching §7.10's demand found in `src/sema`; the `as` result's borrow state is tracked (`src/sema/ExprTypecheck_Exprs.cpp`, `moveThroughBorrowedMoveOnlyArrayBindingName` rejects `move s[i]` through a borrowed binding, the array half of B1's shape). **Spec-ahead-of-impl, probe first.**
- Enumeration interfaces and event sources: no move-only exclusion found; `Channel<T>` rejects through `sendable` (`src/sema/Sendability.cpp`). **Spec-ahead-of-impl.**
- One `spec-tracking` tracker at landing: (i) the record-component rejection at the instantiation site through the chain; (ii) the copy demand at every move-only substitution with the borrowed-operand rejection; (iii) the four interfaces and two source classes at a move-only `T`, the iterator return type and the `for` source included; (iv) `borrowing_match_move_only_binding_consume_4803`'s `spec_17_14_yield_return_consumes_borrowed_binding_rejected` (`Iter(): IEnumerable<Outer>`), whose needle flips to E1's diagnostic (the shape it guards no longer has an admitted host).

---

## 8. Corpus cases to write (after landing)

`s15_3` — a generic record at an owning choice rejected at the naming (`reject`, lib); `s7_10` — `Hold<Slot>` rejected at the store, and `_f := v;` admitted (`reject`, `accept`); `s28_11` or `l4_12` — `IEnumerable<Slot>` as an iterator return type rejected; a generic iterator at `T = Slot` rejected at the call; `l6_5` — `EventSource<Slot>` rejected. Not written under landing pressure (process §5 item 3c).

---

## 9. Declined alternatives (one `declined-at-proposal` row each)

- **E2** — a borrowing `Current` over a frame-owned move-only slot with frame disposal. Adds a yield regime for a kind the owning forms already serve through a holder class, and leaves `IEnumerator<T>.Current` a property of move-only type. Reopened by a measured iterator over a move-only element that a holder class cannot express.
- **R2** — a generic record becoming move-only at a move-only substitution. §15.3 provides no user-defined move-only kind; every record-copying rule would turn per-instantiation. Reopened with a user-defined move-only value-type design.
- **B2** — the copy out of a borrowed binding read as a move. The owner still disposes the value. Reopening bar: none stated.
- **S0** — leaving the move-only leg deferred. The deferral's exit criterion is met by E1's rule at a sibling position; the matrix reached the leg again from a generic body. The `IEvent<T>` row (0154 option E) stays.

---

## 10. Self-critique (adversarial pass)

- *Does E1 over-reject a borrow-bearing inline array `[N]Pair` or a value-like choice?* No: both are copyable (§15.3), outside the move-only row; only owning inline arrays and owning choices (and `C?` over one) are excluded.
- *Does B1 reject the parameter move `_f := v;`?* No: `v` is a named owned binding the body may consume (a by-value-move parameter); the paragraph says so and the `Hold<T>` example admits it.
- *Does B1 reach `where T: class` bodies?* No: the demand is read only at move-only substitutions the constraints admit; a class bound admits none.
- *Does E1 break `for var x in coll` over an `OwningArray<Holder>` or `Drain()`?* No: class elements, borrowing or owning path, unchanged.
- *Does S1 reopen a `rejected` row?* No: the row is `deferred`, with the exit criterion this proposal meets; 0154's option E (`IEvent<T>`) stays declined and is not touched.
- *Composition with W1 (R5/a' touches §9.7's interface paragraph and stdlib §6.5's intro sentence):* this proposal's §9.7 edit is the first paragraph (line 1442), and its §6.5 edits are the move-only bullet and the "Whether either" paragraph — neither is a W1 anchor (checked against `W1a*.json`, `W1b.json`). *With W5 (RC11, the §7.10 copy-demand sentence's placement):* the new paragraph is inserted before *`?` is idempotent* and refers to "the copy demand of the `value` paragraph above"; if W5 moves that sentence, the reference's words change, not the rule — flagged for the composition step. *(Composed at v0.334: W5's #5753 paragraph *The copy demand.* states the scope after the `value` paragraph; this paragraph follows it directly and opens "At a move-only substitution …".)*
- *The 4803 test:* a rejection test whose program is now rejected by E1 before the ownership pass reaches its needle, and whose guarded shape (a yield of a move-only value) no longer exists anywhere; recorded in §3 and §7 as a needle flip, not hidden. *(critic correction)*
- *B1's operand classes (critic correction):* the draft listed "a field read under §28.8.1's restoration rule" among the operands a copy may move. An unmarked field read lends (§28.3 *Atoms*: a storage read is a borrow whose source is the holder), and a body checked once never wrote the restoration store, so reading it as a move would vacate the field with no restoration; the edit now lists a `move` out of a field as the consumable form and puts the unmarked field, property, indexer and slot read among the borrowed operands, rejected at the instantiation.
- *Composition with W5's RC11 (`W5-5753-7.10-copy-demand`):* W5 adds a paragraph **The copy demand.** right after the `value` paragraph, stating the demand's scope (every type parameter whose constraints admit a move-only substitution, read under §3.6 rule 2, decided at the body for a dispatched method). This proposal's §7.10 paragraph states the same scope and adds the operand classes; the two agree. At composition the paragraph moves to directly follow W5's, and its first sentence may shorten to "At a move-only substitution …, an operation the body writes as a copy …" without changing the rule.
