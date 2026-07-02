// fix #1340 (§16.3 v0.162): Range<T>.Contains and the `in` operator now require
// the range to be NONEMPTY (under its iteration direction) before testing the
// extent.  Previously a reversed stepless range like `100..0` (empty under the
// default ascending step=1) incorrectly returned true for values inside the
// min/max extent.
//
// Normative rule (v0.162): Contains(v) = !IsEmpty AND min(Start,End) <= v <=
// max(Start,End), with End excluded when half-open.  Step affects membership
// ONLY through the IsEmpty check — `0..10 step 2` still contains 3.
//
// §16.3 / §16.6 (v0.162):
//   `50 in 100..0`           → false (was true — ascending-empty, §16.6)
//   `(100..0 step -1).Contains(50)` → true (descending, nonempty)
//   `(0..10 step 2).Contains(3)`   → true (nonempty; step doesn't filter extent)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §16.3 v0.162: `50 in 100..0` — stepless literal, default step=1 (ascending).
// Start=100 > End=0 → ascending-empty → Contains must be false.
// (Was incorrectly TRUE before the fix because min/max extent included 50.)
BEZEL_TEST(range_contains_nonempty_1340, reversed_stepless_literal_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 50 in 100..0 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §16.3 v0.162: `50 in 0..100` — correct direction → still true.
// (Regression guard: ascending non-empty range must not be broken.)
BEZEL_TEST(range_contains_nonempty_1340, normal_ascending_literal_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 50 in 0..100 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §16.3 v0.162: reversed half-open `50 in 100..<0` — empty (ascending) → false.
BEZEL_TEST(range_contains_nonempty_1340, reversed_stepless_halfopen_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  if 50 in 100..<0 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §16.3 v0.162: `(100..0 step -1).Contains(50)` — descending and nonempty
// (step<0 and Start > End): Contains must be TRUE.
BEZEL_TEST(range_contains_nonempty_1340, stepped_descending_contains_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 100..0 step -1; "
        "  if r.Contains(50) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §16.3 v0.162: `50 in r` (variable) where r = `100..0 step -1` — true.
BEZEL_TEST(range_contains_nonempty_1340, stepped_descending_in_op_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 100..0 step -1; "
        "  if 50 in r then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §16.3 v0.162: `(0..10 step 2).Contains(3)` — step=2 (ascending), nonempty.
// 3 is within [0,10] even though it would not be *yielded* during iteration.
// Step affects emptiness/direction only — it does NOT filter extent membership.
BEZEL_TEST(range_contains_nonempty_1340, stepped_ascending_contains_non_yield_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 0..10 step 2; "
        "  if r.Contains(3) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §16.3 v0.162: `3 in r` variable form of `0..10 step 2` — same result.
BEZEL_TEST(range_contains_nonempty_1340, stepped_ascending_in_op_non_yield) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 0..10 step 2; "
        "  if 3 in r then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §16.3 v0.162: reversed stepless VARIABLE range (default step=1) — empty.
// `var r := 100..0` → step=1, Start=100 > End=0 → ascending-empty → false.
BEZEL_TEST(range_contains_nonempty_1340, reversed_stepless_variable_contains_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 100..0; "
        "  if r.Contains(50) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §16.3 v0.162: `50 in r` variable form of reversed stepless range — false.
BEZEL_TEST(range_contains_nonempty_1340, reversed_stepless_variable_in_op_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 100..0; "
        "  if 50 in r then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §16.6 v0.162: IsEmpty and Contains agree — an empty range contains nothing.
// `100..0` (step=1): IsEmpty=true, Contains(50)=false; both agree.
BEZEL_TEST(range_contains_nonempty_1340, isempty_agrees_contains_ascending_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 100..0; "
        "  var ie := r.IsEmpty; "
        "  var c := r.Contains(50); "
        "  if ie and not c then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §16.6 v0.162: step-mismatch range is also empty; Contains must be false.
// `0..10 step -1` → step<0 (descending), 0 < 10 → descending-empty; no value contained.
BEZEL_TEST(range_contains_nonempty_1340, isempty_agrees_contains_mismatch_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 0..10 step -1; "
        "  var ie := r.IsEmpty; "
        "  var c := r.Contains(5); "
        "  if ie and not c then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §16.3 v0.162: regression guard — normal ascending inline literals unchanged.
// `5 in 0..10` → true; `11 in 0..10` → false.
BEZEL_TEST(range_contains_nonempty_1340, ascending_normal_literals_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 1 if 5 in 0..10 else 0; "
        "  var b := 0 if 11 in 0..10 else 1; "
        "  return a + b; "
        "end Main; end C;"), 2);
}

// §16.3 v0.162: regression guard — descending stepped range (§16.3 #1092 cases).
// `10..0 step -1`: 5∈ → 1, 0∈ → 1 (inclusive), 10∈ → 1, 11∉ → 0.
BEZEL_TEST(range_contains_nonempty_1340, descending_stepped_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 10..0 step -1; "
        "  var a := 1 if 5 in r else 0; "
        "  var b := 1 if r.Contains(0) else 0; "
        "  var c := 1 if r.Contains(10) else 0; "
        "  var d := 1 if r.Contains(11) else 0; "
        "  return a + b * 2 + c * 4 + d * 8; "
        "end Main; end C;"), 7);
}

// §16.3 v0.162: half-open descending range excludes End endpoint.
// `10..<0 step -1`: 5∈ → 1, 0∉ (excluded) → 0, 10∈ → 1.
BEZEL_TEST(range_contains_nonempty_1340, descending_halfopen_excludes_end) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 10..<0 step -1; "
        "  var a := 1 if 5 in r else 0; "
        "  var b := 1 if r.Contains(0) else 0; "
        "  var c := 1 if r.Contains(10) else 0; "
        "  return a + b * 2 + c * 4; "
        "end Main; end C;"), 5);
}
