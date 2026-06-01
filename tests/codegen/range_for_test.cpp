// Codegen slice CG51 — `for var i in lo..hi do … end` over range literals
// (§16.5).
//
// §16.5 explicitly permits lowering a range `for` to a counted loop that
// bypasses the IEnumerable protocol, provided observable behaviour is
// identical. This slice implements that for closed, step-less range literals.
//
// §16.6: a range whose lower bound exceeds its upper bound is empty; iterating
// it is a no-op.
//
// `..`  is inclusive (§16.1): for var i in 1..3  visits 1, 2, 3.
// `..<` is half-open (§16.1): for var i in 0..<4 visits 0, 1, 2, 3.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Inclusive `..`: sum 1+2+3 = 6.
BEZEL_TEST(codegen_cg51, inclusive_range_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := 0; for var i in 1..3 do s := s + i; end; return s; "
        "end Main; end C;"), 6);
}

// Half-open `..<`: sum 0+1+2+3 = 6 (4 is excluded).
BEZEL_TEST(codegen_cg51, half_open_range_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := 0; for var i in 0..<4 do s := s + i; end; return s; "
        "end Main; end C;"), 6);
}

// Iteration count: 1..5 visits 5 elements.
BEZEL_TEST(codegen_cg51, iteration_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 0; for var i in 1..5 do n := n + 1; end; return n; "
        "end Main; end C;"), 5);
}

// §16.6: empty range (lo > hi) is a no-op.
BEZEL_TEST(codegen_cg51, empty_range_noop) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := 0; for var i in 5..1 do s := s + 1; end; return s; "
        "end Main; end C;"), 0);
}

// `break` exits the loop early.
BEZEL_TEST(codegen_cg51, break_exits_loop) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := 0; "
        "  for var i in 1..100 do "
        "    if i > 3 then break; end; "
        "    s := s + i; "
        "  end; "
        "  return s; "
        "end Main; end C;"), 6);
}

// `continue` skips iterations (skip i=3, sum 1+2+4+5=12).
BEZEL_TEST(codegen_cg51, continue_skips_iteration) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := 0; "
        "  for var i in 1..5 do "
        "    if i = 3 then continue; end; "
        "    s := s + i; "
        "  end; "
        "  return s; "
        "end Main; end C;"), 12);
}

// Nested loops: 3 × 3 = 9 iterations.
BEZEL_TEST(codegen_cg51, nested_loops) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 0; "
        "  for var i in 1..3 do "
        "    for var j in 1..3 do "
        "      n := n + 1; "
        "    end; "
        "  end; "
        "  return n; "
        "end Main; end C;"), 9);
}

// Variable upper bound: sum 1+2+3+4+5 = 15.
BEZEL_TEST(codegen_cg51, variable_upper_bound) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 5; var s := 0; "
        "  for var i in 1..n do s := s + i; end; "
        "  return s; "
        "end Main; end C;"), 15);
}

// Loop variable is observable: last value of i after 10..13 is 13.
BEZEL_TEST(codegen_cg51, loop_variable_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var last := 0; "
        "  for var i in 10..13 do last := i; end; "
        "  return last; "
        "end Main; end C;"), 13);
}

// CG51b (§16.4): stride — `for var i in lo..hi step k do … end`.
// A positive step ascends; a negative step descends. A non-constant or
// zero step is deferred (falls back to the not-yet-implemented path).

// Ascending step 2: 0, 2, 4, 6, 8, 10 → sum = 30.
BEZEL_TEST(codegen_cg51b, ascending_step_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := 0; for var i in 0..10 step 2 do s := s + i; end; return s; "
        "end Main; end C;"), 30);
}

// Half-open with step 2: 0, 2, 4, 6, 8 (10 excluded) → sum = 20.
BEZEL_TEST(codegen_cg51b, half_open_step_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := 0; for var i in 0..<10 step 2 do s := s + i; end; return s; "
        "end Main; end C;"), 20);
}

// Descending step -1: 5, 4, 3, 2, 1 → sum = 15.
BEZEL_TEST(codegen_cg51b, descending_step_neg1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := 0; for var i in 5..1 step -1 do s := s + i; end; return s; "
        "end Main; end C;"), 15);
}

// Descending, count only: 10, 8, 6, 4, 2 → 5 iterations.
BEZEL_TEST(codegen_cg51b, descending_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 0; for var i in 10..2 step -2 do n := n + 1; end; return n; "
        "end Main; end C;"), 5);
}

// Step 3: 0, 3, 6, 9 → sum = 18.
BEZEL_TEST(codegen_cg51b, step_3_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := 0; for var i in 0..10 step 3 do s := s + i; end; return s; "
        "end Main; end C;"), 18);
}

// Step with break: ascend by 3 until i > 6, sum 0+3+6 = 9.
BEZEL_TEST(codegen_cg51b, step_with_break) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := 0; "
        "  for var i in 0..100 step 3 do "
        "    if i > 6 then break; end; "
        "    s := s + i; "
        "  end; "
        "  return s; "
        "end Main; end C;"), 9);
}
