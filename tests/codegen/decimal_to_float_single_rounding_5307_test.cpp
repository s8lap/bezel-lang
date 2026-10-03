// Regression tests for #5307 (spec §5.2, v0.313): "`double(m)` and `float(m)`
// for `decimal` `m` round to the nearest representable binary value". The
// float target was lowered as decimal -> double -> float (two roundings), so
// `float(16777217.0000000000000000001m)` was 16777216 although the nearest
// float is 16777218. Expected values are computed independently in python3
// (exact Fraction arithmetic, nearest float32, ties to even; see
// /var/tmp/bezel-scratch/r26/5307/exp.py), never by the compiler.
//
// Every row converts in a 200-iteration loop and prints how many iterations
// matched the expected float literal (the literal is the shortest round-trip
// spelling of the python3 nearest float, far from any rounding tie).
//
// §5.2 states `float(m)` "never fail[s]" (float's finite range exceeds
// decimal's: |m| < 2^96 < float.MaxValue, and the smallest non-zero |m| = 1e-28
// is a normal float), so a above-max or underflow-to-zero row cannot exist:
// the largest and smallest decimal magnitudes are pinned instead.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {
std::string out(const std::string& body) {
    return runAotOut("module M; class C public static method Main(): int " +
                     body + " return 0; end Main; end C;");
}
std::string row(const std::string& lit, const std::string& expectedF) {
    return out("var m := " + lit + "m; var ok := 0; var i := 0; "
               "while i < 200 do "
               "if float(m) = " + expectedF + "f then ok := ok + 1; end; "
               "i := i + 1; end; Console.WriteLine(ok); ");
}
std::string rowD(const std::string& lit, const std::string& expectedD) {
    return out("var m := " + lit + "m; var ok := 0; var i := 0; "
               "while i < 200 do "
               "if double(m) = " + expectedD + " then ok := ok + 1; end; "
               "i := i + 1; end; Console.WriteLine(ok); ");
}
}  // namespace

