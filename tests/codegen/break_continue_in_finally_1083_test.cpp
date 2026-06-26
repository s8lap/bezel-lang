// Regression tests for #1083 (§31.2): break/continue of a loop nested INSIDE a
// try-with-finally must affect only the loop and leave the rest of the try body
// to run, with the finally executing once when the try exits. The break/continue
// lowering routed EVERY break/continue inside a try-with-finally through the
// finally (storing an exit mode and jumping to the finally block); for a loop
// opened inside the try body the finally's exit-mode dispatcher had no case for
// that mode (its loop targets were captured at try-entry, when the loop did not
// yet exist), so the break fell through past the try — skipping post-loop code
// (wrong value) — and, once the dispatcher did jump back into the try, re-ran
// the finally (an infinite loop in a naive fix). The fix records the loopStack
// depth at try-entry and only routes a break/continue through the finally when
// it actually EXITS the try (the target loop predates the try); an intra-try
// loop break/continue jumps straight to the loop target.
//
// runJit returns the raw exit code (0..255, 8-bit wrap).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1083: a `break` of a while loop INSIDE try/finally exits only the loop; the
// post-loop statement runs, then the finally runs once.
BEZEL_TEST(break_continue_in_finally_1083, break_loop_inside_try_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static _s: int := 0; "
        "  public static method Main(): int "
        "    try "
        "      _s := 1; "
        "      var i := 0; "
        "      while i < 3 do i := i + 1; if i = 2 then break; end; end; "
        "      _s := 2; "                         // must run (break stayed in try)
        "    finally _s := _s + 10; end; "        // runs once → +10
        "    return _s; "                          // 2 + 10 = 12
        "  end Main; end C;"), 12);
}

// #1083: a `continue` of a loop inside try/finally re-enters the loop each time;
// the loop completes, post-loop code runs, finally runs once.
BEZEL_TEST(break_continue_in_finally_1083, continue_loop_inside_try_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var after := 0; "
        "  try "
        "    var i := 0; "
        "    while i < 3 do i := i + 1; continue; end; "
        "    after := 7; "                         // runs after loop completes
        "  finally after := after + 100; end; "    // runs once → +100
        "  return after; "                          // 7 + 100 = 107
        "end Main; end C;"), 107);
}

// A `break` that EXITS the try (the loop CONTAINS the try) must still run the
// finally on the way out — preserved behaviour.
BEZEL_TEST(break_continue_in_finally_1083, break_exiting_try_runs_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 0; "
        "  var i := 0; "
        "  while i < 5 do "
        "    i := i + 1; "
        "    try "
        "      if i = 3 then break; end; "          // exits the try (and loop)
        "      n := n + i; "
        "    finally n := n + 10; end; "            // runs every iteration
        "  end; "
        "  return n; "                               // i1:1+10=11, i2:+2+10=23, i3:break+10=33
        "end Main; end C;"), 33);
}

// Normal completion (no break/continue): the finally runs exactly once and the
// whole try body executes.
BEZEL_TEST(break_continue_in_finally_1083, normal_completion_finally_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 0; "
        "  try "
        "    var i := 0; "
        "    while i < 3 do i := i + 1; x := x + i; end; "  // 1+2+3 = 6
        "    x := x + 20; "                                  // 26
        "  finally x := x + 100; end; "                      // 126
        "  return x; "
        "end Main; end C;"), 126);
}

// A `return` from inside a loop inside try/finally still routes through the
// finally (exit mode 1 unaffected).
BEZEL_TEST(break_continue_in_finally_1083, return_from_loop_in_try_runs_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static _s: int := 0; "
        "  public static method Run(): int "
        "    try "
        "      var i := 0; "
        "      while i < 5 do i := i + 1; if i = 2 then return 7; end; end; "
        "      return 0; "
        "    finally _s := 9; end; "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := Run(); "
        "    return r + _s; "                          // 7 (return) + 9 (finally ran) = 16
        "  end Main; end C;"), 16);
}
