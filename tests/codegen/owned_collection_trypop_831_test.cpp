// fix #831 / stdlib §4.5 / §4.7 / §4.9: the TRY-pop family on owning
// collections was not lowered, so the calls either emitted a non-i1 value used
// as a branch condition (malformed IR — OwningArray/OwningLinkedList
// TryPopFirst/TryPopLast) or returned an always-false bool with no removal
// (OwningDictionary TryPopValue).
//
//   OwningArray<T>.TryPopFirst / TryPopLast (out owned item): bool
//   OwningLinkedList<T>.TryPopFirst / TryPopLast (out owned item): bool
//   OwningDictionary<K,V>.TryPopValue (key, out owned value): bool
//
// Each writes the element to the out param and removes it WITHOUT disposal
// (ownership transfers to the out param), returning true; on empty / absent it
// returns false and leaves the out param unchanged.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// OwningArray.TryPopFirst on a non-empty array returns true and writes the first
// element (was malformed IR: br on a non-i1 value).
// Migrated from OwningArray<int> to OwningArray<Box> per stdlib §4.2/#1327:
// owned collections require class element types.
// Uses `out owned var item` pattern (§28.2/#889); the sema fix (§4.5/#831)
// ensures `item` gets type Box from OwningArray<Box>'s generic arg — previously
// OwningArray was missing from the builtin-Try*-out-element-type inference in
// ExprTypecheck_Exprs.cpp, so item.Val returned 0.
BEZEL_TEST(owned_collection_trypop_831, array_try_pop_first_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  do using var a := new OwningArray<Box>(); "
        "    a.Add(move (new Box(10))); a.Add(move (new Box(20))); "
        "    var ok := a.TryPopFirst(out var item); "
        "    var v := 0; if item <> null then v := item.Val; end; "
        "    item?.Dispose(); "
        "    if ok then return v; end; return 0; end; return 0; "
        "end Main; end C;"), 10);
}

// OwningArray.TryPopLast writes the last element.
// Uses `out owned var item` pattern (§28.2/#889); see array_try_pop_first_true.
BEZEL_TEST(owned_collection_trypop_831, array_try_pop_last_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  do using var a := new OwningArray<Box>(); "
        "    a.Add(move (new Box(10))); a.Add(move (new Box(20))); "
        "    var ok := a.TryPopLast(out var item); "
        "    var v := 0; if item <> null then v := item.Val; end; "
        "    item?.Dispose(); "
        "    if ok then return v; end; return 0; end; return 0; "
        "end Main; end C;"), 20);
}

// OwningArray.TryPopFirst on an empty array returns false and leaves the out
// param unchanged (item stays null).
BEZEL_TEST(owned_collection_trypop_831, array_try_pop_empty_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public Val: int := 0; end Box; "
        "class C public static method Main(): int "
        "  do using var a := new OwningArray<Box>(); "
        "    var item: Box? := null; var ok := a.TryPopFirst(out item); "
        "    if ok then return 1; end; "
        "    if item = null then return 7; end; return 0; end; return 0; "
        "end Main; end C;"), 7);
}

// OwningLinkedList.TryPopFirst on a non-empty list returns true and writes the
// first element. Uses `out owned var item` pattern (§28.2/#889).
BEZEL_TEST(owned_collection_trypop_831, linkedlist_try_pop_first_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  do using var l := new OwningLinkedList<Box>(); "
        "    l.AddLast(move (new Box(7))); "
        "    var ok := l.TryPopFirst(out var item); "
        "    var v := 0; if item <> null then v := item.Val; end; "
        "    item?.Dispose(); "
        "    if ok then return v; end; return 0; end; return 0; "
        "end Main; end C;"), 7);
}

// OwningLinkedList.TryPopLast on an empty list returns false (out unchanged).
BEZEL_TEST(owned_collection_trypop_831, linkedlist_try_pop_empty_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public Val: int := 0; end Box; "
        "class C public static method Main(): int "
        "  do using var l := new OwningLinkedList<Box>(); "
        "    var item: Box? := null; var ok := l.TryPopLast(out item); "
        "    if ok then return 1; end; "
        "    if item = null then return 9; end; return 0; end; return 0; "
        "end Main; end C;"), 9);
}

// OwningDictionary.TryPopValue on a present key returns true and writes the
// owned value (was always false → the `return 3` miss path).
BEZEL_TEST(owned_collection_trypop_831, dict_try_pop_value_hit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  do using var d := new OwningDictionary<string, R>(); "
        "    d.Add(\"a\", move (new R())); var v: R? := null; "
        "    var hit := d.TryPopValue(\"a\", out v); "
        "    if hit then if v <> null then "
        "      do using var _ := v; end; return 1; end; return 2; end; "
        "    return 3; end; return 0; "
        "end Main; end C;"), 1);
}

// OwningDictionary.TryPopValue on an absent key returns false (out unchanged).
BEZEL_TEST(owned_collection_trypop_831, dict_try_pop_value_miss) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  do using var d := new OwningDictionary<string, R>(); "
        "    d.Add(\"a\", move (new R())); var v: R? := null; "
        "    var hit := d.TryPopValue(\"zzz\", out v); "
        "    if hit then return 1; end; return 3; end; return 0; "
        "end Main; end C;"), 3);
}

// OwningDictionary TryPopValue returns the value via out; verify the Val field.
// Migrated from OwningDictionary<string,int> → OwningDictionary<string,Box> per
// stdlib §4.2/#1327 (value types cannot be owned collection elements).
// Uses `out owned var v` pattern (§28.2/#889) and accesses Val before disposal.
BEZEL_TEST(owned_collection_trypop_831, dict_try_pop_value_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  do using var d := new OwningDictionary<string, Box>(); "
        "    d.Add(\"a\", move (new Box(42))); "
        "    var hit := d.TryPopValue(\"a\", out var v); "
        "    var result := 0; if hit then if v <> null then result := v.Val; end; end; "
        "    v?.Dispose(); "
        "    return result; end; return 0; "
        "end Main; end C;"), 42);
}
