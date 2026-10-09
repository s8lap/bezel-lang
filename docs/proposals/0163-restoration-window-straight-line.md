# Proposal 0163 — The restoration window is straight-line, per-slot, and vacated

| | |
|---|---|
| **Status** | graduated-v0.334 — superseded — graduated v0.334 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues" and the 2026-10-09 direction "resolve all findings autonomously": options W2, V1, S1, Y1 and C2 — the K9.2 restoration window admits no throwable expression (the v0.332 `finally`-placed restoration and the `catch`-arm reading withdrawn), except at a slot of a heap collection held by an owned local of the body whose disposal every exit edge passes; every vacated location is in §9.4's disposed state; a slot `move` takes a constant or unreassigned-local index and is restored at it; `yield return` is an exit of the window; the owned-field cycle is recorded; critique round 1 applied, 10 findings resolved, among them the disposal-restored collection-slot exception (without which most disposal-restored slot moves were rejected) and the unmeasured `catch`-arm row; its form-(iv) clauses were dropped at composition with proposal 0162's retirement of the form; landed with the v0.334 changelog entry) |
| **Tracking issue** | [#5775](https://github.com/s8lap/bezel-lang/issues/5775) (RC37, the vacated location inside the window), [#5773](https://github.com/s8lap/bezel-lang/issues/5773) (RC35, "`coll[j] := value` for any `j`"), [#5776](https://github.com/s8lap/bezel-lang/issues/5776) (RC38, `yield return` in a window), [#5774](https://github.com/s8lap/bezel-lang/issues/5774) (RC36, the owned-field cycle); [#5667](https://github.com/s8lap/bezel-lang/issues/5667) is answered "no longer admitted"; one `spec-tracking` compiler tracker is filed at landing (§8) |
| **Target module(s)** | **spec**: §28.3 (K1 item 4 untouched; K5 clause 5 untouched; K9.2 and K9's closing sentence; *The consume-site freeze*'s *Scope* paragraph), §28.8.1 (the exit list; *Exception paths are not exempt*: the `finally` paragraph deleted, the inventory's `await`/`yield return` sentence, the disposed-state paragraph; a new paragraph *The vacated location*), §28.11 (*`move` on a collection slot*: the nullable-restoration sentence, the conservative-state paragraph and its two bullets replaced, the indexer-setter paragraph, the exception-safety sentence), §28.13 (the cycles bullet), §15.3 (*Local lifetime*), §15.1.1 (the slot-restoration sentence), §22.12 form (iv), Appendix C.3 (the `move coll[i]` row; one new acyclicity bullet). **stdlib**: §4.5 (`PopAt`'s sketch binds its index). **rationale**: §5.10 (one dated paragraph). **code-generation reference**: §4.2's restoration paragraph (one sentence corrected), §7.3's §9.4 pair rewritten, two §7.2 pairs added. **`DECLINED.md`**: five `declined-at-proposal` rows |
| **Written against** | spec/stdlib **v0.333** at `429fd72` (main). Section numbers are v0.333's. |
| **Origin** | The disposal matrix's first run (`docs/research/disposal-matrix/runs/v0.333.json`), root causes RC35–RC38 of `scratch/matrix2/ROOTS.md`, filed as #5773–#5776; the simplification review `scratch/matrix2/SIMPLIFY.md` candidates C6 (rank 1) and C7 (rank 5), which the owner approved in direction on 2026-10-09 ("Simplifications look good so far"; "narrow a feature when its measured reach is negligible, rather than add another rule"). A proposal is needed because the decision withdraws two stated admissions: v0.332's `finally`-placed restoration and §28.11's non-constant-index slot move. |

*Reference convention: unprefixed §-references are language-spec references at v0.333; "stdlib §" references are to the standard library companion; K tags are §28.3's kernel ids. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

K9.2 lets a method take an owned field's or slot's value out (`var t := move _r;`) if every path to the method's exit restores the location. Four admitted shapes put code **inside** that window that can reach the vacated location through an alias — a call under a `finally` that restores (v0.332's admission), an `await`, a `yield return`, a constructor body run by the restoring `new` — and nothing says what such code observes or what happens to a value it stores there (#5775). Separately, after `move coll[e]` at an index the compiler cannot track, §28.11 counts a store at *any* index as the restoration, so the moved slot can reach the exit as a hole (#5773); an iterator may suspend inside a window and be abandoned there (#5776); and `move h.f` stored into an owned field of the moved object forms a cycle no cascade reaches (#5774).

**Decision.** Narrow the window rather than add machinery to it, and give the one residual a run-time posture the specification already has:

1. **W2 — the window is straight-line.** Between a `move` of a field or slot and its restoring store, no expression on any path is throwable under §28.8.1's inventory (a `new` expression is not; its arguments classify on their own); a method call, an indexer access, an `await`, a `yield return` or any other throwable expression there is a compile error at that expression naming the move. The v0.332 sentence admitting a restoration "placed in a `finally`" is deleted, and a restoration in a `catch` arm (which K9.2's path reading admitted and the compiler implements, #3241 R2) covers nothing either. One window keeps today's reading: a slot of a heap owning collection held by an owned local of the body and disposed on every path, whose disposal every exit edge passes (§9, critic amendment). This is K9.2 as it was before v0.332, with `yield return` named among the exits (Y1).
2. **S1 — per-slot or not at all.** `move coll[e]` is admitted only at a compile-time constant or at a local the body does not reassign between the move and the restoring store; the restoration is a store at that same index, `Clear()`, `Dispose()` — the scope-end disposal of a `using var` holding the collection included — or, for an owning inline array, a whole-value reassignment. The conservative "entire collection" state and its "for any `j`" bullets go. The `null`-restoration sentence is corrected to the receivers whose indexer admits `null`.
3. **V1 — every vacated location is in §9.4's disposed state** from the move to the restoring store: a read of it through any spelling other than the body's tracked one faults as `DisposedAccessException` in checked disposal mode, a store into it other than the restoring store faults before storing (its RHS the abandoned-store temporary of §28.7), unchecked mode elides both tests (K4.1). Under W2 the only code that can reach the location inside the window is a constructor body run by a `new`, and an alias read spelled as a field read; V1 is the posture §9.4 already states for a move-only field, extended to a class-typed field, an owning-collection slot, an owning-inline-array slot and a §22.12 form-(iv) static slot.
4. **C2 — the cycle is recorded.** §28.13's cycle note names plain owned fields beside `Shared<T>`, and Appendix C.3 states the acyclicity assumption the exactly-once guarantee rests on. The issue's own program is rejected by W2 (the store into `a._next` is a possibly-live drop-on-overwrite inside the window); its rewritten form is the recorded residual.

No new keyword, no new analysis: the compiler's exception-edge classifier already rejects a throwable in the window, and the `finally` dominance case it never implemented (#5667) is withdrawn. **Text removed:** about 22 lines (the `finally` paragraph, the disposed-state paragraph's `finally` sentence, §28.11's conservative paragraph and two bullets, K9's closing clause). **Newly rejected, measured:** two rule-corpus cases (`s9_4_0010`, `s15_1_1_0009`, each a call under a restoring `finally`), two gtest rows (`dict_string_key_leak_3703`'s built-key moves), one stdlib sketch and its extracted doc-case (`stdlib_0051`, a `skip:signature-listing` row), the specification's and the code-generation reference's own `finally` examples. Zero guide sites. **Newly admitted:** nothing.

---

## 1. The problem, in the text

### 1.1 The window admits code that can reach the vacated location (#5775)

§28.8.1 *Exception paths are not exempt* (v0.332): "A restoration placed in a `finally` satisfies the rule as well, when every throwable expression between the move and the store lies in that `try`'s protected block … the shape for a read that must follow the move (`var t := move h.Slots; try … a[0] … finally h.Slots := t; end;`, §9.4 *Move-only receivers*)." So any call may sit in the window. A callee reaches the holder through `this`, a parameter or a static:

```bezel
class Res
  public readonly property N: int;
  public constructor(n: int)  N := n;  end constructor;
  destructor  Console.WriteLine($"closed {N}");  end destructor;
end Res;
class Holder
  private owned _r: Res := new Res(1);
  public method Poke()
    _r := new Res(2);           // drop-on-overwrite of whatever _r holds now
  end Poke;
  public method Cycle()
    var t := move _r;           // window opens; "disposes nothing and sets no flag" (§28.3 Scope)
    try
      Poke();                   // re-entrant store into the vacated field
    finally
      _r := t;                  // "a moved-from or null location therefore stores without disposal" (§28.7)
    end;
  end Cycle;                    // Res(2) is never disposed
end Holder;
```

§28.3 *Scope*: "a slot kill *by move* … disposes nothing and sets no flag", and §9.4 gives a vacated state only to "the storage an **instance field** of either [move-only] kind vacates under a whole-value `move`", conceding "a read of the vacated field through an alias of its holder, which the moved-from state of §28.8.1 does not track". A class-typed field, an owning-collection slot, an owning-inline-array slot and a form-(iv) static slot have no state. §28.7's liveness test ("non-null and not moved-from") does not say whether "not moved-from" is the static per-spelling fact or a run-time one. The same window opens across an `await` (every other continuation of the domain runs there) and inside a constructor body the restoring `new` runs. For statics, K9.4's own ground — "static state is observable across method boundaries, so no per-method window can cover it" — applies to every call in the window, yet §22.12 form (iv) admits `move arr[j]` on a static owning inline array.

### 1.2 "`coll[j] := value` for any `j`" (#5773)

§28.11: "The compiler tracks moved-from state per slot when the index is a compile-time constant or a local read. For variable indices … the *entire collection* is treated as having one or more slots in moved-from state. The admissible restoration operations … `coll[j] := value` for any `j` (treated as restoration of one slot if `j` matches the moved index, or as unrelated reassignment otherwise)". In the conservative state the compiler cannot decide "matches", so a store at an unrelated index discharges the obligation and the moved slot reaches the exit:

```bezel
public static method Pick(): int => 1;
public static method Take(xs: OwningArray<Res>): owned Res
  var t := move xs[Pick()];  // non-constant: the whole collection is 'holed'
  xs[0] := new Res(9);       // 'coll[j] := value for any j': counts as the restoration
  return t;                  // Pick() = 1: slot 1 is still moved-from here
end Take;
```

At `[N owned]T`, §15.3: "auto-dispose on a moved-from slot is not defined"; at `OwningArray<T>` a later `xs[1]` reads a non-nullable slot holding nothing. The loop form `while k < n do var t := move pool[k]; k := k + 1; pool[k] := t; end;` leaves slot 0 a hole the same way. Two further defects in the same paragraphs: §28.11 says an `OwningArray<T>` slot is restored by "assigning `null`", but stdlib §4.5's indexer is `[index: int]: T` with `set(owned)` taking an owned `T`, so `xs[i] := null` does not type at a non-nullable `T` (`OwningDictionary<K, V>`'s indexer is likewise `V`); and whether the restoring store's own drop-on-overwrite edge is an exit of the window is unstated.

