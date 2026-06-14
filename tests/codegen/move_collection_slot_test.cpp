// Codegen — `move coll[i]` slot-take from an owning collection (issue #409,
// language spec §28.8.1 / §28.11, stdlib §4.5 / §4.9).
//
// Spec §28.11 ("`move` on a collection slot"): "`move coll[i]` reads the i-th
// element of an owning collection as an owned value, leaving the slot in
// moved-from state." Spec §28.8.1: the moved-from slot must be left in a state
// the owner's later disposal skips. Spec §28.11's restoration rules and the
// OwnedArray reference implementation (stdlib §4.5) make the moved-from slot
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

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
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
        "  using var a := new OwnedArray<R>(); "
        "  a.Add(move (new R())); "
        "  do "
        "    using var x := move a[0]; "
        "  end; "
        "  afterX := R._n; "                 // x disposed the element -> 1
        "end; "
        "return afterX * 10 + R._n;"), 11);   // array skipped the slot -> 11
}

// §28.8.1 / §28.3: `move` of a `using var` LOCAL into an owning constructor
// transfers ownership; the moved-from local's scope-end Dispose must SKIP (its
// slot is nulled at the move) so the object is disposed EXACTLY ONCE by its new
// owner — no double-dispose. Holder owns the moved-in R; at scope end Holder
// disposes it (R._n -> 1) and `b` (moved-from) disposes nothing.
// Before the fix: segfault (b re-freed the object already disposed by Holder).
BEZEL_TEST(move_coll_slot_28_8_1, move_usingvar_local_into_owning_ctor_disposes_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class Holder "
        "  public owned Item: R; "
        "  public constructor(owned item: R) Item := move item; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  do "
        "    using var b := new R(); "
        "    using var a := new Holder(move b); "
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
        "  using var a := new OwnedArray<R>(); "
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
        "  using var a := new OwnedArray<R>(); "
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
        "  using var a := new OwnedArray<R>(); "
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
        "  using var a := new OwnedArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Get(0).V := 42; "               // mutate the stored element
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
        "  using var a := new OwnedArray<R>(); "
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
        "    using var a := new OwnedArray<R>(); "
        "    a.Add(move (new R())); "
        "    a.Add(move (new R())); "
        "    v := a[0].V; "                   // borrow read, slot retained
        "  end; "
        "  return v * 10 + R._n; "            // 7*10 + 2 disposed = 72
        "end Main; end C;"), 72);
}

// §28.11 / §4.9: the same slot-take semantics for an OwnedDictionary value.
// `move d[k]` transfers the value to x; the dictionary's Dispose must skip the
// moved-from (now null) value. x disposes it; the dict disposes the other entry.
// Total 2, each once. Encoded <afterX>*10 + <total> = 12. Before the fix:
// segfault (the dict re-freed the value x already disposed).
BEZEL_TEST(move_coll_slot_28_11, owned_dict_move_slot_disposes_once) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterX := -1; "
        "do "
        "  using var d := new OwnedDictionary<string, R>(); "
        "  d.Set(\"a\", move (new R())); "
        "  d.Set(\"b\", move (new R())); "
        "  do "
        "    using var x := move d[\"a\"]; "
        "  end; "
        "  afterX := R._n; "                 // x disposed the value -> 1
        "end; "
        "return afterX * 10 + R._n;"), 12);   // dict disposed the other -> 2
}
