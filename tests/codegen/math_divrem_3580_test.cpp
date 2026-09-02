// fix #3580 — stdlib §2.2 declares Math.DivRem (since v0.215, both overloads:
//   public static method DivRem(a: int, b: int, out remainder: int): int;
//   public static method DivRem(a: long, b: long, out remainder: long): long;
// return quotient, write remainder through `out`, raise DivideByZeroException
// on b = 0 (§6.1, paralleling `/`). Until this fix the compiler REJECTED every
// DivRem call with the generic "'Math' has no static method or property
// 'DivRem'" diagnostic, even though #3559's v0.284 removal-message text
// ADVERTISED DivRem as a survivor. A user following the message's repair
// advice hit a wall.
//
// Pre-fix measured reproducer (issue 3580):
//   p_divrem.bzl:3:12: 'Math' has no static method or property 'DivRem' (§3.4)
//
// This file pins the spec-stated behaviour for both overloads, the negative-
// dividend row (the sign convention follows §6.1's `/`), the b=0 row
// (asserting the exception is thrown, not just the call's diagnostic), and
// the INT_MIN/-1 row (the signed-int-overflow rule §6.1 already pins for `/`
// applies to DivRem's quotient — the remainder is representable and follows
// the `mod` rule). The exit-code oracle is used only for the exception rows;
// AOT exit truncates to 8 bits so a value-bearing row would compare against a
// number the harness cannot report, so value rows assert STDOUT via
// `runAotOut` (which strips ONE trailing newline, hence no trailing \n).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

std::string runOut(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    if (!r.ran) return "";
    // Mirror runAotOut's trailing-newline strip so expected text is the
    // observable without the trailing \n (the harness's contract).
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

}  // namespace

// ── stdlib §2.2 Math.DivRem(int, int, out int): int — value rows ────────────

// §6.1: integer division truncated toward zero. 17 / 5 = 3 r 2.
// Before fix: compile rejected with generic unknown-member diagnostic.
BEZEL_TEST(math_divrem_3580, divrem_int_positive_spec_2_2) {
    BEZEL_EXPECT_EQ(runOut(
        "var r := 0; "
        "var q := Math.DivRem(17, 5, out r); "
        "Console.WriteLine(q); "
        "Console.WriteLine(r); "
        "return 0;"),
        "3\n2");
}

// Negative dividend: §6.1's `/` truncates toward zero, so (-17)/5 = -3 r -2.
// Pins the sign convention; the int-mod convention (§6.1) gives remainder
// `a - (a/b)*b`, which matches (-17) - (-3)*5 = -17 + 15 = -2.
BEZEL_TEST(math_divrem_3580, divrem_int_negative_spec_2_2) {
    BEZEL_EXPECT_EQ(runOut(
        "var r := 0; "
        "var q := Math.DivRem(0 - 17, 5, out r); "
        "Console.WriteLine(q); "
        "Console.WriteLine(r); "
        "return 0;"),
        "-3\n-2");
}

// Exact division — quotient matches dividend, remainder is 0.
BEZEL_TEST(math_divrem_3580, divrem_int_exact_spec_2_2) {
    BEZEL_EXPECT_EQ(runOut(
        "var r := 0; "
        "var q := Math.DivRem(20, 5, out r); "
        "Console.WriteLine(q); "
        "Console.WriteLine(r); "
        "return 0;"),
        "4\n0");
}

// Negative divisor: §6.1's `/` keeps the sign convention. (-17)/(-5) = 3 r -2.
BEZEL_TEST(math_divrem_3580, divrem_int_neg_divisor_spec_2_2) {
    BEZEL_EXPECT_EQ(runOut(
        "var r := 0; "
        "var q := Math.DivRem(0 - 17, 0 - 5, out r); "
        "Console.WriteLine(q); "
        "Console.WriteLine(r); "
        "return 0;"),
        "3\n-2");
}

// ── stdlib §2.2 Math.DivRem(long, long, out long): long — value rows ────────

// Long overload: same shape, same numbers, written with `L` suffixes so the
// polymorphic-return refinement at ExprTypecheck_Calls.cpp:10172 routes the
// call to the long overload (DivRem(int, int, out int) → int, kept as int).
BEZEL_TEST(math_divrem_3580, divrem_long_positive_spec_2_2) {
    BEZEL_EXPECT_EQ(runOut(
        "var r: long := 0L; "
        "var q := Math.DivRem(17L, 5L, out r); "
        "Console.WriteLine(q); "
        "Console.WriteLine(r); "
        "return 0;"),
        "3\n2");
}

