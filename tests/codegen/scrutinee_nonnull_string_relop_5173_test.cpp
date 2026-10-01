// Spec-derived codegen tests for fix-forward #5173-ff (Option C, per
// dc 2026-10-01) / §19.3 / §4.6 (spec v0.313 line 511, v0.313 line
// 502): a relational or range pattern over a NON-NULLABLE `string`
// scrutinee is a §4.6 ordinal lexicographic compare, NOT a pointer
// ICmp on the two `bz_string*` handles. The pre-fix code fell through
// to `CreateICmpSLT(ptr, ptr)` (the #5173 silent-wrong-code shape —
// `"z" > "a"` exited 0 instead of 1, allocating-order bug).
//
// The 5c0ea57d attempt to defer string relop to nullptr (return
// nullptr from patternMatches) was rejected by dc on the basis that
// turning a valid Bezel program into an internal compile failure
// (the existing `case s of > "b" => …` over non-null `s string`) is
// worse than silent wrong code. The fix-forward lowers the pattern
// through the same `emitStringOrdCmp` (bz_str_compare_cp) intrinsic
// the binary `<`/`<=`/`>`/`>=` path in `lowerBinary` already uses
// (#4487 fix, stdlib §9.6 / spec §4.6).
//
// runAot drives the real AOT path (fork-isolated Driver::compileFile
// + clang link + native exec) — the strict oracle. Each row asserts
// the SELECTED arm's value on stdout (the exit code), so a regression
// in the relop lowering (pointer-compare, intrinsic misuse, missing
// range-bound handling) flips a test.
//
// Pre-fix-forward every row FAILS (silent wrong code: `"a" > "b"`
// exits 2 normally, but the pre-fix pointer ICmp answers the
// opposite — random by heap-address order); post-fix forward every
// row PASSES.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int runExit(const std::string& body) {
    auto r = runAot(body);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §19.3 / §4.6 / spec v0.313 line 511 / fix #5220 / fix #5173-ff:
// `s = "a"` does NOT match `> "b"` (a is BEFORE b lexicographically)
// — selects the `_ => 2` arm.
BEZEL_TEST(scrutinee_nonnull_string_relop_5173,
           nonnull_string_gt_lower_bound_selects_catchall) {
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"a\"; "
        "  return (case s of > \"b\" => 1, _ => 2); "
        "end Main; end C;"),
        2);
}

// §19.3 / §4.6 / spec v0.313 line 511: `s = "c"` matches `> "b"` (c
// is AFTER b lexicographically) — selects arm 1.
BEZEL_TEST(scrutinee_nonnull_string_relop_5173,
           nonnull_string_gt_upper_bound_selects_match_arm) {
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"c\"; "
        "  return (case s of > \"b\" => 1, _ => 2); "
        "end Main; end C;"),
        1);
}

// §19.3 / §4.6: boundary on `>` is STRICT — `s = "b"` does NOT match
// `> "b"` and selects `_ => 2`. (Pre-fix pointer ICmp would also
// answer wrong by heap-order, so this row is necessary even though
// it equals the lower-bound row's exit.)
BEZEL_TEST(scrutinee_nonnull_string_relop_5173,
           nonnull_string_gt_boundary_is_strict_selects_catchall) {
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"b\"; "
        "  return (case s of > \"b\" => 1, _ => 2); "
        "end Main; end C;"),
        2);
}

// §19.3 / §4.6: boundary on `>=` is INCLUSIVE — `s = "b"` matches
// `>= "b"` and selects arm 1.
BEZEL_TEST(scrutinee_nonnull_string_relop_5173,
           nonnull_string_ge_boundary_is_inclusive_selects_match_arm) {
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"b\"; "
        "  return (case s of >= \"b\" => 1, _ => 2); "
        "end Main; end C;"),
        1);
}

// §19.3 / §4.6 / spec v0.313 line 511: range pattern `"a".."c"`
// (closed, both bounds inclusive) — `s = "a"`, `"b"`, `"c"` all match
// (select 1); `s = "d"` is past the upper bound (selects 2).
BEZEL_TEST(scrutinee_nonnull_string_relop_5173,
           nonnull_string_range_pattern_inclusive_bounds) {
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"a\"; "
        "  return (case s of \"a\"..\"c\" => 1, _ => 2); "
        "end Main; end C;"),
        1);
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"b\"; "
        "  return (case s of \"a\"..\"c\" => 1, _ => 2); "
        "end Main; end C;"),
        1);
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"c\"; "
        "  return (case s of \"a\"..\"c\" => 1, _ => 2); "
        "end Main; end C;"),
        1);
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"d\"; "
        "  return (case s of \"a\"..\"c\" => 1, _ => 2); "
        "end Main; end C;"),
        2);
}

// §19.3 / §4.6: lexicographic ordering — `"apple"` is BEFORE
// `"banana"` so `s = "apple"` does NOT match `> "banana"` and
// selects `_ => 2`; `"banana" > "apple"` and selects arm 1. This
// is the head-to-head probe the reviewer's handback used to
// surface the heap-address shape (the pre-fix pointer ICmp would
// compare the two `bz_string*` allocation addresses instead of
// the code-point sequences and could answer either way).
BEZEL_TEST(scrutinee_nonnull_string_relop_5173,
           nonnull_string_lexicographic_ordering_apple_banana) {
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"apple\"; "
        "  return (case s of > \"banana\" => 1, _ => 2); "
        "end Main; end C;"),
        2);
    BEZEL_EXPECT_EQ(runExit(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"banana\"; "
        "  return (case s of > \"apple\" => 1, _ => 2); "
        "end Main; end C;"),
        1);
}