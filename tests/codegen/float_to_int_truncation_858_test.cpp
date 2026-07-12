// fix #858 / §5.2: a float/double -> integer cast whose value truncates toward
// zero ONTO the target's minimum (e.g. sbyte(-128.9) -> -128 = sbyte.MinValue)
// wrongly raised OverflowException. §5.2: "Floating-point to integer truncates
// toward zero; ... out-of-range values raise OverflowException" — the truncated
// value -128 IS in range, so no overflow. The codegen's in-range guard used the
// CLOSED lower bound `src >= MIN`, rejecting the (MIN-1, MIN] band that
// truncates to MIN. The fix uses the exclusive `src > MIN-1` when MIN-1 is
// exactly representable in the source FP type, and keeps `src >= MIN` for large
// targets whose MIN-1 rounds to MIN (int<-float, long) where FP granularity >= 1
// makes the closed bound exact.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// Wrap a cast attempt: 42 if it truncates to `eq`, 1 if OverflowException.
std::string cast(const std::string& srcTy, const std::string& val,
                 const std::string& dstTy, const std::string& eq) {
    return "module M; class C public static method Main(): int "
           "  var f: " + srcTy + " := " + val + "; "
           "  try var y: " + dstTy + " := " + dstTy + "(f); "
           "    return (42 if y = " + eq + " else 0); "
           "  catch OverflowException => return 1; end; "
           "end Main; end C;";
}
}  // namespace

// float -> byte: trunc(-0.9) = 0, in [0,255] (c25).
BEZEL_TEST(float_to_int_truncation_858, float_to_byte_neg_frac_to_zero) {
    BEZEL_EXPECT_EQ(run(cast("float", "-0.9f", "byte", "0")), 42);
}

// float -> sbyte: trunc(-128.9) = -128 = sbyte.MinValue (c26).
BEZEL_TEST(float_to_int_truncation_858, float_to_sbyte_min) {
    BEZEL_EXPECT_EQ(run(cast("float", "-128.9f", "sbyte", "-128")), 42);
}

// double -> int: trunc(-2147483648.9) = int.MinValue (c27) — MIN-1 exact in double.
BEZEL_TEST(float_to_int_truncation_858, double_to_int_min) {
    BEZEL_EXPECT_EQ(run(cast("double", "-2147483648.9", "int", "int.MinValue")), 42);
}

// float -> short: trunc(-32768.9) = -32768 = short.MinValue (c28).
BEZEL_TEST(float_to_int_truncation_858, float_to_short_min) {
    BEZEL_EXPECT_EQ(run(cast("float", "-32768.9f", "short", "-32768")), 42);
}

// double -> sbyte: trunc(-128.9) = -128 (c29).
BEZEL_TEST(float_to_int_truncation_858, double_to_sbyte_min) {
    BEZEL_EXPECT_EQ(run(cast("double", "-128.9", "sbyte", "-128")), 42);
}

// Regression: a source genuinely below MIN-1 still overflows (sbyte <- -129.0).
BEZEL_TEST(float_to_int_truncation_858, below_min_still_overflows) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f: float := -129.0f; "
        "  try var y: sbyte := sbyte(f); return y; "
        "  catch OverflowException => return 99; end; "
        "end Main; end C;"), 99);
}

// Regression: a source at/above MAX+1 still overflows (int <- 2147483648.0).
BEZEL_TEST(float_to_int_truncation_858, above_max_still_overflows) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: double := 2147483648.0; "
        "  try var y: int := int(d); return y; "
        "  catch OverflowException => return 99; end; "
        "end Main; end C;"), 99);
}

// Regression (precision-sensitive): int <- float at exactly int.MinValue must
// NOT overflow — MIN-1 rounds to MIN in float, so the closed `>= MIN` bound is
// used and -2^31 is accepted.
BEZEL_TEST(float_to_int_truncation_858, int_from_float_min_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var f: float := -2147483648.0f; "
        "  try var y: int := int(f); return (42 if y = int.MinValue else 0); "
        "  catch OverflowException => return 1; end; "
        "end Main; end C;"), 42);
}

// Regression: ordinary in-range truncation toward zero is unchanged.
BEZEL_TEST(float_to_int_truncation_858, ordinary_truncation_toward_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: double := -5.9; return int(d) + 100; "  // trunc(-5.9) = -5 -> 95
        "end Main; end C;"), 95);
}
