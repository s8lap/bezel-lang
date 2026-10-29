// R37 campaign-2 lane C2-07, issue 6942 (spec §27.6 "References to generic methods",
// §14.2, §14.5, v0.350; stdlib §2.2).
//
//   #6942 - `Math.Sign` declares only `Sign<T>` (stdlib §2.2), so a bare
//           reference with no type-argument list is "a compile error naming
//           the type parameters to supply" (§27.6). It was typed `int` (the
//           callee result), so `var g := Math.Sign;` compiled to a zero slot
//           and `g(5)` died with "cannot call a value of type `int`".

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

// #6942 repro 1 (the issue's literal program): untyped bare reference.
BEZEL_TEST(sema_math_sign_reference_6942,
           bare_math_sign_reference_without_target_rejected_27_6_issue_6942) {
    auto r = runAot(
        "module M; import System; "
        "class C public static method Main(): int "
        "  var g := Math.Sign; "
        "  Console.WriteLine(g(5)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Math.Sign` is a reference to a generic method and names no type arguments"));
}

// Same rule with a target type: no inference binds the parameter from it.
BEZEL_TEST(sema_math_sign_reference_6942,
           bare_math_sign_reference_with_target_rejected_27_6_issue_6942) {
    auto r = runAot(
        "module M; import System; "
        "class C public static method Main(): int "
        "  var g: (int) -> int := Math.Sign; "
        "  return g(5); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Math.Sign` is a reference to a generic method and names no type arguments"));
}

// Mirror: the CALL spelling `Math.Sign(x)` (and an explicit-list call) must
// stay admitted - only the non-callee bare reference is the compile error.
BEZEL_TEST(sema_math_sign_reference_6942,
           math_sign_call_spellings_still_admitted_27_6_issue_6942) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C public static method Main(): int "
        "  Console.WriteLine(Math.Sign(0 - 5)); "
        "  Console.WriteLine(Math.Sign<int>(7)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-1\n1\n"));
}
