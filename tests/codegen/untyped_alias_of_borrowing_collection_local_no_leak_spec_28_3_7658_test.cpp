// Spec-derived regression tests for issue #7658 (v0.350): an UNANNOTATED alias
// of a borrowing collection local --- `var b := a;` --- made the source's
// scope-end reclaim vanish, so `var b := a; for var x in b ...` over an Array
// local (nullable or not, with or without `!`) leaked the array's backing
// (144 B / 2 allocs per pass).
//
// Spec: §28.3.1 (v0.350) K1.5 "an unmarked read of an owned binding is a
// borrow of the binding" and K3.13 "`var b := a;` is always a borrow for
// class-typed `a`" (the same sentence the section's own `var b := a;` example
// uses); §29.2 for-in over a borrowed collection; §17.8 `x!` preserves the
// borrow. The alias holds no obligation, so the backing is reclaimed exactly
// once, by the source, at its own scope end (§28.9 / §28.11).
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp, `collectSafeViewAliases`).
// The view scan began with `if (v.init && !v.isUsing && v.type)`: it only
// looked at a binding that carried an explicit type annotation, so an inferred
// `var b := a;` was never a candidate safe alias, the escape walk read it as an
// escape of `a`, and `a` never entered `borrowingArrayFreeEligible_`. This is
// the same cause as #7666's typed spelling; the fix admits the unannotated
// binding with the type inferred from the source (the same-head test is
// unchanged, and the per-view escape walk still proves the alias itself never
// escapes).
//
// Every row is a 40-iteration loop (one-shot LSan runs are vacuous) through
// runAotChecked (AOT + ASan/LSan). The 8-bit exit status bounds each answer.

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

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

// §28.3.1 K3.13 + §29.2 (#7658): the issue's repro --- for-in over the
// unannotated alias of a NULLABLE Array local, spelled with `!`. 40 x 6.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           issue_repro_nullable_array_for_in_bang_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int>? := [1,2,3]; "
        "var b := a; "
        "for var x in b! do sum := sum + x; end for; ")), 240);
}

// §28.3.1 K3.13 + §29.2 (#7658): the same without the `!` (a for-in over a
// nullable collection reads its non-null value).
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           issue_repro_nullable_array_for_in_no_bang_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int>? := [1,2,3]; "
        "var b := a; "
        "for var x in b do sum := sum + x; end for; ")), 240);
}

// §28.3.1 K3.13 + §29.2 (#7658): the NON-nullable source and alias.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           non_nullable_array_for_in_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var b := a; "
        "for var x in b do sum := sum + x; end for; ")), 240);
}

// §28.3.1 K3.13 (#7658): nullable source, `?.Sum()` / `?.Max()` / `?.Count`
// through the unannotated alias.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           nullable_source_sum_max_count_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int>? := [1,2,3]; "
        "var b := a; "
        "sum := sum + (b?.Sum() ?? -1) + (b?.Max() ?? -1) + (b?.Count ?? -1); ")),
        480 % 256);
}

// §28.3.1 K3.13 (#7658): the nested `if a <> null` spelling, aliasing inside the
// branch.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           nested_null_check_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int>? := [1,2,3]; "
        "if a <> null then "
        "  var b := a; "
        "  for var x in b! do sum := sum + x; end for; "
        "end if; ")), 240);
}

// §28.3.1 K3.13 (#7658) matrix: Dictionary --- `.Count` and a for-in over the
// unannotated alias.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           dictionary_count_and_for_in_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var d: Dictionary<int, int> := [1: 1, 2: 1]; "
        "var b := d; "
        "sum := sum + b.Count; "
        "for var e in b do sum := sum + e.Value; end for; ")), 160);
}

// §28.3.1 K3.13 (#7658) matrix: nullable Dictionary source.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           nullable_dictionary_for_in_bang_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var d: Dictionary<int, int>? := [1: 1, 2: 1]; "
        "var b := d; "
        "for var e in b! do sum := sum + e.Value; end for; ")), 80);
}

// §28.3.1 K3.13 (#7658) matrix: Set.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           set_for_in_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var s: Set<int> := [1,2,3]; "
        "var b := s; "
        "for var x in b do sum := sum + 1; end for; ")), 120);
}

// CONTROL (#7658): the alias is declared and never used, so the source is the
// sole user (the alias must still not extend or steal the backing).
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           control_unused_alias_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var b := a; "
        "sum := sum + a.Count; ")), 120);
}

// CONTROL (#7658): the original is used AFTER the alias's last use.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           control_original_used_after_alias_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var b := a; "
        "for var x in b do sum := sum + x; end for; "
        "sum := sum + a.Count; ")), 360 % 256);
}

// CONTROL (#7658): the alias OUTLIVES the original's last use.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           control_alias_outlives_original_last_use_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "var b := a; "
        "sum := sum + 1; "
        "for var x in b do sum := sum + x; end for; ")), 280 % 256);
}

// CONTROL (#7658): an alias declared in an inner block, the source in the loop
// body --- the alias dies first.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
           control_inner_scope_alias_spec_28_3_1_k3_13) {
    BEZEL_EXPECT_EQ(runChecked(loop40(
        "var a: Array<int> := [1,2,3]; "
        "if i > -1 then "
        "  var b := a; "
        "  sum := sum + b.Count; "
        "end if; "
        "sum := sum + a.Count; ")), 240);
}

// CONTROL (#7658): an owned collection with owned class elements, alias then
// `move` of the original into an owned sink: must stay exactly-once (no
// double-free) --- the owning family is not a borrowing literal local.
BEZEL_TEST(untyped_alias_borrowing_collection_no_leak_7658,
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
        "      var b := a; "
        "      sum := sum + b.Count; "
        "      sum := sum + C.Take(move a); "
        "    end for; "
        "    return sum; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(src), 80);
}
