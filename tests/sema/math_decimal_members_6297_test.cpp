// Regression tests for #6297 (sema half): the call result of `Math.Abs`,
// `Math.Floor`, `Math.Ceiling`, `Math.Truncate`, `Math.Min`, `Math.Max` and
// `Math.Clamp` on decimal arguments is decimal (stdlib §2.2, v0.339), so the
// initializer check of §5.1 applies to it.
//
// THE DEFECT.  The call's type was a placeholder: int for Abs, double for the
// rounding members, and untyped for the Min / Max / Clamp selectors.  The
// initializer check accepts an untyped result into any target, so
//     var y: int := Math.Min(x, 2m);     -- x: decimal
// compiled on f9c549e0 and silently took a decimal into an int, while
//     var y: int := Math.Abs(x);
// reached codegen and failed verification.  A decimal to int or to a binary float
// is never implicit (§5.1), so both are rejected at compile time now.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
// The one-variable program shape: `x` is decimal, `decl` is the statement under
// test.  Only the declaration varies, so a rejection is attributable to it.
std::string withDecl(const char* decl) {
    return std::string("module Conf; public class Program public static method Main() "
                       "  var x: decimal := -2.25m; ") +
           decl + " end Main; end Program;";
}
}  // namespace

// §5.1: an `int` target takes no decimal.  The Abs result was typed int, so this
// formerly reached codegen (malformed IR) instead of being refused.
BEZEL_TEST(sema_math_decimal_members_6297, abs_decimal_into_int_rejected_5_1) {
    auto r = runAot(withDecl("var y: int := Math.Abs(x);"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type (§5.1)"));
}

// §5.1: a decimal is never implicit to a binary float.  Floor formerly typed as
// double, so this was accepted at sema and failed in codegen.
BEZEL_TEST(sema_math_decimal_members_6297, floor_decimal_into_double_rejected_5_1) {
    auto r = runAot(withDecl("var y: double := Math.Floor(x);"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "between `decimal` and binary floats (`float`, `double`) is never implicit"));
}

// §5.1: the untyped Min selector silently accepted a decimal into int on the
// base compiler (rc 0).  Fail-without shape: the base accepts this program.
BEZEL_TEST(sema_math_decimal_members_6297, min_decimal_into_int_rejected_5_1) {
    auto r = runAot(withDecl("var y: int := Math.Min(x, 2m);"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type (§5.1)"));
}

// §5.1: Clamp at T = decimal is decimal, so an int target is refused the same way.
BEZEL_TEST(sema_math_decimal_members_6297, clamp_decimal_into_int_rejected_5_1) {
    auto r = runAot(withDecl("var y: int := Math.Clamp(x, 1m, 3m);"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type (§5.1)"));
}

// CONTROL: a decimal target is the accepted case for every member, so the new
// rejections are limited to the non-decimal targets.
BEZEL_TEST(sema_math_decimal_members_6297, decimal_targets_accepted) {
    auto r = runAot(withDecl(
        "var a: decimal := Math.Abs(x); "
        "var b: decimal := Math.Floor(x); "
        "var c: decimal := Math.Ceiling(x); "
        "var d: decimal := Math.Truncate(x); "
        "var e: decimal := Math.Min(x, 2m); "
        "var f: decimal := Math.Max(x, 2m); "
        "var g: decimal := Math.Clamp(x, 1m, 3m);"));
    BEZEL_EXPECT_TRUE(r.ran);
}
