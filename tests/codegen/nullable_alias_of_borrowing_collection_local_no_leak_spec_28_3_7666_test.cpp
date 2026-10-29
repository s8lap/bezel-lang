// Spec-derived regression tests for issue #7666 (v0.350): a TYPED same-head
// alias of a borrowing collection local --- `var n: Array<int>? := a;` --- made
// the source's scope-end reclaim vanish, so `n?.Sum()` / `n?.Max()` (and every
// other use of the alias) leaked the array's backing (144 B / 2 allocs per
// pass).
//
// Spec: §28.3.1 (v0.350) K1.5 "an unmarked read of an owned binding is a
// borrow of the binding" and K3.13 "an unmarked read of a class-typed binding
// never consumes: `var b := a;` is always a borrow". The alias therefore holds
// no obligation, the source stays the sole owner, and its backing is reclaimed
// exactly once at the source's own scope end (§28.9 / §28.11); §17.8 `x!`
// preserves ownership/borrow status, so `n!` is a borrow too.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp, `collectSafeViewAliases`).
// The only aliases the escape walk accepted as non-escaping borrows were an
// IEnumerable-family / Object / read-only-interface annotation. An alias
// annotated with the SOURCE'S OWN head (`Array<int>?`, `Array<int>`,
// `Dictionary<K,V>?`, `Set<T>?`) was not admitted, so the walker read the
// binding as an escape of `a` and `a` never entered
// `borrowingArrayFreeEligible_`. The fix admits the same-head alias to the
// candidate views; the per-view escape walk still proves the alias itself
// never escapes, so a returned / stored / captured alias keeps the source
// un-reclaimed (the leak-not-UAF direction).
//
// Every row is a 40-iteration loop (a one-shot repro is vacuous under LSan: the
// last iteration's pointer is still reachable from the stack) run through
// runAotChecked (AOT + ASan/LSan). The 8-bit exit status bounds each answer.

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free makes r.ran false -> -999, distinct
// from every legal exit value here.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

std::string loop40(const std::string& body) {
    return "module M; "
           "class C public static method Main(): int "
           "  var sum: int := 0; "
           "  for var i in 0..<40 do " + body + " end for; "
           "  return sum; "
           "end Main; end C;";
}

}  // namespace

// §28.3.1 K3.13 (#7666): the issue's repro e02 --- `n?.Sum()` over a nullable
// alias of a non-nullable Array local. 40 x 6 = 240. Pre-fix: -999 (LSan).
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           issue_repro_array_sum_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var n: Array<int>? := a; "
        "sum := sum + (n?.Sum() ?? -1); ")), 240);
}

// §28.3.1 K3.13 (#7666): the issue's repro e04 --- `n?.Max()`. 40 x 3 = 120.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           issue_repro_array_max_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var n: Array<int>? := a; "
        "sum := sum + (n?.Max() ?? -1); ")), 120);
}

// §28.3.1 K3.13 / §17.8 (#7666): `?.Count` through the nullable alias.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           array_safe_count_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var n: Array<int>? := a; "
        "sum := sum + (n?.Count ?? -1); ")), 120);
}

// §17.8 (#7666): `n!.Count` --- the null-forgiving spelling preserves the
// borrow, so it is no different from the bare alias.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           array_bang_count_spec_17_8) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var n: Array<int>? := a; "
        "sum := sum + n!.Count; ")), 120);
}

// §28.3.1 K3.13 (#7666): the nested `if n <> null` narrowing spelling.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           array_nested_null_check_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var n: Array<int>? := a; "
        "if n <> null then "
        "  if n.Count > 1 then sum := sum + n.Count; end if; "
        "end if; ")), 120);
}

// §28.3.1 K3.13 + §29.2 (#7666): for-in over `n!`.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           array_for_in_bang_spec_29_2) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var n: Array<int>? := a; "
        "for var x in n! do sum := sum + x; end for; ")), 240);
}

// §28.3.1 K3.13 (#7666): the alias of a NULLABLE source (`a: Array<int>?`),
// read through `?.Sum()` and a for-in over `n!`.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           nullable_source_sum_and_for_in_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int>? := [1,2,3]; "
        "var n: Array<int>? := a; "
        "sum := sum + (n?.Sum() ?? -1); "
        "for var x in n! do sum := sum - x; end for; "
        "sum := sum + 1; ")), 40);
}

// §28.3.1 K3.13 (#7666): the NON-nullable same-head spelling
// (`var b: Array<int> := a;`) is the same borrow.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           non_nullable_typed_alias_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var b: Array<int> := a; "
        "sum := sum + b.Sum(); ")), 240);
}

// §28.3.1 K3.13 (#7666) matrix: Dictionary (`?.Count` and a for-in over `n!`).
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           dictionary_count_and_for_in_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var d: Dictionary<int, int> := [1: 1, 2: 1]; "
        "var n: Dictionary<int, int>? := d; "
        "sum := sum + (n?.Count ?? -1); "
        "for var e in n! do sum := sum + e.Value; end for; ")), 160);
}

// §28.3.1 K3.13 (#7666) matrix: Set.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           set_count_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var s: Set<int> := [1,2,3]; "
        "var n: Set<int>? := s; "
        "sum := sum + (n?.Count ?? -1); ")), 120);
}

// CONTROL (#7666): the original is read AFTER the alias is last used. The
// source stays the owner; the backing is freed once, at the source's scope end.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           control_original_used_after_alias_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var n: Array<int>? := a; "
        "sum := sum + (n?.Count ?? 0); "
        "sum := sum + a.Sum(); ")), 360 % 256);
}

// CONTROL (#7666): the alias OUTLIVES the original's last use (`a` is never
// read after the binding). Both die at the same scope end; the backing is
// freed once, by the source.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           control_alias_outlives_original_last_use_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var n: Array<int>? := a; "
        "sum := sum + 1; "
        "sum := sum + (n?.Count ?? 0); "
        "sum := sum + (n?.Max() ?? 0); ")), 280 % 256);
}

// CONTROL (#7666): no alias at all --- the pre-existing reclaim, unchanged.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           control_no_alias_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "sum := sum + a.Count; ")), 120);
}

// CONTROL (#7666): an OWNED collection of owned class elements (`new
// OwningArray<Item>()`, a different ownership class from the borrowing
// literal local) with an unmarked alias, then the original moved into an owned
// sink. The fix must not reclaim it a second time (no double-free): the
// alias is a borrow and `move a` transfers the obligation.
BEZEL_TEST(nullable_alias_borrowing_collection_no_leak_7666,
           control_owning_array_alias_then_move_spec_28_3_1_k3_13) {
    std::string src =
        "module M; "
        "class Item public constructor() end constructor; end Item; "
        "class C "
        "  public static method Take(owned xs: OwningArray<Item>): int "
        "    using var t := move xs; return t.Count; "
        "  end Take; "
        "  public static method Main(): int "
        "    var sum: int := 0; "
        "    for var i in 0..<40 do "
        "      var a := new OwningArray<Item>(); "
        "      a.Add(new Item()); "
        "      var n: OwningArray<Item>? := a; "
        "      sum := sum + (n?.Count ?? 0); "
        "      sum := sum + C.Take(move a); "
        "    end for; "
        "    return sum; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 80);
}
