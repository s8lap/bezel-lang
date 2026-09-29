// Codegen — `move coll[i]` slot-take from an owning collection (issue #409,
// language spec §28.8.1 / §28.11, stdlib §4.5 / §4.9).
//
// Spec §28.11 ("`move` on a collection slot"): "`move coll[i]` reads the i-th
// element of an owning collection as an owned value, leaving the slot in
// moved-from state." Spec §28.8.1: the moved-from slot must be left in a state
// the owner's later disposal skips. Spec §28.11's restoration rules and the
// OwningArray reference implementation (stdlib §4.5) make the moved-from slot
// observable as a NULL slot: "the compiler generates the default Dispose which
// disposes _storage's contents (every non-null slot's owned T)" and "the old
// buffer's destructor sees only null slots and does no further work."
//
// Root cause this file guards against (#409): `move IndexExpr` on an owning
// collection lowered to a plain indexer-get (a borrow) with no slot-null, so the
// element was carried to its new owner AND remained reachable from the
// collection. At scope end both the new owner and the collection's Dispose
// freed it — a double-free / segfault. The fix nulls the slot at the `move` so
// the collection's element-disposer (which already skips null slots) does not
// re-free it.
//
// Disposal is made observable through a static counter the element's destructor
// bumps; the counter (often encoded) is the exit code. Before the fix these
// programs SEGFAULT / double-free; after, they dispose each element exactly once.

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Compile with generated-code AddressSanitizer, run under LeakSanitizer, and
// return the program's exit code — the dispose-exactly-once oracle for this
// slot-move file. A double-free / use-after-free (the #409 defect these tests
// guard against) or a leak FAILS the run (ran=false -> -999), so the sanitizer
// verdict rides on the same assertion as the destructor-count value. This
// consolidates the retired hand-rolled JIT-count + bespoke-LSan pair into the
// standard checked AOT harness.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// A resource class R whose destructor bumps R._n, wrapped in a holder class C
// with the given Main body (which returns the encoded counter).
int runResource(const std::string& mainBody) {
    return run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;");
}

}  // namespace

// §28.11 / §28.8.1: `move arr[0]` into a `using var` transfers ownership of the
// element to `x`; `x`'s scope-end Dispose disposes it EXACTLY ONCE. The array's
// scope-end Dispose must then skip the moved-from (now null) slot — no second
// dispose, no double-free. The single-element case isolates the slot-take: after
// `x`'s inner scope R._n == 1; after the array's Dispose R._n is STILL 1 (the
// slot was moved-from). Encoded <afterX>*10 + <afterArray> = 11.
// Before the fix: segfault (the array re-freed the element x already disposed).
BEZEL_TEST(move_coll_slot_28_8_1, owned_array_move_slot_disposes_once) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterX := -1; "
        "do "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  do "
        "    using var x := move a[0]; "
        "  end; "
        "  afterX := R._n; "                 // x disposed the element -> 1
        "end; "
        "return afterX * 10 + R._n;"), 11);   // array skipped the slot -> 11
}

// §28.8.1 / §28.3 / §22.3: transferring a plain-`var` owned LOCAL into an owning
// constructor consumes the local — the call-site `owned` marker announces the
// transfer. The moved-from local has no disposal obligation, so the object is
// disposed EXACTLY ONCE by its new owner (Holder), no double-dispose: at scope
// end Holder disposes it (R._n -> 1) and `b` (moved-from) disposes nothing.
// A `using var` is scope-bound and *cannot* be moved out (§28.3 — the spec's own
// "copy is using-var, cannot move out" example); the pre-migration form moved
// out of a `using var`, which the JIT harness ran only because it skips the
// ownership pass the AOT/real driver enforces.
BEZEL_TEST(move_coll_slot_28_8_1, move_usingvar_local_into_owning_ctor_disposes_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class Holder "
        "  public owned Item: R; "
        "  public constructor(owned item: R) Item := item; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  do "
        "    var b := new R(); "
        "    using var a := new Holder(owned b); "
        "  end; "
        "  return R._n; "                     // 1 = single dispose (correct)
        "end Main; end C;"), 1);
}

// §28.11: after `move arr[i]` the OTHER slots are untouched and still owned by
// the array; the array's Dispose disposes each of them EXACTLY ONCE. With 3
// elements, `move a[0]` into x: x disposes 1, the array disposes the remaining 2
// — total 3, each exactly once (no double-free of the moved slot, no leak of the
// survivors). Encoded <afterX>*10 + <total> = 13.
// Before the fix: segfault.
BEZEL_TEST(move_coll_slot_28_8_1, owned_array_move_slot_other_elements_dispose_once) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterX := -1; "
        "do "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  do "
        "    using var x := move a[0]; "
        "  end; "
        "  afterX := R._n; "                 // x disposed 1 -> 1
        "end; "
        "return afterX * 10 + R._n;"), 13);   // array disposed the other 2 -> 3
}

