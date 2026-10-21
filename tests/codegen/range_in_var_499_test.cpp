// Codegen fix #499 — Range<T> stored in a variable: `var r := a..b`
// Members and `in` operator must work when the range is not an inline literal.
//
// §9.8 (stdlib): Range<T> has Start, End, Step, IsEndInclusive, IsOpenEnded,
// IsEmpty, and Contains(T). The `in` operator (§17.5) calls Contains.
//
// §16.5 / §9.8: the compiler may optimize `for var i in rangeExpr` to a
// counted loop, and must do the same when the range is in a variable.
//
// Bug: lowerExpr(RangeExpr) had no handler — a range stored in a local
// defaulted to an i32 slot; member/`in` access on it was broken.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §17.5 / §9.8: `x in r` where r is a stored range variable — inclusive.
// 5 in 1..10 → true (return 1).
BEZEL_TEST(codegen_range_in_var_499, in_stored_inclusive_hit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..10; "
        "  if 5 in r then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5 / §9.8: 0 not in 1..10 → false (return 0).
BEZEL_TEST(codegen_range_in_var_499, in_stored_inclusive_miss) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..10; "
        "  if 0 in r then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §17.5 / §9.8: boundary — 10 in 1..10 → true (inclusive upper bound).
BEZEL_TEST(codegen_range_in_var_499, in_stored_inclusive_hi_boundary) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..10; "
        "  if 10 in r then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §17.5 / §9.8: half-open — 10 not in 1..<10 (exclusive upper bound).
BEZEL_TEST(codegen_range_in_var_499, in_stored_halfopen_excludes_hi) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..<10; "
        "  if 10 in r then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §17.5 / §9.8: half-open — 9 in 1..<10 → true.
BEZEL_TEST(codegen_range_in_var_499, in_stored_halfopen_hit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..<10; "
        "  if 9 in r then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §9.8: Contains — same semantics as `in`.
// r.Contains(5) for 1..10 → true (return 1).
BEZEL_TEST(codegen_range_in_var_499, contains_stored_hit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..10; "
        "  if r.Contains(5) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §9.8: Contains — 0 not in 1..10 → false (return 0).
BEZEL_TEST(codegen_range_in_var_499, contains_stored_miss) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..10; "
        "  if r.Contains(0) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §9.8: IsEmpty — descending / inverted range 5..1 (inclusive) is empty
// (hi < lo for ascending semantics, Step not specified).
BEZEL_TEST(codegen_range_in_var_499, is_empty_inverted_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 5..1; "
        "  var e := r.IsEmpty; "
        "  return (if e then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: IsEmpty — 1..5 (ascending, inclusive) is not empty.
BEZEL_TEST(codegen_range_in_var_499, is_empty_normal_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..5; "
        "  var e := r.IsEmpty; "
        "  return (if e then 0 else 1); "
        "end Main; end C;"), 1);
}

// §9.8: IsEmpty — half-open empty: 5..<5 (hi == lo for half-open → empty).
BEZEL_TEST(codegen_range_in_var_499, is_empty_halfopen_equal_bounds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 5..<5; "
        "  var e := r.IsEmpty; "
        "  return (if e then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: Start property — r.Start for 3..7 → 3.
BEZEL_TEST(codegen_range_in_var_499, start_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 3..7; "
        "  return r.Start!; "
        "end Main; end C;"), 3);
}

// §9.8: End property — r.End for 3..7 → 7.
BEZEL_TEST(codegen_range_in_var_499, end_property) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 3..7; "
        "  return r.End!; "
        "end Main; end C;"), 7);
}

// §9.8: IsEndInclusive — `..` is inclusive (true).
BEZEL_TEST(codegen_range_in_var_499, is_end_inclusive_dotdot) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..10; "
        "  return (if r.IsEndInclusive then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.8: IsEndInclusive — `..<` is half-open (false).
BEZEL_TEST(codegen_range_in_var_499, is_end_inclusive_halfopen) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..<10; "
        "  return (if r.IsEndInclusive then 1 else 0); "
        "end Main; end C;"), 0);
}

// §16.5 / §9.8: `for var i in r` where r is a stored range variable —
// should iterate like `for var i in 1..3` (sum 1+2+3=6).
BEZEL_TEST(codegen_range_in_var_499, for_in_stored_range_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..3; "
        "  var s := 0; "
        "  for var i in r do s := s + i; end; "
        "  return s; "
        "end Main; end C;"), 6);
}

// §16.5 / §9.8: `for var i in r` half-open (1..<4 → 1,2,3, sum=6).
BEZEL_TEST(codegen_range_in_var_499, for_in_stored_halfopen_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..<4; "
        "  var s := 0; "
        "  for var i in r do s := s + i; end; "
        "  return s; "
        "end Main; end C;"), 6);
}

// §16.5 / §9.8: empty stored range (5..1) — for loop is a no-op.
BEZEL_TEST(codegen_range_in_var_499, for_in_stored_empty_noop) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 5..1; "
        "  var s := 0; "
        "  for var i in r do s := s + 1; end; "
        "  return s; "
        "end Main; end C;"), 0);
}

// §9.8: range stored in a variable, then passed to a helper method that
// uses `x in r`. (Exercises cross-expression-boundary range storage.)
BEZEL_TEST(codegen_range_in_var_499, range_var_cross_use) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := 1..10; "
        "  var count := 0; "
        "  for var i in 1..15 do "
        "    if i in r then count := count + 1; end; "
        "  end; "
        "  return count; "
        "end Main; end C;"), 10);
}

// ── #741/#743 §16.4: direction is decided by the step's SIGN at runtime ──────
// Pre-fix, the Range-variable `for` path only derived direction from a
// compile-time-constant step; a step loaded from the struct always took the
// ascending comparison. That both threw / failed to terminate for a
// sign-mismatched ascending range and silently emptied a legitimate
// descending range. These tests pin all four bounds/step-sign combinations.

// §16.4: ascending bounds + negative step → EMPTY iteration (not an exception).
BEZEL_TEST(codegen_range_in_var_499, step_sign_mismatch_ascending_is_empty_16_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var count := 0; "
        "  var r := 1..5 step -1; "
        "  for var i in r do count := count + 1; end; "
        "  return count; "
        "end Main; end C;"), 0);
}

// §16.4: descending bounds + negative step → iterates 5,4,3,2,1 (inclusive) = 5.
// (Pre-fix this wrongly produced an empty loop because direction defaulted up.)
BEZEL_TEST(codegen_range_in_var_499, descending_negative_step_iterates_16_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var count := 0; "
        "  var r := 5..1 step -1; "
        "  for var i in r do count := count + 1; end; "
        "  return count; "
        "end Main; end C;"), 5);
}

// §16.4: descending bounds + positive step → EMPTY iteration.
BEZEL_TEST(codegen_range_in_var_499, step_sign_mismatch_descending_is_empty_16_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var count := 0; "
        "  var r := 5..1 step 1; "
        "  for var i in r do count := count + 1; end; "
        "  return count; "
        "end Main; end C;"), 0);
}

// §16.4: a Range variable agrees with the inline-literal form for the same
// sign-mismatched range (both empty), confirming path consistency.
BEZEL_TEST(codegen_range_in_var_499, literal_and_var_agree_on_descending_16_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var lit := 0; "
        "  for var i in 5..1 step -1 do lit := lit + 1; end; "
        "  var vc := 0; "
        "  var r := 5..1 step -1; "
        "  for var j in r do vc := vc + 1; end; "
        "  if lit <> vc then return 99; end; "
        "  return lit; "  // both iterate 5,4,3,2,1
        "end Main; end C;"), 5);
}