// The issue repro: nearest float is 16777218 (the double-rounded path gave 16777216).
BEZEL_TEST(decimal_to_float_5307, issue_repro) {
    BEZEL_EXPECT_EQ(row("16777217.0000000000000000001", "16777218.0"), "200");
}
BEZEL_TEST(decimal_to_float_5307, issue_repro_prints_16777218) {
    BEZEL_EXPECT_EQ(
        out("var m := 16777217.0000000000000000001m; var f := float(m); "
            "Console.WriteLine(f = 16777218.0f); "
            "Console.WriteLine(f.ToString()); "),
        "True\n16777218");
}
// Double-rounding witness that goes DOWN wrongly: via double -> exact tie 1+2^-24 -> even 1.0.
BEZEL_TEST(decimal_to_float_5307, witness_wrongly_down_near_one) {
    BEZEL_EXPECT_EQ(row("1.0000000596046447753906250001", "1.0000001"), "200");
}
// Double-rounding witness that goes UP wrongly: via double -> 16777219 tie -> even 16777220.
BEZEL_TEST(decimal_to_float_5307, witness_wrongly_up) {
    BEZEL_EXPECT_EQ(row("16777218.9999999999999999999", "16777218.0"), "200");
}
// Not a witness (double rounding agrees), pinned for the tie neighbourhood.
BEZEL_TEST(decimal_to_float_5307, below_tie_16777217) {
    BEZEL_EXPECT_EQ(row("16777216.9999999999999999999", "16777216.0"), "200");
}
BEZEL_TEST(decimal_to_float_5307, above_tie_16777219) {
    BEZEL_EXPECT_EQ(row("16777219.0000000000000000001", "16777220.0"), "200");
}
BEZEL_TEST(decimal_to_float_5307, neg_witness) {
    BEZEL_EXPECT_EQ(row("-16777217.0000000000000000001", "-16777218.0"), "200");
}
// Exact float values convert exactly.
BEZEL_TEST(decimal_to_float_5307, exact_1_5) {
    BEZEL_EXPECT_EQ(row("1.5", "1.5"), "200");
}
BEZEL_TEST(decimal_to_float_5307, exact_2p24) {
    BEZEL_EXPECT_EQ(row("16777216", "16777216.0"), "200");
}
BEZEL_TEST(decimal_to_float_5307, exact_neg_0_25) {
    BEZEL_EXPECT_EQ(row("-0.25", "-0.25"), "200");
}
BEZEL_TEST(decimal_to_float_5307, nonexact_0_1) {
    BEZEL_EXPECT_EQ(row("0.1", "0.1"), "200");
}
BEZEL_TEST(decimal_to_float_5307, neg_0_1) {
    BEZEL_EXPECT_EQ(row("-0.1", "-0.1"), "200");
}
// Scale 0 and scale 28.
BEZEL_TEST(decimal_to_float_5307, scale0_27_digits) {
    BEZEL_EXPECT_EQ(row("123456789012345678901234567", "1.2345679e26"), "200");
}
BEZEL_TEST(decimal_to_float_5307, scale28_0_5_plus_tiny) {
    BEZEL_EXPECT_EQ(row("0.5000000000000000000000000001", "0.5"), "200");
}
BEZEL_TEST(decimal_to_float_5307, scale28_full96) {
    BEZEL_EXPECT_EQ(row("7.9228162514264337593543950335", "7.9228163"), "200");
}
// Max 96-bit mantissa at scale 0 (decimal.MaxValue) and negated.
BEZEL_TEST(decimal_to_float_5307, max_value) {
    BEZEL_EXPECT_EQ(row("79228162514264337593543950335", "7.9228163e28"), "200");
}
BEZEL_TEST(decimal_to_float_5307, min_value) {
    BEZEL_EXPECT_EQ(row("-79228162514264337593543950335", "-7.9228163e28"), "200");
}
// Smallest non-zero magnitudes (a normal float; nothing underflows).
BEZEL_TEST(decimal_to_float_5307, tiny_1e_28) {
    BEZEL_EXPECT_EQ(row("0.0000000000000000000000000001", "1e-28"), "200");
}
BEZEL_TEST(decimal_to_float_5307, tiny_neg_3e_28) {
    BEZEL_EXPECT_EQ(row("-0.0000000000000000000000000003", "-3e-28"), "200");
}
BEZEL_TEST(decimal_to_float_5307, zero) {
    BEZEL_EXPECT_EQ(row("0", "0.0"), "200");
}
// Zero: §4.4 "the sign of a zero is not part of the value", so float(m) and
// double(m) of any decimal zero are +0 (1/x > 0, i.e. +Infinity).
BEZEL_TEST(decimal_to_float_5307, neg_zero_literal_float) {
    BEZEL_EXPECT_EQ(out("var m := -0.0m; Console.WriteLine(1.0f / float(m) > 0.0f); "),
                    "True");
}
BEZEL_TEST(decimal_to_float_5307, neg_zero_computed_float) {
    BEZEL_EXPECT_EQ(out("var z := 0.0m; var m := -z; Console.WriteLine(1.0f / float(m) > 0.0f); "),
                    "True");
}
BEZEL_TEST(decimal_to_float_5307, neg_zero_literal_double) {
    BEZEL_EXPECT_EQ(out("var m := -0.0m; Console.WriteLine(1.0 / double(m) > 0.0); "),
                    "True");
}
BEZEL_TEST(decimal_to_float_5307, neg_zero_computed_double) {
    BEZEL_EXPECT_EQ(out("var z := 0.0m; var m := -z; Console.WriteLine(1.0 / double(m) > 0.0); "),
                    "True");
}
// double target is unchanged by the fix (#5298 owns its rows).
BEZEL_TEST(decimal_to_float_5307, double_target_unchanged) {
    BEZEL_EXPECT_EQ(rowD("0.12345678901234567890123456789", "0.12345678901234568"), "200");
}
