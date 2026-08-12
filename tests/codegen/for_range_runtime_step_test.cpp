// Codegen — discovery sweep #8: `for var i in lo..hi step s` with a RUNTIME
// (non-constant) step crashed with "not yet implemented: codegen for this for
// statement". lowerRangeFor's inline-range path resolved the step and bailed to
// the catch-all when it was not an LLVM ConstantInt, even though §16.4 permits
// runtime-computed steps. THE FIX emits a direction-aware loop (pick the
// ascending/descending bound test by the step's runtime sign), mirroring the
// Range-variable path. §16.4.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
using bezel::test::runAot;
namespace { int run(const std::string& p){ auto r=runAot(p); return r.ran?r.exitCode:-999; } }
// §16.4 (v0.242, #3052 — was #1623): a computed step of zero raises
// `OverflowException` **at the range expression**. (Through v0.241 it was
// `ArgumentException` at `GetEnumerator` time; the fault type and site both
// moved with the revision — see range_step_construction_3052_test.cpp for the
// rule's own tests, including the never-iterated case only the new site
// catches.) Wrap the step-0 statement in try/catch OverflowException —
// returns 0 when raised (pass), 7 otherwise (fail). Without the guard the
// counted loop never advances (induction adds 0) and hangs, so this helper
// only terminates under the fix.
namespace {
int expectsStepZeroThrow(const std::string& stmt) {
    return run(
        "module M; class C public static method Main(): int "
        "  var ok: int := 7; "
        "  try " + stmt + " "
        "  catch var e: OverflowException => do ok := 0; end "
        "  end; "
        "  return ok; "
        "end Main; end C;");
}
}

BEZEL_TEST(for_range_runtime_step, runtime_positive_step_ascends) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var sum: int := 0; var s: int := 2; "
        "  for var i in 0..10 step s do sum := sum + i; end; "  // 0+2+4+6+8+10
        "  return sum; end Main; end C;"), 30);
}
BEZEL_TEST(for_range_runtime_step, runtime_negative_step_descends) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var sum: int := 0; var s: int := -3; "
        "  for var i in 10..0 step s do sum := sum + i; end; "  // 10+7+4+1
        "  return sum; end Main; end C;"), 22);
}
// Control: a constant step still uses the (unchanged) compile-time-direction loop.
BEZEL_TEST(for_range_runtime_step, constant_step_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var sum: int := 0; "
        "  for var i in 0..10 step 2 do sum := sum + i; end; "  // 30
        "  return sum; end Main; end C;"), 30);
}

// §16.4 (v0.242): a COMPUTED step of zero faults at the range expression. In
// the inline-literal `for` form the range expression IS the loop's iterable,
// so the fault site is the loop header — the same place the pre-v0.242
// `ArgumentException` guard stood; only the exception type moved. The
// compiler-optimized counted loop (§16.5) still must carry the check, else the
// induction adds 0 and the loop never terminates.
BEZEL_TEST(for_range_runtime_step, range_step_zero_inline_raises_16_4) {
    BEZEL_EXPECT_EQ(expectsStepZeroThrow(
        "var z: int := 0; var touched: int := 0; "
        "for var i in 0..10 step z do touched := i; end;"), 0);
}

// §16.4 (v0.242): the #1623 repro — a Range VARIABLE carrying a computed
// step-0. The fault now happens at the `var r := 0..10 step z` line (the range
// EXPRESSION), so the `for` is never reached; before v0.242 it happened at the
// `for` (GetEnumerator time), and before #1623 not at all (an emitted infinite
// loop / process hang). The catch is what both revisions observe.
BEZEL_TEST(for_range_runtime_step, range_var_step_zero_raises_16_4) {
    BEZEL_EXPECT_EQ(expectsStepZeroThrow(
        "var z: int := 0; var r := 0..10 step z; var touched: int := 0; "
        "for var i in r do touched := i; end;"), 0);
}

// §16.4 control: a NONZERO computed step over a Range VARIABLE still iterates
// correctly — the new step-0 guard must not fire for step != 0. 0+2+4+6+8+10=30.
BEZEL_TEST(for_range_runtime_step, range_var_nonzero_step_iterates_16_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: int := 2; var r := 0..10 step s; var sum: int := 0; "
        "  for var i in r do sum := sum + i; end; "
        "  return sum; end Main; end C;"), 30);
}
