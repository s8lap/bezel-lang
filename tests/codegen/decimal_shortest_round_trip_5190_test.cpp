// Codegen tests for fix #5190: §5.2 v0.311 shortest round-trip decimal conversion.
//
// Spec reference:
//   §5.2 (v0.311): "decimal(x) for binary-float x yields the value of x's
//   shortest round-trip form — the digits x's no-format ToString() produces,
//   the shortest decimal string that parses back to x in x's own type and,
//   of several such, the one nearest x — rounded as §4.4 states where that
//   value is not in decimal's set:
//     decimal(0.1)           → 0.1
//     decimal(0.1 + 0.2)     → 0.30000000000000004
//     decimal(0.1f)          → 0.1
//     decimal(2.5e-28)       → 0.0000000000000000000000000002  (tie at scale 28 → even digit)
//
// Issue: #5190 (OPEN, spec-ahead-of-impl)
// Branch: a24-5190-rb / WT: /mnt/fast/worktrees/a24-wt-5190

#include <cstdio>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

// Assert a decimal value equals the given decimal string literal.
int assertDecimalEq(const std::string& expr, const char* expected, int ok = 42) {
    return run(prog(
        "var d := " + expr + "; "
        "if d <> " + std::string(expected) + "m then return 1; end; "
        "return " + std::to_string(ok) + ";"));
}

} // namespace

// ---------------------------------------------------------------------------
// §5.2 v0.311: shortest round-trip decimal conversion
// ---------------------------------------------------------------------------

// decimal(0.1) — double 0.1 shortest round-trip is "0.1" → decimal(0.1) = 0.1m
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_0_1_double_shortest_spec_5_2_v0_311) {
    // 0.1 is exactly representable as double; shortest round-trip = "0.1"
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(0.1)", "0.1"), 42);
}

// decimal(0.1 + 0.2) — double result's shortest round-trip is "0.30000000000000004"
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_0_1_plus_0_2_double_shortest_spec_5_2_v0_311) {
    // 0.1 + 0.2 = 0.30000000000000004 in double (binary FP accumulation).
    // The shortest round-trip of 0.30000000000000004 is "0.30000000000000004".
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(0.1 + 0.2)", "0.30000000000000004"), 42);
}

// decimal(0.1f) — float 0.1 shortest round-trip is "0.1" (not "0.100000024")
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_0_1f_float_shortest_spec_5_2_v0_311) {
    // The old %.15g approach emitted "0.100000024" for float 0.1f, losing the
    // shortest round-trip property. The fix uses float-specific search (1..9 digits).
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(0.1f)", "0.1"), 42);
}

// decimal(1.5) — double 1.5 is exactly representable; shortest = "1.5"
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_1_5_double_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(1.5)", "1.5"), 42);
}

// decimal(2.5e-28) — SPEC ROW (§5.2 v0.311 line 768, §4.4 ties-to-even).
// Shortest round-trip of 2.5e-28 is "2.5e-28" → parses to mantissa 25, scale 29.
// Reducing scale 29→28 drops the single digit 5, so the dropped portion is
// exactly half: 2.5e-28 lies midway between 2e-28 and 3e-28, and §6.4's tie to
// the even last digit takes it to 2e-28.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_2_5e_28_spec_5_2_4_4_ties_to_even) {
    // §5.2 v0.311 line 768: "decimal(2.5e-28) is 0.0000000000000000000000000002,
    //  the form's tie at scale 28 going to the even digit"
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(2.5e-28)", "0.0000000000000000000000000002"), 42);
}

// decimal(2.6e-28) — NOT A TIE: the dropped digit is 6, strictly above half, so
// the result is 3e-28 regardless of the kept parity. Shortest round-trip
// "2.6e-28" → mantissa 26, scale 29. Pairs with the two tie rows to show the
// rule is "above half rounds up", not "dropped digit ≥ 5 rounds up".
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_2_6e_28_non_tie_round_up_spec_5_2_4_4) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(2.6e-28)", "0.0000000000000000000000000003"), 42);
}

// decimal(3.5e-28) — ODD TIE: the same exact-half tie as 2.5e-28, but here the
// kept mantissa is odd, so §6.4's tie to the even last digit rounds UP.
// Shortest round-trip "3.5e-28" → mantissa 35, scale 29; dropping the single
// digit 5 leaves 3, which is odd → 4e-28. Paired with the 2.5e-28 row above it
// pins the direction of the tie: same dropped digit, opposite kept parity,
// opposite result.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_3_5e_28_odd_tie_round_up_spec_5_2_4_4) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(3.5e-28)", "0.0000000000000000000000000004"), 42);
}

// ---------------------------------------------------------------------------
// §4.4 / §6.4: the rounding direction comes from the MOST significant dropped
// digit. Every row above reduces by exactly one digit, where the first digit
// peeled is also the deciding one, so they cannot distinguish a peel that reads
// the direction off the LEAST significant dropped digit. These rows reduce by
// two or three and do.
// ---------------------------------------------------------------------------

