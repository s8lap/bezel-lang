# Proposal 0170 — The restoration window's holder and index are stable; a body-local collection's binding is frozen in its window

| | |
|---|---|
| **Status** | graduated-v0.335 — superseded — graduated v0.335 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues", the 2026-10-09 direction "resolve all findings autonomously" and the 2026-10-09 "Please proceed as you recommend" after the v0.334 matrix re-runs: options H1, F1, C2 and A2 — the restoration window's index local and holder binding are stable over the window (no store into, transfer of or `ref`/`out` argument naming them between the move and the restoration), a body-local collection's binding and the live local borrows whose referent is the collection are frozen in its slot window (no member call but the store at the vacated slot's own index and `coll.Dispose()`, no borrow passed, captured, yielded, stored or iterated), `Clear()` leaves the restoration list while the destruction of the holder's value restores, and an `await using var` declared inside a straight-line window is rejected (#5833, #5843, #5796's `await using` face); critique round 1 applied, shared with the lane's five rulings (15 fixes), among them the freeze's reach over element borrows and its admission of a borrowing `for` (a deterministic checked fault where a compile error is available), a store at a new key that inserts, a move-only holder's unmarked transfer, a field holder's discard (no restoration), and the residual of a task started before the move; landed with the v0.335 changelog entry) |
| **Tracking issue** | [#5833](https://github.com/s8lap/bezel-lang/issues/5833) (NR02: structural mutation, `Clear()`, a closure- or `ref`-written index inside a body-local collection window), [#5843](https://github.com/s8lap/bezel-lang/issues/5843) (NR17: rebinding the holder inside the window), [#5796](https://github.com/s8lap/bezel-lang/issues/5796) (NR08, the `await using var` face only; the scope-end edge and the discard are ruled in the v0.335 changelog, not here). Implementation: [#5828](https://github.com/s8lap/bezel-lang/issues/5828) (proposal 0163's tracker) and [#5725](https://github.com/s8lap/bezel-lang/issues/5725) (non-`this` receivers), plus the tracker the v0.335 landing files for this proposal's deltas. |
| **Target module(s)** | **spec**: §28.3 (K9's closing sentence), §28.8.1 (*The field-restoration rule*: the holder's stability; *The window is straight-line*: the collection freeze and the `await using var` rejection), §28.11 (*`move` on a collection slot*: the nullable-restoration sentence; *The index is trackable*: the stable index and holder, the restoration list), §15.3 (*Local lifetime*), Appendix C.3 (the `move coll[i]` row). **rationale**: §5.10 (one dated paragraph). **code-generation reference**: §7.2 (three *Looks right, is rejected* lines and their fixes; the §28.11 quote). **stdlib**: none (the §4.5 reference `Clear` is unchanged: it is no longer a restoration, so its vacated-slot read is a callee's read and faults as *The vacated location* states). |
| **Written against** | spec/stdlib **v0.334** at `fd8a9a2` (main). Section numbers are v0.334's. |
| **Origin** | The v0.334 re-run of the disposal matrix (`docs/research/disposal-matrix/runs/v0.334.json`): roots NR02 and NR17 of `scratch/matrix2/ROOTS2.md` (lanes L2 RV2, L3 R2/R3, L5 RC-L5-R1; L1 lead C), and the `await using var` face L5 added to #5796. Decided in the v0.335 train's lane X1. |

*Reference convention: unprefixed §-references are language-spec references at v0.334; "stdlib §" references are to the standard library companion; K tags are §28.3's kernel ids. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Summary

Proposal 0163 (v0.334) made the K9.2 restoration window straight-line and tracked a slot move **per slot, at that index**, and it kept one window open to calls: a slot of a heap owning collection held by an owned local of the body, where "any callee" may run because every exit edge passes the collection's disposal, which skips the vacated slot. The re-run found that the open window and the per-spelling tracking together leave three ways for the restoring store to land on a slot that is not the one the move vacated, and one way for it to land in another object:

1. **Structural mutation of the collection inside its own window** (#5833 face 1, R5/C5, R5/C6·reindex, R5/C10, R5/C11): `var t := move xs[1]; xs.RemoveAt(0); xs[1] := t;` — the hole moves to index 0, the untested restoring store overwrites the live third element without disposal, and the cascade later skips the hole. The same through a callee's `Insert`, an `await`ed continuation, or a consumer of `yield return xs`.
2. **An index written outside the body's text** (#5833 face 3): `var k := 0; var bump := lambda() => do k := 1; end; var t := move pool[k]; bump(); pool[k] := t;` — "a local the body does not reassign" is true of `k`, and the store lands on live slot 1.
3. **`Clear()` as a restoration** (#5833 face 2): §28.11 lists it, but the reference `Clear` (stdlib §4.5, which "the standard library provides … directly") `move`s every slot below `_count`, so on the vacated slot it faults in checked mode and raises `NullReferenceException` unchecked — a sanctioned restoration that raises mid-pass.
4. **Rebinding the holder inside the window** (#5843): `var t := move p._r; p := q; p._r := t;` — a borrow-state store is non-throwable and admitted; `q`'s `Res` is overwritten undisposed and `p`'s original leaves with the hole, which checked mode never meets because the second object's location was never vacated.

**Decision.** Narrow, in the posture 0163 chose for every other window, and state the one residual on the flag the specification already relies on:

- **H1 — the holder and the index are stable.** Over the window, the index local is a §7.4 **stable local** (not mutably captured, not named by a `ref` or `out` argument) that the body does not reassign, and the holder binding (`x` in `move x.f`, `coll` in `move coll[k]`) is likewise stable and is neither stored into nor moved between the move and the restoration. A violation is a compile error at that site, naming the move. `this` and a `using var` satisfy this trivially.
- **F1 — the collection binding is frozen in its window.** In the one window that admits calls, the body calls no member on the collection other than a restoration form (the store at a vacated slot's own index `coll[k] := v`, `coll.Dispose()`), passes no borrow of it (argument, `ref`/`out`, `yield return` operand, stored value, a `for` loop over it, capture of a lambda formed there), and the same holds for every live local borrow whose referent is the collection itself — an alias, a span over it, a closure or frame carrying it, whose invocation or advance counts as a call (K5's live local borrows; an element borrow is not one). Slot reads, further slot moves and property reads stay admitted (they meet the vacated slot as *The vacated location* states). The residual — a structural member that moves the vacated slot, reached through an **untracked** alias or run at a suspension inside the window by a task started before the move — reads the vacated slot as it shifts or copies the storage and faults in checked mode; a growing `Add` reaches it or not depending on the capacity; unchecked mode elides the test (K4.1). *(Critic amendment: the lane's draft cited "K5.1's tracked set" — K5's clause 1 is the family-1 transfers — and admitted iteration, whose enumerator reads every slot: a fault where a compile error is available, the ground F2 is declined on.)*
- **C2 — destruction restores; `Clear()` does not.** The restorations are the store at the same index and the destruction of the holder's value: a heap collection's disposal on every path (explicit `Dispose()`, the scope end of the `using var`, owned `var` or `owned` parameter holding it, a discard of that local) and an owning inline array's whole-value reassignment or discard of the array local. `Clear()` leaves the list: it is a member call, admitted in no window; the remedy is `PopAt`, then `Clear()`.
- **A2 — `await using var` inside a straight-line window is rejected** at its declaration, its scope end being a suspension, as an `await` expression there already is.

No new keyword, no new analysis beyond what 0163's tracker already owes (per-slot tracking at a local index; tracking `move x.f` at non-`this` receivers, #5725): the stable-local test is §7.4's, the freeze is K5's shape. **Newly rejected, measured:** zero sites in `tests/` (gtest, lit, both conformance corpora), the documents' programs and the guides for every one of the four decisions (§4). **Newly admitted:** the discard of an owning inline array local with a vacated slot, which the compiler already accepts (one gtest row, `undisposed_leak_test.cpp` `discard_inline_owned_partial_null_slots_1016`).

---

## 1. The problem, in the text

### 1.1 The open window and the per-spelling index (#5833)

§28.8.1 *The window is straight-line* (v0.334): "One window carries no straight-line obligation: that of a slot of a heap owning collection (or of a `Span<T>` over one) whose collection is held by an owned local of this body — a `using var`, an owned `var` or an `owned` parameter, disposed rather than moved on every path — since every edge the body leaves by, the exception edges included, passes that collection's disposal, which skips the vacated slot. Code that runs inside a window — … in a collection window of that kind any callee — meets the vacated state."

§28.11 *The index is trackable*: "`move coll[e]` is admitted only where `e` is a compile-time constant or a read of a local the body does not reassign between the move and the restoring store … Moved-from state is tracked **per slot**, at that index. The restoration is the store at the same index (above), `coll.Clear()` for the collections that provide it, the collection's disposal on every path to the exit … Because the restoring store targets a slot the analysis knows to be moved-from, it displaces nothing and is non-throwable."

§28.8.1 *The vacated location*: "no store other than the restoration can give the location a value inside the window, so the restoration displaces nothing and stays non-throwable."

The last sentence is true of the *location*; the analysis, however, addresses the location by an index. A structural member — `RemoveAt`, `Insert`, `PopAt`, `Sort`, `Reverse`, a growing `Add` — moves which location an index names (stdlib §4.5's reference `PopAt` `move`s every shifted slot down by one; `GrowTo` `move`s every live element into a fresh buffer). Two readings follow, and the text supports neither over the other: (A) the relocation *reads* the vacated slot through another spelling and faults in checked mode — deterministic for `RemoveAt`, `Insert`, `Sort` and `Reverse`, capacity-dependent for `Add`; (B) the vacated state travels with the element, the restoring store at the tracked index, emitted without the test, overwrites a live element without disposal in both modes, and the cascade skips the relocated hole for good. Reading B is a leak with no backstop; reading A is a run-time fault for a shape a compile error can name.

```bezel
class Res
  private _n: int;
  public constructor(n: int)  _n := n;  end constructor;
  destructor  Console.WriteLine($"closed {_n}");  end destructor;
end Res;
class Demo
  public static method Shift()
    using var xs := new OwningArray<Res>();
    xs.Add(new Res(0)); xs.Add(new Res(1)); xs.Add(new Res(2));
    var t := move xs[1];        // body-local holder: the window admits calls
    xs.RemoveAt(0);             // A: a read of the vacated slot (checked fault); B: the hole moves to index 0
    xs[1] := t;                 // B: overwrites Res(2) undisposed — "closed 2" is never printed
  end Shift;
end Demo;
```

The index face is the same mechanism one step removed: "a local the body does not reassign" is a statement about the body's text, and §27.5's shared cell or a `ref` argument writes the local from outside it. §7.4 already has the term that excludes both — a **stable local** "is not mutably captured, is not named by a `ref` or `out` argument in the region".

### 1.2 `Clear()` (#5833 face 2)

§28.11 names `coll.Clear()` a restoration. Stdlib §4.5's reference `Clear` runs `var item := move _storage[slot]; _storage[slot] := null; item!.Dispose();` over every slot below `_count`, under the comment "slot was non-null (slot < _count guarantees it)". On the vacated slot the `move` is a callee's read through another spelling — *The vacated location* says it faults in checked mode — and unchecked, `item!` raises `NullReferenceException`. Exactly-once survives (the later cascade skips the vacated slot), but the two passages give different verdicts for `var t := move _pool[0]; _pool.Clear(); return t;`. In a field window that program is already rejected: `_pool.Clear()` is a method call inside a straight-line window. The conflict is therefore confined to the body-local collection window, where it is also the one admitted call that is structural.

### 1.3 Rebinding the holder (#5843)

§28.8.1 *The field-restoration rule*: "every control-flow path from the move site to the method's exit must execute an assignment `x.f := value;` … The rule reads a field of **any** object `x` — `this`, a parameter, or a local holder the method itself disposes". The restoration is stated over the spelling. A plain parameter is reassignable, a store into a borrow-state local is non-throwable (§28.8.1's inventory), and nothing says what a store into `x` between the move and the restoring store does:

```bezel
class Holder
  private owned _r: Res;
  public constructor(n: int)  _r := new Res(n);  end constructor;
  public static method Swap(p: Holder, q: Holder)
    var t := move p._r;      // p's object: _r vacated
    p := q;                  // borrow-state store: non-throwable, admitted in the window
    p._r := t;               // "the restoring store": by spelling it restores; by object it overwrites q's Res undisposed
  end Swap;
end Holder;
// using var a := new Holder(1); using var b := new Holder(2); Holder.Swap(a, b);
// a leaves Swap with _r vacated for good; "closed 2" is never printed.
```

The owned-local twin, `var h2 := move h; h := new Holder(0); h.f := t;`, is the same shape with every store non-throwable (a store into a moved-from local). The collection twin, `var t := move xs[0]; xs := Fill(2); xs[0] := t;`, sits in the call-admitting window, where the drop-on-overwrite of `xs` is throwable but admitted. Under a by-object reading all three are unrestored exits; under the by-spelling reading the per-slot text suggests, all three leak, and checked mode cannot catch the leak because the second object's location was never vacated.

### 1.4 `await using var` inside a straight-line window (#5796, the L5 face)

The straight-line rule rejects an `await` **expression** inside the window. An `await using var` declared inside it suspends at its scope end — `DisposeAsync()` awaited, §28.12 — at no expression the inventory classifies, so interleaved continuations run inside a window the text says admits no suspension. They meet the vacated state (backstopped), but the admission contradicts the rule's own statement.

### 1.5 What the compiler does today (source read at `fd8a9a2`; no build)

- Heap collection slots are not tracked at all; owning inline-array slots are tracked at constant indices only (`src/ownership/Ownership.cpp:8700–8760`, `moveOnlyArrayLive_`); the index rule and the vacated-state tests are #5828's.
- `move x.f` is tracked for `this`-rooted fields only (`fieldMovedFrom_`, `src/ownership/Ownership_Checks.cpp:19308–19330`); non-`this` receivers are #5725's. Nothing freezes a holder or a collection binding.
- `findThrowable28_8_1` (`src/ownership/Ownership_Checks.cpp:19221–19303`) classifies expressions; a scope end, an `await using var` declaration and a discard reach no arm of it.
- A discard of an owning inline array local with a vacated slot is accepted and lowered as a null-skipping pass (`tests/codegen/undisposed_leak_test.cpp` `discard_inline_owned_partial_null_slots_1016`, run under ASan).

---

## 2. Options

**H — the holder and the index.**
- **H1 (chosen).** Both are stable locals over the window (§7.4's term), neither reassigned nor moved; a violation is a compile error at the site, naming the move.
- **H2.** Track the vacated *object* through rebinding: after `p := q`, the obligation follows `p`'s old value to whatever binding holds it (`h2` in the owned-local twin) and `p._r := t` is an ordinary possibly-live store. A by-object analysis the compiler does not have, for a shape with zero sites.
- **H3 (status quo).** By spelling; the programs of §1.3 compile and leak.

**F — the collection window.**
- **F1 (chosen).** Freeze the collection binding and the live local borrows whose referent is the collection inside the window (K5's shape; a `for` over it included, an element borrow excluded); state the untracked-alias residual as reading A on the vacated state's flag.
- **F2.** Reading A alone (#5833 option 2; L5's recommendation): every structural member meeting a vacated slot faults in checked mode, growth's internal relocation excluded or not from "a read". Keeps the window open and makes a statically visible `xs.RemoveAt(0)` a run-time fault — capacity-dependent for `Add` — where a compile error is available; the exclusion for growth would make `Add` silently relocate the hole (reading B for one member).
- **F3.** Reading B with a tested, null-guarded restoring store at a heap collection (#5833 option 3). Reclassifies the restoration as throwable, against build-before-take, and still leaves the relocated hole in the collection for good (the cascade skips it; the element that was there is lost to no one, but the slot the body thinks it restored is a different one). 0163 declined the same store for the field case (V2).
- **F4.** Close the collection window (every slot window straight-line). Undoes 0163's critic amendment 1 and rejects the ~170 gtest sites that use the taken element before the collection dies.

**C — `Clear()` and the restoration list.**
- **C1.** Keep `Clear()` as a restoration and state that a collection's own cascade members (`Dispose()`, `Clear()`) meet a vacated slot as empty; rewrite the reference `Clear` with no `!`. Needs a spelling exemption for the collection's own body that *The vacated location* does not have (its reads are "through another spelling"), and a reference sketch that cannot express the exemption in Bezel.
- **C2 (chosen).** Drop `Clear()` from the list — it is a call, and under F1 it is rejected in the collection window and under the straight-line rule everywhere else — and name the destruction forms that do restore, the discard of the holding local included.
- **C3.** Keep `Clear()` and admit it only in the collection window as a frozen-binding exception. A per-member carve for zero sites.

**A — `await using var` in a straight-line window.**
- **A1.** Admit it under the vacated state (the #5796 comment's suggested wording extended to the suspension).
- **A2 (chosen).** Reject the declaration inside the window, as an `await` expression is rejected.

---

## 3. Evaluation

| Option | Soundness | Simplicity | Consistency | Reach (sites) |
|---|---|---|---|---|
| H1 | closes §1.3 statically; residual none (an alias of the holder is 0163's alias residual, unchanged) | reuses §7.4's term; one sentence per site | the same bar the index already has | 0 |
| H2 | sound | a new by-object analysis | — | 0 |
| H3 | leaks, unbackstopped | — | — | — |
| F1 | closes §1.1 statically for the binding and tracked borrows; untracked alias on the flag (K4.1), capacity-dependent for `Add` | one paragraph; K5's shape | 0163's posture: a compile error where available, the flag where not | 0 |
| F2 | backstopped, capacity-dependent for a statically visible `Add` | one sentence | a fault where a compile error is available (0163 declined W1 on this ground) | 0 |
| F3 | leaves the relocated hole | a second restoring-store regime | against build-before-take (0163 V2) | 0 |
| F4 | sound | deletes a window | undoes a measured amendment | ~170 |
| C1 | sound if the exemption is stated | a spelling exemption + a sketch rewrite | the sketch cannot show it | 0 |
| C2 | sound | one form fewer | `Clear()` is a call like `RemoveAt` | 0 |
| C3 | sound | a per-member carve | — | 0 |
| A1 | backstopped | one clause | contradicts "no suspension" | 0 |
| A2 | sound | one clause | `await` is already rejected there | 0 |

---

## 4. Measurement

Method: `scratch/v0335/x1work/scan_windows.py` (raw text: gtest `.cpp` with string escapes unescaped, lit `.bzl`, both conformance corpora, every `docs/*.md`) and `scan2.py` (program text only: joined gtest literals, `.bzl`, fenced `bezel` blocks). For every `move x.f` / `move coll[e]` the text up to the same-spelling restoring store (or the end of the member body) was classified. 505 windows in program text (gtest 127, lit 39, conformance 62, docs 26); 787 in raw text.

- **H1 — holder stored into or moved inside the window:** 0 program sites. Every `rebind` hit is a whole-value reassignment of an owning inline array (`a := [new Cell(), new Cell()]` after `move a[0]`, `move_only_inline_array_reassign_drop_on_overwrite_1385`, `owning_inline_array_whole_value_reassign_3263`) — the restoration form §15.3 already names, which C2 keeps — or prose. **Index local reassigned or `ref`-passed in a window:** 0 program sites; the one `k := k + 1` hit is the code-generation reference's own *rejected* line.
- **F1 — a member call on the collection, the collection passed, captured or yielded inside its slot window:** 0 program sites (the one `collcall` hit, `owner.Add` in `move_borrowing_span_slot_871`, precedes the move). Further slot moves on the same collection inside a window: 16 sites (the swap and the two-slot shapes, `owning_inline_array_whole_value_reassign_3263`, `move_only_scrutinee_pattern_binding_borrow_1665`, rule cases `s15_1_1_0001`, `s15_3_0009`, `l4_5_0010`, `l4_14_0008`) — F1 admits them. A slot read of another index inside a window: 1 (`assign_target_index_moved_from_1932`, an inline-array indexer access 0163 already rejects); `Count`/`Length` reads: 0 in collection windows (`s15_1_1_0009`'s `a.Length` is the inline-array compiler-provided read 0163 admits).
- **C2 — `Clear()` as a restoration:** 0 program sites in `tests/`, the documents' programs and the guides; the three prose mentions are §28.11 (×2) and the code-generation reference's §7.2 quote, edited here. **Discard of the holding local inside its window:** 1 site, `undisposed_leak_test.cpp:732` (`var c := move a[1]; _ := a;`), accepted and ASan-clean today — C2 states it.
- **A2 — `await using var` declared inside a window:** 0 sites.
- **Corpus baseline:** no banked rule case states `Clear()` as a restoration, a holder rebinding, a captured index or an `await using` in a window; no verdict moves. `s15_3_0008` (reject: an inline-array slot moved and never reassigned, the scope end as the exit) is unchanged — C2 names the explicit kills, not the scope end (§8, residual).

---

## 5. The normative edits

Exact `old → new` pairs are `scratch/v0335/X1.json`, ids `X1-0170-*`. In prose:

1. **§28.11 *The index is trackable*** ▲ becomes *The index is trackable, and the holder and index are stable*: the index local is a §7.4 stable local the body does not reassign over the window; the holder binding is stable and is neither stored into nor moved there (a compile error at the site, naming the move; `this` and a `using var` trivially satisfy it); the restoration list is the same-index store and the destruction of the holder's value (a heap collection's disposal on every path — explicit `Dispose()`, the scope end of the `using var`, owned `var` or `owned` parameter holding it, a discard of that local — and an owning inline array's whole-value reassignment or discard); `Clear()` restores nothing, being a member call admitted in no window (remedy: `PopAt`, then `Clear()`).
2. **§28.11 *`move` on a collection slot*,** the `OwningArray<T>`/`OwningDictionary<K, V>` sentence: "by a fresh element, by `Clear()`, or by the collection's disposal" ▲ "by a fresh element or by the collection's disposal on every path to the exit — an explicit `Dispose()`, the scope end of the `using var` or owned local that holds it, or a discard of that local".
3. **§28.8.1 *The window is straight-line*:** ▲ after the `await`/`yield return` sentence: "an `await using var` declared inside the window is rejected the same way at its declaration, its scope end being a suspension (§28.12)"; ▲ after the collection-window sentence, the freeze paragraph (F1) with the residual stated on the flag.
4. **§28.8.1 *The field-restoration rule*:** ▲ one sentence after "The remedy is the same at every receiver …": the holder is stable over the window; `var t := move p._r; p := q; p._r := t;` is rejected at `p := q;`, naming the move.
5. **K9's closing sentence:** ▲ names the stable holder and index and the collection freeze beside the index rule.
6. **§15.3 *Local lifetime*:** "at a constant or unreassigned-local index" ▲ "at a constant or a stable, unreassigned-local index"; the restoration sentence ▲ adds the destruction forms (whole-value reassignment, a discard of the array local).
7. **Appendix C.3,** the `move coll[i]` row ▲ restated over the stable index and holder, the destruction forms and the freeze.
8. **Rationale §5.10:** ▲ one dated paragraph (v0.335) recording H1, F1, C2, A2 and the declines.
9. **Code-generation reference §7.2:** ▲ three rejected lines (the rebound holder; a member call on a frozen collection; `Clear()` as a restoration) and their fixes in the adjacent accept block; the §28.11 quote restated. Per the document's maintenance rule, no sentence is appended to prose.

---

## 6. Declined alternatives (rows for `DECLINED.md`)

| Topic | Verdict | Reason | Reopening bar |
|---|---|---|---|
| Reading A alone — a structural member meeting a vacated slot faults in checked mode, with the collection window otherwise open (option F2; #5833 option (2)) | `declined-at-proposal` | a run-time fault, capacity-dependent for `Add`, for a shape a compile error can name at zero measured sites; the ground 0163 declined W1 on | a measured program that must call a structural member on a collection inside its own slot window |
| Reading B with a tested, null-guarded restoring store at a heap collection (option F3; #5833 option (3)) | `declined-at-proposal` | reclassifies the restoration as throwable against build-before-take, and leaves the relocated hole in the collection for good; the field-case twin is 0163's declined V2 | —(none stated) |
| Keeping `Clear()` as a restoration with a vacated-tolerant cascade stated for the collection's own members (option C1; L3-R2 option (a)) | `declined-at-proposal` | needs a spelling exemption *The vacated location* does not have, for a form with zero sites, and a reference sketch that cannot express it; the `PopAt`-then-`Clear()` rewrite is one line | a measured program that takes a slot and clears the rest in one window |
| Counting the holding local's drop-on-overwrite (`xs := Fill(2)` inside the window) as the collection's disposal and a restoration (#5843's recommended exception) | `declined-at-proposal` | an exception to the stable-holder bar for a restoration form with zero sites; the store that follows it addresses a different object, which is exactly the shape the bar closes | —(none stated) |
| Tracking the vacated object through a rebinding of its holder (option H2) | `declined-at-proposal` | a by-object analysis the compiler does not have, for zero sites; the stable-holder bar is one sentence over §7.4's term | a measured program that must rebind a holder inside a window |
| Admitting an `await using var` declared inside a straight-line window under the vacated state (option A1; #5796's suggested wording) | `declined-at-proposal` | a suspension inside a window the rule says admits none; an `await` expression there is already rejected and the declaration's scope end is the same suspension | —(none stated) |

---

## 7. The matrix cells after the change

Verdicts are the disposal matrix's (`docs/research/disposal-matrix/runs/v0.334.json`); "closes" means the cell's open status is decided by this proposal's text.

- **R5/C5** (conflict, L3-R2 + L3-R3): → **decided, once**. `_pool.Clear()` after `move _pool[0]` is a call inside a field window (rejected, 0163) and `Clear()` is no restoration anywhere; the captured-index `pool[k] := t` is rejected at the move (`k` is not a stable local). The static `readonly OwningArray<T>` face stays decided (K9.4).
- **R5/C6·reindex** (route, ambiguous): → **closed**. `xs.RemoveAt(0)` inside `xs[1]`'s window is a member call on the frozen binding, rejected at the call; the untracked-alias residual faults in checked mode (reading A, stated).
- **R5/C10** (ambiguous): → **decided, once-backstopped**. `await Grow(coll)` passes a borrow of `coll` inside its window: rejected at the argument. An `await` with no borrow of the collection stays admitted; a continuation reaching the collection through a field-held or callee-stored borrow is the residual.
- **R5/C11** (ambiguous): → **decided, once-backstopped**. `yield return coll` inside the window is rejected; `yield return other` stays admitted, the abandonment edge passing the collection's disposal as before.
- **R4/C5, R5/C5, R5/C6** (the ·rebind face NR17 asked the next run to add): **decided, rejected** at the holder store, the holder move or the `ref`/`out` argument.
- **R4–R6/C3·scope-end** (the `await using var` face): → **decided, rejected** at the declaration inside a straight-line window; inside a collection window the declaration is admitted and its continuations meet the vacated state (once-backstopped). The synchronous scope-end edge and the discard are the v0.335 ruling on #5796.
- **R6/C9** (the discard of an inline-array holder, L4's face of NR01): the destruction restores the slot; the slot objects' blocks are NR01's.
- Neighbours re-walked, no change: **R5/C2** (a slot move with a trackable index restored at that index; the return edge is #5781's), **R5/C8** (`for var i in …` with an unreassigned `i` stays per-slot, and `i` is a stable local), **R6/C5** (an inline array is never a heap collection: its windows are straight-line, and its holder is a local the body may not rebind inside the window — the whole-value reassignment is a restoration, not a rebinding), **R6/C6** (a slot vacated through an alias is 0163's residual), **R4/C6** (`o._r := t` with `o` aliasing `this` is a possibly-live store, rejected), **R8/C11** (the frame's own owned state is K6's).

Newly opened: none found. The scope end of a plain-`var` owning inline array local with a vacated slot stays an unrestored exit (§15.3, `s15_3_0008` banked `reject`): the destruction forms this proposal names are the explicit kills; whether the scope end should join them is recorded in §8 as a question, not decided here.

---

## 8. Changelog, migration, trackers

- **Class.** Design change (two admissions narrowed — the open collection window, the by-spelling holder; one restoration form removed — `Clear()`; one restoration form stated — the discard of the holding local), graduated here. The removal of `Clear()` is a subtraction under process §10's bar: its keep was defended by no site.
- **Migration.** *Newly rejected (spec-ahead-of-impl, zero sites each):* a store into, a `move` of, or a `ref`/`out` argument naming the holder binding of a vacated field or slot inside its window; `move coll[k]` at an index local that is mutably captured or `ref`/`out`-passed in the window; a member call on a body-local owning collection inside its slot window other than the restoring `coll[k] := v` and `coll.Dispose()`, and a borrow of that collection passed, captured, yielded, stored or iterated there, or a closure or frame carrying it invoked or advanced there; `Clear()` as the restoration of a vacated slot; an `await using var` declared inside a straight-line window. *Newly changed at run time (spec-ahead-of-impl):* a structural member reached through an untracked alias of a collection with a vacated slot faults in checked mode (reading A stated; zero sites). *Newly admitted (implementation-aligned):* a discard of an owning inline array local, or of a body-local heap collection local, with a vacated slot, as the destruction that restores (one gtest row, already accepting). *Doc-only:* K9's closing sentence, §15.3's index wording, Appendix C.3's row, the rationale paragraph, the code-generation reference pairs.
- **Trackers (filed at landing, `spec-tracking`):** one tracker for this proposal's deltas — (i) the stable-local test on the index and the holder over the window (today: no heap-slot tracking, #5828; no non-`this` receiver tracking, #5725); (ii) the collection freeze; (iii) the `Clear()` removal (no test flips: no site uses it as a restoration); (iv) the `await using var` rejection; (v) reading A's checked-mode fault through an untracked alias. The accepted discard row (`undisposed_leak_test.cpp` `discard_inline_owned_partial_null_slots_1016`) is the pin of the newly stated restoration and does not flip.
- **Corpus cases to write after landing:** reject — `var t := move p._r; p := q; p._r := t;`; `var k := 0; var bump := lambda() => do k := 1; end; var t := move pool[k]; bump(); pool[k] := t;`; `var t := move xs[1]; xs.RemoveAt(0); xs[1] := t;`; `var t := move xs[0]; await Grow(xs); xs[0] := t;`; `var t := move _pool[0]; _pool.Clear(); return t;` (the call in a field window); `var t := move xs[0]; xs.Clear();` (the body-local window); `var t := move xs[0]; for var e in xs do … end; xs[0] := t;` (the enumerator would read the vacated slot); an `await using var` after `var t := move _r;`. Accept — `var t := move xs[0]; var u := move xs[1]; xs[0] := u; xs[1] := t;` (the swap); `var t := move xs[0]; await Task.Delay(1); xs[0] := t;`; `var a: [2 owned]Res? := [new Res(), new Res()]; using var c := move a[1]; _ := a;`. Run (checked mode) — a structural member on an alias of a collection with a vacated slot, taken before the move through a field the callee stored, faults with `DisposedAccessException`.
- **Residual question (to file):** whether the scope end of a plain-`var` owning inline array local with a vacated slot should restore as its discard does (§15.3's "auto-dispose on a moved-from slot is not defined" predates 0163's vacated state, under which the pass skips the slot); it flips the banked `reject` of `s15_3_0008`, so it takes the corpus-verdict process and is not decided here.