// §28.11: `move arr[i]` with a non-leading index leaves THAT slot moved-from and
// the rest intact. `move a[1]` into x: x disposes the middle element; the array
// disposes a[0] and a[2]. Total 3, each once. Encoded <afterX>*10 + <total> = 13.
BEZEL_TEST(move_coll_slot_28_8_1, owned_array_move_middle_slot) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterX := -1; "
        "do "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  do "
        "    using var x := move a[1]; "
        "  end; "
        "  afterX := R._n; "
        "end; "
        "return afterX * 10 + R._n;"), 13);
}

// §28.11 indexer setter on a moved-from slot: "the previous value was already
// taken out by `move`, so there is no previous value to dispose. The slot
// transitions directly from moved-from to holding the new value." After
// `move a[0]` (x owns the old element) we restore the slot with a fresh R; the
// fresh value is NOT disposed at the set, and the OLD value is NOT re-disposed
// (it left via the move). x disposes the old one; the array disposes the fresh
// one + the untouched a[1]. Encoded <afterX>*10 + <total>:
//   afterX = 1 (only x's old element gone),  total = 3 (fresh + a[1] + old).
BEZEL_TEST(move_coll_slot_28_11, owned_array_move_then_restore_slot) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterX := -1; "
        "do "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  do "
        "    using var x := move a[0]; "
        "    a[0] := move (new R()); "       // restore moved-from slot, no dispose
        "  end; "
        "  afterX := R._n; "                 // x disposed the old element -> 1
        "end; "
        "return afterX * 10 + R._n;"), 13);   // fresh + a[1] disposed -> 3 total
}

// §28.11: `move arr[i]` returns the OWNED element and member access on it works
// (it is the live object). Set a field, move it out, read the field through x.
// Field value flows through unchanged. (A value/identity check on the moved
// element, not just disposal accounting.)
BEZEL_TEST(move_coll_slot_28_8_1, owned_array_moved_element_value_preserved) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R public V: int := 0; end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  var e := a.Get(0); e.V := 42; "   // mutate the stored element (via a borrow local; §6.6/A.6 forbids a method-call lvalue base)
        "  using var x := move a[0]; "
        "  return x.V; "                      // moved element carries V=42
        "end Main; end C;"), 42);
}

// REGRESSION GUARD (§28.11): a BORROWING indexer-get `arr[i]` (no `move`) does
// NOT transfer ownership and must NOT null the slot. Reading a[0] as a borrow
// (here via .Get, which binds a borrow) leaves the array owning all elements;
// the array's Dispose disposes all 2 exactly once. If the borrow path mistakenly
// nulled the slot, the count would be 1 (a leak). Expect 2.
BEZEL_TEST(move_coll_slot_28_11, borrowing_indexer_get_does_not_take_ownership) {
    BEZEL_EXPECT_EQ(runResource(
        "var v := 0; "
        "do "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  v := a.Get(0).GetHashCode(); "    // touch a[0] as a borrow
        "end; "
        "return R._n;"), 2);
}

// REGRESSION GUARD: plain expression read of the indexer (a borrow) does not
// consume the slot either — the element is still disposed by the array. Read the
// element's field through the indexer-get, then let the array dispose both.
BEZEL_TEST(move_coll_slot_28_11, borrowing_indexer_expr_read_keeps_ownership) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public V: int := 7; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var v := -1; "
        "  do "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(move (new R())); "
        "    a.Add(move (new R())); "
        "    v := a[0].V; "                   // borrow read, slot retained
        "  end; "
        "  return v * 10 + R._n; "            // 7*10 + 2 disposed = 72
        "end Main; end C;"), 72);
}

// §28.11 / §4.9: the same slot-take semantics for an OwningDictionary value.
// `move d[k]` transfers the value to x; the dictionary's Dispose must skip the
// moved-from (now null) value. x disposes it; the dict disposes the other entry.
// Total 2, each once. Encoded <afterX>*10 + <total> = 12. Before the fix:
// segfault (the dict re-freed the value x already disposed).
BEZEL_TEST(move_coll_slot_28_11, owned_dict_move_slot_disposes_once) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterX := -1; "
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d[\"a\"] := move (new R()); "
        "  d[\"b\"] := move (new R()); "
        "  do "
        "    using var x := move d[\"a\"]; "
        "  end; "
        "  afterX := R._n; "                 // x disposed the value -> 1
        "end; "
        "return afterX * 10 + R._n;"), 12);   // dict disposed the other -> 2
}