// Long negative dividend.
BEZEL_TEST(math_divrem_3580, divrem_long_negative_spec_2_2) {
    BEZEL_EXPECT_EQ(runOut(
        "var r: long := 0L; "
        "var q := Math.DivRem(0L - 17L, 5L, out r); "
        "Console.WriteLine(q); "
        "Console.WriteLine(r); "
        "return 0;"),
        "-3\n-2");
}

// ── stdlib §2.2: b = 0 raises DivideByZeroException (§6.1, as `/`) ─────────

// §6.1: DivRem's b = 0 path follows `/` exactly. The exact exception TYPE
// matters (a code path that "prints 0" instead of throwing would still
// compile and still exit 0, so a `hasErrors()` oracle or `exit 0` check
// cannot detect the regression). Caught by the exact type AND by the §31.4
// ArithmeticException base.
BEZEL_TEST(math_divrem_3580, divrem_int_zero_divisor_throws_dbze_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "var r := 0; "
        "try var q := Math.DivRem(17, 0, out r); return q; "
        "catch DivideByZeroException => return 1; end;"), 1);
}

BEZEL_TEST(math_divrem_3580, divrem_int_zero_divisor_caught_by_arithmetic_base) {
    BEZEL_EXPECT_EQ(run(
        "var r := 0; "
        "try var q := Math.DivRem(17, 0, out r); return q; "
        "catch ArithmeticException => return 2; end;"), 2);
}

BEZEL_TEST(math_divrem_3580, divrem_long_zero_divisor_throws_dbze_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "var r: long := 0L; "
        "try var q := Math.DivRem(17L, 0L, out r); return 0; "
        "catch DivideByZeroException => return 3; end;"), 3);
}

// ── stdlib §2.2 / §6.1: signed INT_MIN / -1 raises OverflowException ────────

// §6.1 / §22.16: signed INT_MIN / -1. The mathematical quotient is
// +INT_MAX + 1 (unrepresentable), so `/` raises OverflowException; the same
// rule applies to DivRem's QUOTIENT. The mathematical remainder is 0
// (representable), so `mod` substitutes a 1-divisor and selects 0 — DivRem
// uses the same pattern for its remainder, but the OVERALL call still raises
// because the QUOTIENT edge fires first.
//
// Without the guard: LLVM `sdiv` of INT_MIN / -1 is UB (LangRef) and x86
// IDIV faults with SIGFPE. Without the quotient's guard the program would
// die with a signal, not the spec-stated exception. Without the
// `safeR = select(isMinNegOne, 1, b)` substitution the remainder edge would
// also SIGFPE on a NOT-thrown path before the select runs (we emit the
// select's two arms regardless of the actual branch outcome, so the
// UB-producing `srem(INT_MIN, -1)` is reachable even on the throw path —
// this is why both edges are required, not just the quotient's).
BEZEL_TEST(math_divrem_3580, divrem_int_min_over_neg_one_throws_overflow_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "var r := 0; "
        "try var q := Math.DivRem(-2147483648, -1, out r); return q; "
        "catch OverflowException => return 4; end;"), 4);
}

// Long MIN / -1 — the same rule at the 64-bit width.
BEZEL_TEST(math_divrem_3580, divrem_long_min_over_neg_one_throws_overflow_spec_2_2) {
    BEZEL_EXPECT_EQ(run(
        "var r: long := 0L; "
        "try var q := Math.DivRem(-9223372036854775808L, -1L, out r); "
        "return 0; "
        "catch OverflowException => return 5; end;"), 5);
}

// ── §6.1: out remainder slot receives the actual remainder on the success ──
//         path (not the pre-declared 0, not the integer-division default).

// Distinguishes a real write from "out was never assigned". `r` is
// pre-declared with a known nonzero value so the row fails if the call
// merely returns a quotient and leaves the slot alone. Without this guard a
// hypothetical codegen that returned a quotient but skipped the remainder
// store would still exit 0 and print only one number, but the EXPECT_EQ's
// expected text includes `99` (the pre-declared value's failure mode) so the
// row would fail loudly.
BEZEL_TEST(math_divrem_3580, divrem_out_writes_the_remainder_spec_2_2) {
    BEZEL_EXPECT_EQ(runOut(
        "var r := 999; "
        "var q := Math.DivRem(17, 5, out r); "
        "Console.WriteLine(r); "
        "return 0;"),
        "2");
}
