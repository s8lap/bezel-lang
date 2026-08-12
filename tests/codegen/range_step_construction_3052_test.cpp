// Codegen end-to-end for §16.4's construction-time range-step validation
// (spec v0.242 — proposal 0086 item 1, tracked as #3052).
//
// §16.4, verbatim: "A step of zero is rejected **at construction**: a literal
// (or constant-folded) step of zero is a compile error at the range
// expression, and evaluating a range expression whose computed step is zero
// raises `OverflowException` at the expression. Range expressions are the sole
// producer of `Range<T>` values … so no `Range<T>` value with a zero step can
// exist."
//
// Two things changed at v0.242 and both are pinned here:
//   * the FAULT TYPE — `OverflowException`, not the pre-v0.242
//     `ArgumentException` (the joint checked-posture ruling: a range
//     expression is no more a method boundary than a shift or a cast, so
//     there is no `ParamName` referent);
//   * the FAULT SITE — the range EXPRESSION, not `GetEnumerator`. That is
//     strictly earlier and strictly more programs fault: a
//     constructed-but-never-iterated zero-step range now faults, which is what
//     the first test below measures and which no pre-v0.242 test could.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Wrap `stmt` in `try … catch OverflowException` — 0 when the fault is raised
// (pass), 7 otherwise (fail). A test whose loop would hang without the guard
// only terminates under the fix.
int expectsOverflow(const std::string& stmt) {
    return run(
        "  var ok: int := 7; "
        "  try " + stmt + " "
        "  catch var e: OverflowException => do ok := 0; end "
        "  end; "
        "  return ok;");
}

}  // namespace

// §16.4 (v0.242): THE new-behavior test — a range expression with a computed
// zero step faults **even though the range is never iterated**. Under the
// pre-v0.242 rule the fault lived at `GetEnumerator`, so this program ran to
// completion; the value it constructed was the zero-step `Range<T>` §16.3 now
// says "cannot exist".
BEZEL_TEST(codegen_range_step_construction_3052,
           computed_zero_step_faults_at_expression_without_iterating_spec_16_4) {
    BEZEL_EXPECT_EQ(expectsOverflow(
        "var z: int := 0; var r := 0..10 step z; var unused: int := 1;"), 0);
}

// §16.4: the same fault through the `for`-header spelling, where the range
// expression is the loop's iterable. Without a construction check the counted
// loop's induction adds 0 and the program hangs, so this only terminates
// under the fix.
BEZEL_TEST(codegen_range_step_construction_3052,
           computed_zero_step_in_for_header_faults_spec_16_4) {
    BEZEL_EXPECT_EQ(expectsOverflow(
        "var z: int := 0; var touched: int := 0; "
        "for var i in 0..10 step z do touched := i; end;"), 0);
}

// §16.4: a Range VARIABLE built from a computed zero step. The fault is at the
// `var r := …` line — the range expression — so the `for` below is never
// reached. (Pre-v0.242 this faulted at the `for`.)
BEZEL_TEST(codegen_range_step_construction_3052,
           range_variable_zero_step_faults_at_construction_spec_16_4) {
    BEZEL_EXPECT_EQ(expectsOverflow(
        "var z: int := 0; var r := 0..10 step z; var touched: int := 0; "
        "for var i in r do touched := i; end;"), 0);
}

// §16.4 control: a NONZERO computed step is untouched — the construction guard
// must not fire, and the range still iterates. 0+2+4+6+8+10 = 30.
BEZEL_TEST(codegen_range_step_construction_3052,
           nonzero_computed_step_still_iterates_spec_16_4) {
    BEZEL_EXPECT_EQ(run(
        "var s: int := 2; var r := 0..10 step s; var sum: int := 0; "
        "for var i in r do sum := sum + i; end; return sum;"), 30);
}

// §16.4 control: a nonzero computed step in the `for`-header spelling, and a
// negative one (direction chosen by the step's runtime sign). 10+7+4+1 = 22.
BEZEL_TEST(codegen_range_step_construction_3052,
           nonzero_negative_computed_step_still_descends_spec_16_4) {
    BEZEL_EXPECT_EQ(run(
        "var s: int := -3; var sum: int := 0; "
        "for var i in 10..0 step s do sum := sum + i; end; return sum;"), 22);
}

// §16.4 control: a STEPLESS range is unaffected — the default step is 1
// (§16.4), stored without any guard, so no fault and no behavior change.
BEZEL_TEST(codegen_range_step_construction_3052,
           stepless_range_unaffected_spec_16_4) {
    BEZEL_EXPECT_EQ(run(
        "var r := 1..4; var sum: int := 0; "
        "for var i in r do sum := sum + i; end; return sum;"), 10);
}

// §16.4 / §17.5: the `in`-operand position is a third spelling of the range
// expression (`x in lo..hi step s`, §16.3's `Contains`), reached by its own
// lowering rather than the value path. §16.4's fault is stated on the
// EXPRESSION, so it must fire here too — pinned because the three range-expr
// lowerings are separate code paths and a future one could easily be missed.
BEZEL_TEST(codegen_range_step_construction_3052,
           computed_zero_step_in_in_operand_faults_spec_16_4) {
    BEZEL_EXPECT_EQ(expectsOverflow(
        "var z: int := 0; var m: bool := 3 in 0..10 step z;"), 0);
}

// §16.4 / §17.5 control: the same `in` spelling with a nonzero computed step
// still evaluates membership — `3 in 0..10 step 2` is true (§16.3: "`Step`
// affects membership only through emptiness/direction — `0..10 step 2`
// contains `3`"). Exit 5 distinguishes "no fault, membership true" from the
// helper's 0 (faulted) and 7 (ran, never entered the catch).
BEZEL_TEST(codegen_range_step_construction_3052,
           nonzero_step_in_in_operand_still_evaluates_spec_16_3) {
    BEZEL_EXPECT_EQ(run(
        "  var z: int := 2; var ok: int := 7; "
        "  try var m: bool := 3 in 0..10 step z; "
        "      ok := (if m then 5 else 6); "
        "  catch var e: OverflowException => do ok := 0; end "
        "  end; "
        "  return ok;"), 5);
}