// ===========================================================================
// #2119 / §28.11 / §28.8.1 — a `move coll[i]` take-out bound to a PLAIN `var`
// (not a `using var`) and discharged by an explicit `.Dispose()` reclaims the
// extracted element's STORAGE, not just its destructor.
//
// Root cause: the disposal-registration for a plain-`var` owned local gates on
// codegen's `unwindOwnedLocalClass`, which classifies the initializer SHAPE
// (`new C(...)`, an owned-returning / `Pop*`/`Drain` call, an arm-meet, …). It
// had NO case for a `move <IndexExpr>` initializer, so `var taken := move a[i]`
// was not recognized as an owned class local: the explicit `taken.Dispose()`
// only DISPATCHED the destructor (explicitDisposeOwnedReceiverSlot returned
// null) and never RECLAIMED the 16-byte block, and no exception-edge disposal
// was registered — the destructor ran (disposal count correct) but the storage
// LEAKED under LeakSanitizer. The `using var x := move a[i]` form above never
// exercised this: a using-var reclaims through the TYPE-keyed
// registerUsingDisposal / usingVar reclaim, which is why those stayed clean.
// The `Pop*`/`Drain()` blessed take-out forms reclaim via case (b3)
// isBuiltinOwnedTransferCall (builtin_owned_transfer_leak_1388_test.cpp) and
// were also clean — `move coll[i]` was the sole leaking blessed form.
//
// runAotChecked compiles with generated-code ASan and runs under LSan, so a
// leaked block FAILS the run (ran=false -> -999) — the value assertion and the
// leak/double-free verdict ride the same check. Before the fix these programs
// leak 16 bytes (LSan) while returning the correct disposal count; after, they
// are leak-clean AND disposal-correct.

// §28.11 (#2119): the exact issue repro — `var taken := move a[0]` (PLAIN var)
// then `taken.Dispose()`. `taken` disposes the taken element (R._n -> 1); the
// array's scope-end cascade disposes the untouched a[1] (-> 2). Encoded
// <afterTake>*10 + <total> = 12. Pre-fix: exit 12 BUT a 16-byte LSan leak of the
// taken element's storage → runAotChecked reports ran=false.
BEZEL_TEST(move_coll_slot_leak_2119, plainvar_move_array_slot_dispose_reclaims_no_leak) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterTake := -1; "
        "do "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  var taken := move a[0]; "          // PLAIN var (the #2119 gap)
        "  taken.Dispose(); "                 // reclaims the block, not just the dtor
        "  afterTake := R._n; "               // 1
        "end; "                                // cascade disposes a[1] -> 2
        "return afterTake * 10 + R._n;"), 12);
}

// §28.11 / §4.9 (#2119): the same plain-`var` + explicit-`.Dispose()` take-out
// on an OwningDictionary VALUE (`var taken := move d[k]`). The dictionary's
// value slot is the owned thing; the same leak and the same fix apply. Pre-fix:
// 16-byte LSan leak of the taken value's storage.
BEZEL_TEST(move_coll_slot_leak_2119, plainvar_move_dict_value_dispose_reclaims_no_leak) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterTake := -1; "
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d[\"a\"] := move (new R()); "
        "  d[\"b\"] := move (new R()); "
        "  var taken := move d[\"a\"]; "       // PLAIN var
        "  taken.Dispose(); "
        "  afterTake := R._n; "               // 1
        "end; "                                // cascade disposes d[\"b\"] -> 2
        "return afterTake * 10 + R._n;"), 12);
}

// §28.3 / §28.11 (#2119, unwind edge): a plain-`var` `move a[i]` local discharged
// by an explicit `.Dispose()` on the NORMAL path, with an exception thrown
// BEFORE the dispose is reached. Because `taken` is discharged on some path it is
// NOT auto-drop-flagged, so its scope-end coverage is the exception-edge-only
// `unwindOnly` disposal registered via unwindOwnedLocalClass — the same code the
// fix taught to recognize the `move <IndexExpr>` shape. On the throwing path the
// unwind disposes `taken` (its slot still live) exactly once, and the array's
// cascade disposes the untouched a[1] — both destructors run once, no leak and
// no double-free of the moved-from slot 0. R._n after the catch == 2.
BEZEL_TEST(move_coll_slot_leak_2119, plainvar_move_slot_unwind_disposes_once_no_leak) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C "
        "  public static method Sink(b: bool): int "
        "    if b then throw new InvalidOperationException(\"x\"); end; "
        "    return 0; "
        "  end Sink; "
        "  public static method Run(): int "
        "    do "
        "      using var a := new OwningArray<R>(); "
        "      a.Add(move (new R())); "
        "      a.Add(move (new R())); "
        "      var taken := move a[0]; "        // plain var, discharged below
        "      var z := Sink(true); "           // throws -> unwind before Dispose
        "      taken.Dispose(); "               // normal-path discharge (unreached)
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    var ok := 0; "
        "    try "
        "      var r := Run(); "
        "    catch InvalidOperationException => ok := R._n; "  // both disposed -> 2
        "    end; "
        "    return ok; "
        "  end Main; end C;"), 2);
}
