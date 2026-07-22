// Codegen + runtime — Math.Sign return type is always int (stdlib §2.2).
//
// Spec authority: stdlib §2.2 (Math class):
//   public static method Sign(x: int):    int;   // -1, 0, or 1
//   public static method Sign(x: long):   int;
//   public static method Sign(x: double): int;
//
// Bug: before this fix, ExprTypecheck_Calls.cpp included "Sign" in the
// "Math polymorphic — return type matches argument type" override block.
// That block wrongly promoted Sign's result type from int to double when the
// argument was double/float/long.  The consequence:
//
//   var sg := Math.Sign(-5.0);
//   return (1 if sg = -1 else 0);    // sg typed as double → ICmp on
//                                     // mismatched types → SIGABRT
//
// Fix: remove "Sign" from the polymorphic-return override so that every
// Sign overload keeps its declared int return type.
//
// Control: Math.Abs IS polymorphic (Abs(x: double): double, etc.) and must
// NOT be affected by this fix.
//
// stdlib §2.2:
//   public static method Abs(x: int):     int;
//   public static method Abs(x: long):    long;
//   public static method Abs(x: double):  double;
//   public static method Abs(x: decimal): decimal;

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── stdlib §2.2 Math.Sign(x: double): int ────────────────────────────────────

// Exact crash reproducer: Sign(-5.0) must return -1, not crash with ICmp mismatch.
BEZEL_TEST(math_sign, sign_double_negative_is_minus_one_spec_2_2) {
    // stdlib §2.2: Sign(x: double): int — negative argument yields -1.
    // Before fix: sg typed as double → comparison with int -1 → SIGABRT.
    BEZEL_EXPECT_EQ(run(
        "var sg := Math.Sign(0.0 - 5.0); "
        "return (if sg = -1 then 1 else 0);"), 1);
}

BEZEL_TEST(math_sign, sign_double_positive_is_plus_one_spec_2_2) {
    // stdlib §2.2: Sign(x: double): int — positive argument yields 1.
    BEZEL_EXPECT_EQ(run(
        "var sg := Math.Sign(5.0); "
        "return (if sg = 1 then 1 else 0);"), 1);
}

BEZEL_TEST(math_sign, sign_double_zero_is_zero_spec_2_2) {
    // stdlib §2.2: Sign(x: double): int — zero argument yields 0.
    BEZEL_EXPECT_EQ(run(
        "var sg := Math.Sign(0.0); "
        "return (if sg = 0 then 1 else 0);"), 1);
}

// ── stdlib §2.2 Math.Sign(x: int): int ───────────────────────────────────────

BEZEL_TEST(math_sign, sign_int_negative_is_minus_one_spec_2_2) {
    // stdlib §2.2: Sign(x: int): int — negative int argument yields -1.
    BEZEL_EXPECT_EQ(run(
        "var sg := Math.Sign(0 - 3); "
        "return (if sg = -1 then 1 else 0);"), 1);
}

BEZEL_TEST(math_sign, sign_int_positive_is_plus_one_spec_2_2) {
    // stdlib §2.2: Sign(x: int): int — positive int argument yields 1.
    BEZEL_EXPECT_EQ(run(
        "var sg := Math.Sign(42); "
        "return (if sg = 1 then 1 else 0);"), 1);
}

BEZEL_TEST(math_sign, sign_int_zero_is_zero_spec_2_2) {
    // stdlib §2.2: Sign(x: int): int — zero int argument yields 0.
    BEZEL_EXPECT_EQ(run(
        "var sg := Math.Sign(0); "
        "return (if sg = 0 then 1 else 0);"), 1);
}

// ── stdlib §2.2 Math.Sign(x: long): int ──────────────────────────────────────

BEZEL_TEST(math_sign, sign_long_negative_is_minus_one_spec_2_2) {
    // stdlib §2.2: Sign(x: long): int — negative long yields -1.
    // The override block previously promoted Sign's result to long; verify int.
    BEZEL_EXPECT_EQ(run(
        "var n: long := 0L - 999L; "
        "var sg := Math.Sign(n); "
        "return (if sg = -1 then 1 else 0);"), 1);
}

BEZEL_TEST(math_sign, sign_long_positive_is_plus_one_spec_2_2) {
    // stdlib §2.2: Sign(x: long): int — positive long yields 1.
    BEZEL_EXPECT_EQ(run(
        "var n: long := 999L; "
        "var sg := Math.Sign(n); "
        "return (if sg = 1 then 1 else 0);"), 1);
}

// ── result used in int arithmetic (the crash trigger path) ───────────────────

BEZEL_TEST(math_sign, sign_result_used_in_arithmetic_spec_2_2) {
    // stdlib §2.2: Sign returns int — arithmetic on result must be valid.
    // If Sign were typed as double, adding two Sign results would produce
    // double + double, and the return of (double) to int would fail or
    // require an explicit cast.  This test checks the int arithmetic path.
    BEZEL_EXPECT_EQ(run(
        "var a := Math.Sign(0.0 - 3.0); "   // -1
        "var b := Math.Sign(7.0); "          // 1
        "return a + b;"), 0);                // -1 + 1 = 0
}

BEZEL_TEST(math_sign, sign_result_compared_to_negative_literal_spec_2_2) {
    // stdlib §2.2: the crash reproducer pattern — Sign(double) result compared
    // with a negative int literal.  ICmp requires both operands to be the same
    // type; if Sign is typed as double this emits double ICmp int → SIGABRT.
    BEZEL_EXPECT_EQ(run(
        "var sg := Math.Sign(0.0 - 5.0); "
        "return (if sg = -1 then 1 else 0);"), 1);
}

// ── control: Math.Abs remains polymorphic (still returns argument type) ───────

BEZEL_TEST(math_sign, abs_double_returns_double_spec_2_2) {
    // stdlib §2.2: Abs(x: double): double — result is double, not int.
    // The polymorphic-return override for Abs must remain intact.
    // We verify by using the result in a double comparison (would fail if int).
    BEZEL_EXPECT_EQ(run(
        "var a := Math.Abs(0.0 - 5.0); "
        "return (if a > 4.9 and a < 5.1 then 1 else 0);"), 1);
}

BEZEL_TEST(math_sign, abs_int_returns_int_spec_2_2) {
    // stdlib §2.2: Abs(x: int): int — result is int.
    // Polymorphic override: int arg → int result.
    BEZEL_EXPECT_EQ(run(
        "var a := Math.Abs(0 - 5); "
        "return (if a = 5 then 1 else 0);"), 1);
}