### 1.3 A `yield return` inside the window (#5776)

§28.8.1's exit list names "`yield break` (in iterator methods), and any other transfer of control that leaves the method body"; §29.7's abandonment runs the cleanup pass "exactly as if the body had exited at the suspension point". Reading A: the `yield return` is such an exit, so `var t := move _r; yield return t.N; _r := t;` is rejected. Reading B: `yield return` has a resume edge and is not listed, so the restoration after it satisfies the rule — and abandonment then disposes `t`, leaving `_r` vacated for good, which the holder's destructor meets.

### 1.4 The owned-field cycle (#5774)

```bezel
using var h := new Node();        // Node: private owned _next: Node?
h._next := new Node();            // a: owned by h
var a := h._next!;                // borrow of a (field-sourced; K5.6: not frozen)
a._next := move h._next;          // a owns itself
h._next := null;                  // K9.2 restoration
```

Every step is admitted today; `a` is never disposed; no flag is set. §28.13 records cycles only through `Shared<T>`. Appendix C.3's exactly-once guarantee assumes an acyclic owned graph without saying so.

### 1.5 What the compiler does today (source read at `429fd72`; no build)

- The exception-edge half of K9.2 is `checkThrowableWhileFieldMovedFrom` (`src/ownership/Ownership_Checks.cpp:19312`): "a throwable expression evaluated while an owned field is moved-from IS itself an unrestored exit", read off the path-sensitive `fieldMovedFrom_` map. It implements W2 except in one place: a throwable inside a `try` is deferred to `fireAllPendingInTryChecks` (`:19441`, `:19642`), which admits it when a `catch` arm restores the field (`catchArmRestoresField`; #3241 R2; pinned by `own_throwable_edge_3080`'s `in_try_move_with_catch_restore_accepted_28_8_1`). W2 withdraws that admission (critic finding, §9). The `finally` dominance admission was never implemented (#5667 is a HYPOTHESIS pin that it should be).
- Per-slot tracking exists for owning inline arrays at constant indices only (`moveOnlyArrayNoteSlotMoved`, `Ownership.cpp:8666–8696`, via `foldIntConst`); a non-constant index is not tracked at all — neither per slot nor conservatively — so the "entire collection" state has no implementation. `move d[$"m{i}"]` compiles (`dict_string_key_leak_3703`, rows at lines 404 and 436).
- At run time a moved field slot is nulled (`LlvmBackend_Exprs2.cpp:648–700`, the #1328 merge gate: "the field slot kept the moved-out pointer … a double-free"), as is a moved collection slot (`lowerMoveFromCollectionSlot`, #747). V1's state is therefore already represented; what is missing is the checked-mode test on an alias read or store.
- `yield return` is not consulted by the restoration analysis (no `Yield` arm reads `fieldMovedFrom_`; `grep -n "yield" src/ownership/Ownership.cpp` finds none in that analysis), so reading B is the implementation's behaviour by omission.

---

## 2. Options

For the window (#5775, #5776):

- **W0 — status quo.** The `finally` form stays; the re-entrant read, the re-entrant store and the `await` interleaving are unstated; `yield return` is ambiguous.
- **W1 — the sentinel alone (the issue's recommendation (a)).** Keep the `finally` form; extend §9.4's disposed state to every vacated location; add `yield return` to the exit list. Sound in checked mode, flag-backstopped; keeps a window that may contain arbitrary calls, so every alias access inside it is a run-time fault rather than a compile error, and the compiler would have to implement the `finally` dominance analysis (#5667) plus the state tests.
- **W2 — the straight-line window (SIMPLIFY C6).** Delete the `finally` admission; the window admits only the non-throwable forms and `new`. Calls, indexer accesses, `await` and `yield return` are rejected at the expression. One sentence; the compiler's shipped classifier already does it.
- **W3 — reject calls, `await` and construction that could reach the holder.** Undecidable in general (statics, arguments); in practice W2 plus a ban on `new`, which v0.255 (#3240) ruled the other way for the single most common restoration idiom.
- **W4 — the replace primitive** (`move x.f` taking a replacement operand, deleting K9.2 entirely). Would rewrite every field-move site (77 gtest, 38 lit, 21 doc-case, 27 rule-case, 17 spec programs, SIMPLIFY §C6) and needs a spelling decision under the no-new-keyword rule. Recorded as a study, not taken.

For the residual under W2 — a `new` in the window whose constructor body reaches the holder, and an alias read spelled as a field read (`var y := o._r;` with `o` aliasing `this`, a non-throwable field read):

- **S0 — leave it unstated** (today's text for class-typed fields).
- **V1 — the vacated state** (W1's sentence, now with nothing but construction and field-read spellings able to meet it). §9.4's posture, generalized.
- **V2 — the restoring store is a run-time null-guarded drop-on-overwrite.** Makes the restoring store throwable under aliasing, against §28.8.1's inventory and build-before-take; and an alias read still reads a null through a non-nullable type.
- **V3 — a K4.3 recorded trust clause** ("no alias reaches a vacated location during its window") with a report entry. Adds to the register for a shape no program writes, and leaves the read undefined rather than faulting.

For the slot index (#5773):

- **S0 — status quo** (the escape).
- **S1 — trackable indices only (SIMPLIFY C7; the issue's recommendation (b)).** `move coll[e]` at a constant or an unreassigned local; restoration at the same index, `Clear()`, `Dispose()` or whole-value reassignment. Deletes the conservative state.
- **S2 — keep the conservative state, restore only at the same index expression over unchanged locals, else by `Clear()`/`Dispose()`** (the issue's option (a)). Keeps a second tracking mode for the sake of `move d[$"m{i}"]`, whose rewrite is one line.
- **S3 — a run-time moved-from fact at non-constant indices.** Makes K9.2 a run-time invariant at some indices and a static one at others.

For the cycle (#5774):

- **C0 — status quo** (silent leak, no record).
- **C1 — reject the decidable body-local shape** (a store of a moved slot's value into an owned field of a borrow sourced at that slot or below it) **and record the general case.** A new analysis for a shape with zero measured sites.
- **C2 — record the general case** beside `Shared<T>`'s note, state the acyclicity assumption in Appendix C.3, and note that W2 already rejects the issue's literal program.
- **C3 — a checked-mode owner-chain walk** at owned-field stores of a moved value. A run-time cost on every owned-field store of a moved value, for the same zero sites.

---

## 3. Evaluation

| | Soundness | Simplicity | Consistency | Migration reach (measured, §4) |
|---|---|---|---|---|
| W1 | checked-mode sound; unchecked undefined on every alias access in the window | keeps the `finally` form and adds the state and the dominance analysis | §9.4 generalized; K9.4's ground contradicted for statics (calls in a static slot's window) | 0 sites rejected; #5667 to implement |
| **W2** | every alias access in the window is a compile error except through construction and a field-read spelling, which V1 covers | one deletion, one sentence; the shipped classifier already does it | restores K9.2's pre-v0.332 shape; agrees with K9.4's ground and rationale §5.10 reason 3 | 2 rule cases, the spec's and the code-generation reference's own examples |
| W3 | as W2 | bans `new` in the window, against #3240 | contradicts the v0.255 ruling | every `var old := move _r; _r := new Res();` idiom |
| V1 under W2 | sound in checked mode; K4.1-undefined unchecked, like every alias of a killed slot | one paragraph, no new run-time state (the slot is already nulled) | the §9.4 posture at every vacated location; statics admitted by the straight-line window | 0 |
| S1 | per-slot tracking is exact; the restoring store targets a provably moved-from slot (non-throwable, which answers #5773's second gap) | deletes the second tracking mode | the element-borrow warning already uses exactly this index precision | 2 gtest rows, 1 stdlib sketch (+ its doc-case) |
| S2 | sound | keeps two modes | — | 0 |
| C2 | a leak, not a use-after-free; recorded where `Shared<T>`'s is | one bullet, one sentence | the register's existing cycle posture | 0 |
| C1 | as C2 plus one rejected shape | a new path-equality analysis | — | 0 |

**Decision: W2 + V1 + Y1 (`yield return` named as an exit, which W2 makes a consequence) + S1 + C2.** The owner's standing preference decides between W1 and W2: the `finally` admission has negligible measured reach — it was added at v0.332 so that a read of a vacated move-only field could follow the move, "not for a use case" (SIMPLIFY §C6), and no guide, test program or idiom uses it — so the feature is narrowed rather than given another rule. V1 is kept because W2 alone leaves two reachable spellings (a constructor body; an alias field read) undefined, and the sentence costs nothing the implementation does not already have. S1 over S2 for the same reason: one tracking mode, and the only programs it rejects bind the index first. C2 over C1 because the shape has zero sites and C1's analysis would be the proposal's only new mechanism.

**Kernel classification (process §9).** W2, Y1 and S1 are derivations under K9.2 (the exit set and the per-slot reading K9 already names); V1 is K4.1's backstop applied to a location, a derivation from K4 (the state is "the value's disposed flag" as §9.4 already puts it); C2 adds no rule — it records an assumption. No new axiom.

**Body-locality (proposal 0105 §1): local.** Every rule reads one body: the window, its index expressions and the restoring store are all in it.

---

## 4. Measurement

Scripts: `scratch/v0334/w4work/measure_windows.py` (every `:= move <field or slot>` in gtest string programs, lit, both conformance corpora and the documents, classified by whether its restoring store is in a `finally` or the window spans an `await`/`yield return`) and `measure_slot_index.py` (every `move coll[e]` whose index is neither a literal nor an identifier, and every identifier index reassigned before the restoring store). Both read the hit text; no false positive is counted below.

**W2 — newly rejected:**
- Rule corpus: `s9_4_0010` (`c.IsFull()` and `Console.WriteLine` inside a `try` restored in `finally`) and `s15_1_1_0009` (`Console.WriteLine(a[0] <> null)` likewise). Neither has a banked row in `baseline.tsv`. Each pins §9.4's checked-mode fault on a read of a vacated move-only field; under W2 the fault is still the rule for the non-throwable reads the window admits (`a.Length`, a `case` over the binding — the v0.332 #5666 ruling, unchanged), so each case is rewritten to such a read, its `rule:` header re-cited, and its expectation kept; the implementing tracker carries both.
- Documents: the specification's own example sentence in §28.8.1 (deleted with the paragraph) and the code-generation reference's §7.3 pair (`try _ := a[0]; finally h.Slots := t; end;`), rewritten here.
- gtest: **one** accepting row whose throwable sits in a `try` whose `catch` arm restores the field — `move_field_throwable_exception_edge_3080_test.cpp`'s `in_try_move_with_catch_restore_accepted_28_8_1` (line 280; critic measurement `scratch/v0334/W4crit/catch_restore.py`, which also finds only the two rule cases elsewhere) — flips to rejected; **zero** finally-placed restorations of a moved field or slot, zero windows spanning an `await`, zero spanning a `yield return` (SIMPLIFY §C6's "5–8 programs" counted call-containing windows, which today's rule already rejects or which are destructor bodies, K9.3-exempt). Lit: zero. Doc-case corpus: zero. Guides: zero.

**S1 — newly rejected:**
- gtest: `dict_string_key_leak_3703` rows `move_found_key_built_key_leak_free_4_9` (line 404, `move d[$"m{i}"]`) and `move_absent_key_throw_path_built_key_leak_free_4_9` (line 436, `move d[$"absent{i}"]`). Rewrite: `var k := $"m{i}"; … move d[k]` — the rows' purpose (the built key's release) is kept. (Their windows run to `d`'s scope-end disposal through checked arithmetic, `sum + taken.Tag`; without §9's collection exemption the rewrite would still be rejected by W2, as would most of the ~170 disposal-restored `move coll[i]` sites in gtest, e.g. `owning_span_779`'s `return t!.V;`.)
- Stdlib §4.5's `PopAt` sketch, `var shifted := move _storage[j + 1];` (line 1878), and its extracted doc-case `tests/conformance/cases/stdlib_0051.bzl` (manifest row `skip:signature-listing`, so no verdict changes); the sketch is rewritten in this proposal to bind the index. The other three `move _storage[…]` sketches (`index`, `slot`, `i`) are unreassigned locals and stay.
- The spec's `move span.Buffer[span.Start + i]` (§28.11) is the *definition* of `move span[i]`, not a program; unchanged.
- No identifier-indexed move in `tests/` or `docs/` reassigns its index before the restoring store (the `k := k + 1` loop of #5773 is the matrix's own counterexample).

**V1, Y1, C2 — newly rejected:** nothing. **Newly admitted (all items):** nothing. **Newly changed at run time:** V1 — a checked-mode read of, or store into, a vacated class-typed field or owning slot through a spelling other than the restoring store faults; today it reads or overwrites the null the compiler already leaves there (undefined by the text). Zero measured sites, since every spelling that could do it inside a straight-line window is a constructor body reached from a `new` in the window.

**Matrix cells (§7)** list the per-cell re-walk.

---

## 5. The normative edits

Exact `old → new` pairs are `scratch/v0334/W4.json`, ids `W4-0163-*`. In prose:

1. **K9.2** gains "a suspension point's abandonment edge (`yield return`, §29.7)" in its exit enumeration, and K9's closing sentence stops naming "the per-slot conservative tracking for non-constant indices" and names the index rule instead.
2. **§28.8.1 *The field-restoration rule*:** the exit list gains `yield return`, "whose abandonment edge (§29.7) leaves the body".
3. **§28.8.1 *Exception paths are not exempt*:** ▲ the paragraph "A restoration placed in a `finally` satisfies the rule as well …" is replaced by the straight-line statement (`W4-0163-5775-finally`, as amended by the critic): "**The window is straight-line.** Between a `move` of a field or slot and the store that restores it, no expression evaluated on any path is throwable under the inventory below (a `new` expression is not; its arguments classify on their own): a method call, an indexer access, an `await`, a `yield return` or any other throwable expression there is a compile error at that expression, naming the move. A restoration placed in a `finally` or in a `catch` arm does not cover a throwable expression in the protected block … One window carries no straight-line obligation: that of a slot of a heap owning collection (or of a `Span<T>` over one) whose collection is held by an owned local of this body — a `using var`, an owned `var` or an `owned` parameter, disposed rather than moved on every path — since every edge the body leaves by, the exception edges included, passes that collection's disposal, which skips the vacated slot. Code that runs inside a window — a constructor body run by a `new`, a destructor run at a scope end inside it, and in a collection window of that kind any callee — meets the vacated state." The inventory's throwable sentence names "`yield return` (its abandonment edge leaves the body, §29.7) and `await`". The disposed-state paragraph's sentence "Between such a move and its restoration, `a.Length` … need no `finally`, and `a[0]`, an inline-array indexer access, does" becomes "… are admitted in the window, and `a[0]`, an inline-array indexer access, is rejected there: read it before the move."
4. **§28.8.1, new paragraph *The vacated location*** (after *Exception paths are not exempt*): ▲ "From the move to the restoring store the vacated location — a class-typed `owned` field, an owning-collection slot, an owning-inline-array slot, or a form-(iv) static slot (§22.12) — is in §9.4's disposed state, holding no live value: in checked disposal mode a read of it through any spelling other than this body's tracked one (an alias of the holder, code run inside the window — a constructor body, a destructor, a callee where the window admits calls) faults as `DisposedAccessException` (§28.2), and a store into it other than the restoring store faults the same way before storing, its RHS the abandoned-store temporary of §28.7; unchecked mode elides both tests (K4.1). The restoring store, and this body's reads of the location after it, are emitted without the test. The state is what makes §28.7's liveness test — 'non-null and not moved-from' — a fact at run time as well as in the analysis: no store other than the restoration can give the location a value inside the window, so the restoration displaces nothing and stays non-throwable. The object the move took is not flagged: a borrow of it taken before the move stays valid, bounded now by the taker's holder. A location a checked-mode fault leaves in the state is no hole: its value belongs to the taker, which disposes it once; the holder's cascade and a later overwrite dispose nothing through it, and every later read faults again — the posture §9.4 states for a move-only field, which this paragraph generalizes. Statics: a form-(iv) slot's window admits a per-slot `move` for exactly this reason — the straight-line window admits no call, and a constructor body run inside it meets the state." *(Composed at v0.334: proposal 0162 retires §22.12 form (iv) in the same train, so the form-(iv) list item and this *Statics:* sentence were struck before landing, and item 8 was dropped.)*
5. **§28.3 *The consume-site freeze*, *Scope*:** the sentence "A whole-value `move` of a **move-only** field … is the one kill by move that marks its storage …" becomes ▲ "The object is not flagged, but every `move` of a field or slot marks its **storage**: it leaves the location in §9.4's disposed state until the restoration store (§28.8.1 *The vacated location*), so that a read or store through an alias of the holder inside the window is flag-backstopped in checked mode (K4.1) rather than left reading the taker's value or storing over a hole — a same-domain case the domain clause does not reach; a whole-value `move` of a move-only field (`move h.Slots`, §15.3) marks it so that a borrowing-match binding addressing the field's storage reads the state."
6. **§28.11 *`move` on a collection slot*:** the nullable-restoration sentence becomes ▲ "The restoring store is a store at the same index of a fresh owned element, or — where the receiver's indexer admits `null`: `[N owned]T?`, a class-typed `OwningBuffer<T>` or a `Span<T>` over one (their slots are `T?`) — of `null`, which leaves the slot empty. `OwningArray<T>`'s and `OwningDictionary<K, V>`'s indexers take an owned `T` and `V` (stdlib §4.5, §4.9), so their slots are restored by a fresh element, by `Clear()`, or by the collection's disposal before the exit — an explicit `Dispose()`, or the scope end of a `using var` that holds it; 'take the element and leave the slot empty' is `PopAt`/`PopValue` there." The conservative paragraph and its two bullets are replaced by ▲ "**The index is trackable.** `move coll[e]` is admitted only where `e` is a compile-time constant or a read of a local the body does not reassign between the move and the restoring store — the per-slot precision the element-borrow warning above already uses; any other index is a compile error at the move naming the remedy: bind the index to a local first (`var k := Pick(); var t := move xs[k]; xs[k] := fresh;`). Moved-from state is tracked per slot, at that index. The restoration is the store at the same index, `Clear()` or the collection's disposal as above, or, for an owning inline array, a whole-value reassignment (§15.3); a store at another index restores nothing. Because the restoring store targets a slot the analysis knows to be moved-from, it displaces nothing and is non-throwable (§28.8.1)." The indexer-setter paragraph's last sentence gains "and at run time the slot is in the vacated state (§28.8.1 *The vacated location*), so no other store can have given it a value". The closing "same exception-safety rules" sentence cites the straight-line window.
7. **§15.3 *Local lifetime*** and **§15.1.1**: "at a constant or unreassigned-local index (§28.11)" and "at the same index".
8. **§22.12 form (iv):** one clause after "follow §15.1.1 unchanged": ▲ "(a window on a static slot is admissible because §28.8.1's window is straight-line, admitting no call, and a constructor body run inside it meets the vacated state)". *(Composed at v0.334: dropped — proposal 0162 retires form (iv) in the same train, and this clause's anchor was inside the paragraph 0162 deletes; with no static slot movable (K9.4 as 0162 restates it), the clause has no subject.)*
9. **§28.13 cycles bullet:** ▲ "The compiler does not detect ownership cycles, through `Shared<T>` or through plain owned fields: a `move` of a field or slot stored into an owned field of an object the moved value transitively owns (`var t := move h._next; h._next := null; a._next := t;` with `a` a borrow of the taken node) forms a cycle no cascade reaches, and the objects on it are never disposed. Appendix C.3's exactly-once guarantee assumes the owned graph is acyclic, which the program maintains. Break cycles with `Weak<T>`."
10. **Appendix C.3:** the `move coll[i]` row reads "admitted only at a constant or unreassigned-local index, tracked per slot, restored at that index, by `Clear()`/disposal or by whole-value reassignment"; a new bullet states the acyclicity assumption and the vacated state.
11. **Stdlib §4.5:** `PopAt`'s loop body binds `var k := j + 1;` and moves and restores at `k`.
12. **Rationale §5.10:** one dated paragraph recording W2, V1, S1, C2 and the declines.
13. **Code-generation reference:** §4.2's restoration sentence names a `yield return`'s abandonment edge among the exits and gains nothing else (the document's maintenance rule: new clauses land as pairs, not appended sentences); §7.3's §9.4 pair is rewritten (the rejected form is the indexer read inside the window; the fix reads before the move or matches the taken local); §7.2 gains one pair for the built-key slot move.

---

## 6. Declined alternatives (rows for `DECLINED.md`)

| Topic | Verdict | Reason | Reopening bar |
|---|---|---|---|
| Keeping v0.332's `finally`-placed restoration and giving every vacated location a run-time sentinel (option W1; #5775's recommendation (a) alone) | `declined-at-proposal` | the admission has no measured user (zero gtest, lit, doc-case or guide sites; two rule cases written to exercise §9.4's fault), it contradicts K9.4's ground for statics, and it would make every alias access in a call-bearing window a run-time fault where a compile error is available | a real program that must call out of a window with a field taken, which the take-restore-then-call rewrite cannot express |
| Rejecting `new` inside a restoration window (option W3) | `declined-at-proposal` | re-litigates the v0.255 #3240 ruling that `new` is not analysis-throwable; rejects `var old := move _r; _r := new Res();` everywhere; the constructor-body residual is covered by the vacated state | —(none stated) |
| Making the restoring store a run-time null-guarded drop-on-overwrite (option V2; #5775 option (b)) | `declined-at-proposal` | reclassifies the restoration as throwable, against §28.8.1's inventory and build-before-take; an alias read would still read a null through a non-nullable type | —(none stated) |
| Keeping the conservative whole-collection moved-from state and restoring only at the same index expression over unchanged locals (option S2; #5773 option (a)) | `declined-at-proposal` | keeps a second tracking mode whose only clients bind their index in one line; the implementation never had the state | —(none stated) |
| Rejecting statically the body-local cycle shape — a `move`d slot's value stored into an owned field of a borrow sourced at that slot (option C1; #5774 option (a)) | `declined-at-proposal` | a new path-equality analysis for a shape with zero measured sites; the general case is undecidable and is recorded instead | a program that forms the cycle by this shape in `tests/` or a report of one |

Recorded as a study, not declined: the replace primitive (option W4), with its migration count (SIMPLIFY §C6).

---

## 7. The matrix cells after the change

Verdicts are the disposal matrix's (`docs/research/disposal-matrix/runs/v0.333.json`); "closes" means the cell's open status is decided by this proposal's text.

- **R4/C5, R5/C5, R6/C5, R9/C5** (#5775's RC-A face; R5/C5 also #5773): *conditional, missing/conflict* → **decided, once**. No call in the window; the restoring store is the one store; a constructor body or an alias field read meets the vacated state (once-backstopped on that leg).
- **R4/C3·reentrant** (route): **closed** — the `finally`-restored call is rejected.
- **R4/C6, R6/C6** (#5775 and #5747's aliasing leg): *ambiguous* → **decided**: "not moved-from" is the static per-spelling fact for the analysis and a run-time fact under the vacated state; `var t := move o._r; _r := new Res(); o._r := t;` with `o` aliasing `this` is rejected (the middle store is a possibly-live drop-on-overwrite inside the window). R6/C6's editorial half (RC6, slot-store scope) is W3's.
- **R4/C10, R5/C10, R6/C10, R9/C10** (#5775's `await` leg): *ambiguous* → **decided, rejected**: `await` is throwable and the window admits no throwable.
- **R4/C11, R5/C11, R6/C11, R9/C11** (#5776): *ambiguous/missing* → **decided, rejected** (reading A, now stated). R6/C11's other leg (RC-L5-1, #5766) is W7's.
- **R5/C2, R6/C2** (#5773; R6/C2 also #5781): the slot-index half → **decided, once** (per-slot restoration at a trackable index). The return-edge half is RC43's ruling (W4.md).
- **R5/C8, R6/C8, R9/C8** (#5773): *conflict* → **decided**: the loop form rebinds its index or is rejected; `for var i in …` with an unreassigned `i` stays per-slot.
- **R4/C12·cycle** (#5774): *missing* → **decided, recorded**: the residual is named at §28.13 and Appendix C.3; the issue's program is rejected by the straight-line window.
- **R9/C5, R9/C8, R9/C10, R9/C11** (the form-(iv) face): n/a at landing — proposal 0162 retires form (iv) in the same train and its K9.4 admits no `move` or slot store through static storage, so these faces are decided there (rejected), and this proposal's §22.12 clause was dropped with the form.
- Neighbours re-walked, no change: **R4/C2** (break/continue stay inside the window), **R4/C7** and **R5/C7** (an arm `move _f` is a fresh-producer arm; restoration per path), **R4/C8** (a move before a zero-trip loop is still rejected), **R1/C5** and **R3/C5** (RC08 and RC42, decided in W4.md), **R8/C11** (the frame's own owned state is K6's, untouched).

Newly opened: none found. The one question W2 leaves to the implementing tracker is diagnostic placement: the error sits at the throwable expression and names the move, as `checkThrowableWhileFieldMovedFrom` already does.

---

## 8. Changelog, migration, trackers

- **Class.** Design change (two admissions withdrawn), graduated here; W2's deletion is also a reversal of v0.332's #5666 admission, recorded as such.
- **Migration.** *Newly rejected (spec-ahead-of-impl for S1 and Y1; implementation-aligned for W2):* a throwable expression inside a field or slot restoration window whose exit is covered by a restoring `finally` (two rule cases, the two document examples) or a restoring `catch` arm (one gtest row); `move coll[e]` at an index that is neither a constant nor an unreassigned local (two gtest rows, one stdlib sketch and its doc-case); a `yield return` inside a window (zero sites). *Newly changed at run time (spec-ahead-of-impl):* a checked-mode alias read of, or store into, a vacated class-typed field or owning slot faults (zero sites). *Newly admitted:* nothing. *Doc-only:* §28.11's `null`-restoration correction, §28.13's cycle record, Appendix C.3's acyclicity sentence, §22.12's clause, the rationale paragraph, the code-generation reference pairs. *Deleted text:* the `finally` paragraph, the conservative-state paragraph and its two bullets, the disposed-state paragraph's `finally` sentence, K9's closing clause (~22 lines).
- **Trackers (one, filed at landing, `spec-tracking`):** (i) S1's index rule — reject a non-trackable index at `move coll[e]` and track a local index per slot (today only constant indices on owning inline arrays are tracked, `Ownership.cpp:8666–8696`; heap collections have no slot tracking); (ii) Y1 — `yield return` inside a window; (iii) V1 — the checked-mode tests on an alias read or store of a vacated location (the slot is already nulled: `LlvmBackend_Exprs2.cpp:648–700`, `lowerMoveFromCollectionSlot`); (iv) the test flips named in §4 — `own_throwable_edge_3080`'s `in_try_move_with_catch_restore_accepted_28_8_1` (accepted → rejected at `Dangerous()`; the `catchArmRestoresField` deferral is deleted), `dict_string_key_leak_3703`'s two rows (bind the key), rule cases `s9_4_0010` and `s15_1_1_0009` (a non-throwable read in the window), the regenerated `stdlib_0051` doc-case. #5667 closes as "no longer admitted" with a pointer here; #5725 (non-`this` receivers) is unaffected.
- **Corpus cases to write after landing:** reject — a call under a restoring `finally`; `yield return` in a window; `move xs[Pick()]`; `move pool[k]` with `k := k + 1` before `pool[k] := t`; `a._next := move h._next` (the store is a possibly-live drop in the window). Accept — `var k := Pick(); var t := move xs[k]; xs[k] := new Res(9); return t;`; `using var d := …; var key := $"m{i}"; using var taken := move d[key];` (the `using var`'s disposal restores). Run (checked mode) — a constructor body reading the vacated field through `this` passed as an argument faults with `DisposedAccessException`.

---

## 9. Critic amendments (v0.334 lane critic)

The lane critic accepted W2 + V1 + Y1 + S1 + C2 and amended the edit set as follows; `W4.json` carries the amended text, rebuilt from `scratch/v0334/w4work/build_json.py`.

1. **The collection window.** As drafted, W2 governed every slot window, and S1 counted the collection's disposal (a `using var`'s scope end included) as a restoration. The two together rejected every program that takes a slot of a body-local collection and then uses the element before the collection dies — checked arithmetic (`sum + taken.Tag`), a `!`, a call — which today's text admits, since every exit edge, the exception edges included, passes the collection's disposal: the rewritten `dict_string_key_leak_3703` rows themselves, `owning_span_779`'s `return t!.V;`, and the bulk of the ~170 gtest `move coll[i]` sites with no restoring store (`scratch/v0334/W4crit/slot_disposal_windows.py`). The amendment keeps that reading: a slot of a heap owning collection held by an owned local of the body (a `using var`, an owned `var` or an `owned` parameter, disposed rather than moved on every path) carries no straight-line obligation; V1 backstops a callee that reaches the vacated slot through an alias. Fields, slots of field-held collections and owning-inline-array slots keep W2. Newly admitted: nothing (today's set).
2. **The `catch`-arm restoration.** The compiler admits a throwable whose enclosing `try`'s `catch` arm restores the field (#3241 R2), and K9.2's path reading admitted it. W2 withdraws it with the `finally` form: one gtest row flips (§4). The text now names `catch` beside `finally`.
3. **The inventory, not a whitelist.** "Every expression … is one of the non-throwable forms below" read as an enumeration (comparisons, `is`, unchecked arithmetic and `not` are in neither list); the sentence now says "no expression … is throwable under the inventory below". "Method or accessor call" is back to the inventory's "method call": whether a property read is a call is the inventory's open question, not this proposal's.
4. **Who reaches the vacated location.** "Crosses no method boundary except a constructor body" was false: a destructor runs at a scope end inside a window (an auto-dropped local, a `using var` in a nested block), and in the excepted collection window any callee runs. The sentence lists them; each meets the vacated state.
5. **"Sets no flag".** §28.3 *Scope*'s preceding clause says a slot kill by move "disposes nothing and sets no flag" (the object's flag); the new sentence now opens "The object is not flagged, but every `move` … marks its **storage**".
6. **Form (iv).** W3's proposal 0162 retires §22.12 form (iv). The edits that depend on the form carry a `composition` note in `W4.json` (`W4-0163-5775-22.12-iv` whole; two fragments of `W4-0163-5775-vacated`; one clause of `W4-0163-rationale-5.10`); none was deleted here. The R9 cells (§7) become n/a with the form; the decline of W1 keeps its other grounds (a compile error where W1 gives a fault; no dominance analysis). *(Composed at v0.334: 0162 landed in the same train; the drop and the two strikes were applied, and the rationale's decline names only those other grounds.)*

**Residual questions (to file, not decided here):** (a) a scope-end disposal inside a window (`var t := move _r; do var x := new Res(); end; _r := t;`) runs a destructor that may throw; the inventory classifies expressions, not scope ends, so its edge is neither rejected nor listed — the vacated state makes it sound, but K9.2's "every exception edge" wording and the inventory disagree; (b) whether a property read is a "method call" in the inventory (the compiler treats a member access as a field read).
