// fix #823 / §27.5: a closure capturing a loop-body local read a STALE
// (first-iteration) value when nested control flow (an inner loop / while /
// if-then) split the loop body into multiple LLVM basic blocks. boxLocalSlot
// decided "fresh cell each iteration" by scanning for the slot's init store in
// the CURRENT basic block only; with nested control flow the init store sits in
// an earlier body-entry block, so it was missed and a single first-iteration
// cell was reused. Fix: a local declared inside a loop body is recorded and
// always boxed fresh-per-iteration.
//
// Value tests — runJit returns the raw int.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
std::string mainOf(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}
}  // namespace

// §27.5: a NESTED for-loop splits the body's basic blocks. Each iteration's
// lambda must capture that iteration's `s`. 1+2+3 = 6 (was 3).
BEZEL_TEST(closure_loop_capture_823, nested_for_loop) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var total := 0; "
        "for var i in 1..3 do "
        "  var s := i; "
        "  for var j in 1..1 do end; "
        "  var f := lambda() => s; "
        "  total := total + f(); "
        "end; "
        "return total;")), 6);
}

// §27.5: an if-then inside the loop body also splits basic blocks. 6 (was 3).
BEZEL_TEST(closure_loop_capture_823, nested_if_then) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var total := 0; "
        "for var i in 1..3 do "
        "  var s := i; "
        "  if i > 0 then var z := 1; end; "
        "  var f := lambda() => s; "
        "  total := total + f(); "
        "end; "
        "return total;")), 6);
}

// §27.5: a while loop inside the body. 6 (was 3).
BEZEL_TEST(closure_loop_capture_823, nested_while) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var total := 0; "
        "for var i in 1..3 do "
        "  var s := i; "
        "  var k := 0; "
        "  while k < 1 do k := k + 1; end; "
        "  var f := lambda() => s; "
        "  total := total + f(); "
        "end; "
        "return total;")), 6);
}

// Regression: the simple (no nested control flow) loop-body capture still works.
BEZEL_TEST(closure_loop_capture_823, simple_loop_capture_unaffected) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var total := 0; "
        "for var i in 1..3 do "
        "  var s := i; "
        "  var f := lambda() => s; "
        "  total := total + f(); "
        "end; "
        "return total;")), 6);
}

// Regression: a local declared OUTSIDE the loop and captured before it keeps a
// single SHARED cell — the closure observes the accumulated value, not a fresh
// per-iteration snapshot. acc accumulates to 6; add() reads it. 6.
BEZEL_TEST(closure_loop_capture_823, accumulator_outside_loop_shares_cell) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var acc := 0; "
        "var add := lambda() => acc; "
        "for var i in 1..3 do "
        "  acc := acc + i; "
        "end; "
        "return add();")), 6);
}
