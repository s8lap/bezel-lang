// fix #7606 (spec §25.2, v0.355; stdlib §2.5, v0.355): `2.0 * ts` has no
// applicable operator and is a compile error.
//
// Stdlib §2.5 (v0.355), the TimeSpan record, declares
//
//   public static method operator *(a: TimeSpan, factor: double): TimeSpan;
//   public static method operator /(a: TimeSpan, divisor: double): TimeSpan;
//
// with the TimeSpan as the FIRST parameter, and no `operator *(double,
// TimeSpan)`. Spec §25.2 matches the operands to the parameters in order,
// with no automatic commutation, and a use to which no overload applies is
// "a compile error at the expression". Before the fix the pair
// (`double`, `TimeSpan`) was silently accepted and computed as a bare tick
// product (`2.0 * TimeSpan.FromTicks(5L)` printed 10).
//
//   B1  the issue's repro `2.0 * a` is rejected, naming the operator.
//   B2  `2 * a` (an int on the left) is rejected too.
//   B3  ADMITTING TWIN: `a * 2.0` compiles and prints 10.
//   B4  the annotated shape (`var a: TimeSpan`) is rejected as well.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

BEZEL_TEST(timespan_commuted_operator_7606,
           double_times_timespan_is_rejected_spec_25_2) {
    auto r = runAot(
        "module M7606A; "
        "class C public static method Main(): int "
        "  var a := TimeSpan.FromTicks(5L); "
        "  var r := 2.0 * a; "
        "  Console.WriteLine($\"{r.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` is not defined with a primitive left operand and a "
           "`TimeSpan` right operand"));
}

BEZEL_TEST(timespan_commuted_operator_7606,
           int_times_timespan_is_rejected_spec_25_2) {
    auto r = runAot(
        "module M7606B; "
        "class C public static method Main(): int "
        "  var a := TimeSpan.FromTicks(5L); "
        "  var r := 2 * a; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` is not defined with a primitive left operand"));
}

BEZEL_TEST(timespan_commuted_operator_7606,
           timespan_times_double_twin_still_computes_stdlib_2_5) {
    auto r = runAot(
        "module M7606C; "
        "class C public static method Main(): int "
        "  var a := TimeSpan.FromTicks(5L); "
        "  var r := a * 2.0; "
        "  Console.WriteLine($\"{r.Ticks}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

BEZEL_TEST(timespan_commuted_operator_7606,
           double_times_annotated_timespan_is_rejected_spec_25_2) {
    auto r = runAot(
        "module M7606D; "
        "class C public static method Main(): int "
        "  var a: TimeSpan := TimeSpan.FromTicks(5L); "
        "  var r := 2.0 * a; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` is not defined with a primitive left operand"));
}