// decimal(1.46e-28) — drop=2, most significant dropped digit is 4 (< 5) →
// truncate. 146e-30 = 14.6e-29 → 1e-28. Reading the direction off the least
// significant dropped digit (6 > 5) wrongly rounded up to 2e-28.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_1_46e_28_drop2_truncates_spec_4_4) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(1.46e-28)", "0.0000000000000000000000000001"), 42);
}

// decimal(2.55e-28) — drop=2, deciding digit 5 with a non-zero digit below it
// (5) → strictly above half → round up. 255e-30 → 3e-28. Reading the direction
// off the least significant dropped digit (5, tie) wrongly truncated to 2e-28.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_2_55e_28_drop2_above_half_spec_4_4) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(2.55e-28)", "0.0000000000000000000000000003"), 42);
}

// decimal(5.55e-29) — drop=3, deciding digit 5 with non-zero digits below
// (5, 5) → above half → round up to 1e-28. Reading the direction off the least
// significant dropped digit (5) and stopping there wrongly truncated to 0.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_5_55e_29_drop3_above_half_spec_4_4) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(5.55e-29)", "0.0000000000000000000000000001"), 42);
}

// decimal(1.2345e-29) — drop=5, deciding digit 1 (< 5) → truncate to 0.
// Guards the peel for a multi-digit reduction that must not accumulate the
// dropped portion into a value (10^5 does not threaten __uint128_t, but the
// subnormal 1e-324 reaches drop=296, which would).
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_1_2345e_29_drop5_truncates_spec_4_4) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(1.2345e-29)", "0"), 42);
}

// decimal(-0.1) — negative double
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_neg_0_1_double_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(-0.1)", "-0.1"), 42);
}

// decimal(0.0) — zero
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_0_0_double_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(0.0)", "0"), 42);
}

// decimal(1.0) — one
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_1_0_double_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(1.0)", "1"), 42);
}

// decimal(0.2f) — float 0.2 shortest round-trip
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_0_2f_float_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(0.2f)", "0.2"), 42);
}

// decimal(NaN) — raises OverflowException per §5.2 (compile-time constant) or
// is rejected at compile time (fix #4801 v0.307). This tests a RUNTIME NaN
// (non-constant operand) which raises OverflowException at runtime.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_nan_overflow_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(run(prog(
        "var n: double := 0.0 / 0.0; "   // non-constant: runtime NaN
        "try "
        "  var d := decimal(n); "
        "  return 1; "   // should not reach
        "catch OverflowException => return 42; "
        "end; "
        "return 2; ")), 42);
}

// decimal(+Infinity) — raises OverflowException per §5.2
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_pos_inf_overflow_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  var inf := 1.0 / 0.0; "
        "  var d := decimal(inf); "
        "  return 1; "
        "catch OverflowException => return 42; "
        "end; "
        "return 2; ")), 42);
}

// decimal(-Infinity) — raises OverflowException per §5.2
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_neg_inf_overflow_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  var inf := -1.0 / 0.0; "
        "  var d := decimal(inf); "
        "  return 1; "
        "catch OverflowException => return 42; "
        "end; "
        "return 2; ")), 42);
}

// decimal(x) for a runtime x at 2^96 — §4.4: decimal.MaxValue is 2^96 − 1, so a
// value at or above 2^96 does not fit at scale 0 and is out of range. §5.2: an
// out-of-range binary float raises OverflowException. The operand is a runtime
// double (not a constant), so this exercises the runtime entry; the constant
// form is a compile error at the conversion site per §17.14.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_2_pow_96_overflow_spec_4_4_5_2_v0_311) {
    BEZEL_EXPECT_EQ(run(prog(
        "var v := 7.9228162514264338e28; "   // rounds to 2^96
        "try "
        "  var d := decimal(v); "
        "  return 1; "
        "catch OverflowException => return 42; "
        "end; "
        "return 2; ")), 42);
}

// decimal(1.0000000000000002) — §5.2's shortest round-trip rule: the value one
// ulp above 1.0 needs 17 significant digits to round-trip, and that is what
// decimal(x) reads (%.15g could not distinguish it from 1.0).
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_1_0000000000000002_double_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(1.0000000000000002)", "1.0000000000000002"), 42);
}

// decimal(5e-29) — §5.2's own subnormal-range example: 5e-29 = 0.5e-28 is an
// exact tie at scale 28 and ties-to-even takes it to 0 (§6.4: "a nonzero result
// that rounds to zero is zero"). Fix-forward: reading the tie off the peeled
// digit and rounding half-up gave 1e-28.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_5e_29_tie_to_even_zero_spec_4_4) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(5e-29)", "0"), 42);
}

// decimal(3.14159) — double that is not exactly representable
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_pi_double_shortest_spec_5_2_v0_311) {
    // double pi ≈ 3.141592653589793; shortest round-trip is "3.141592653589793"
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(3.14159)", "3.14159"), 42);
}

// decimal(3.141592653589793) — the issue's own row: the full 16-digit shortest
// round-trip of pi, which %.15g truncated to 3.14159265358979.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_pi_full_double_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(3.141592653589793)", "3.141592653589793"), 42);
}
