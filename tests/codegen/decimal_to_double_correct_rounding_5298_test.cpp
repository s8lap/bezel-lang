// Regression tests for #5298 (spec §5.2, v0.313): `double(m)` for a `decimal`
// m rounds to the nearest representable binary value. The runtime converted
// the 96-bit mantissa to a double and divided by 10^scale in double arithmetic
// (two roundings), so `double(0.12345678901234567890123456789m)` was the
// neighbouring double, 0.12345678901234569. Each row compares `double(m)` with
// the double spelled by the expected correctly rounded value, computed
// independently as python3 `float(Decimal(...))` (see
// /var/tmp/bezel-scratch/r26/5299/expected-5298.txt) and read back by
// `double.Parse` (strtod, one rounding).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {
std::string out(const std::string& body) {
    return runAotOut("module M; class C public static method Main(): int " +
                     body + " return 0; end Main; end C;");
}
// Prints True when double(<lit>m) equals the double spelled `expected`.
std::string row(const std::string& lit, const std::string& expected) {
    return out("var m := " + lit + "m; "
               "Console.WriteLine(double(m) = double.Parse(\"" + expected +
               "\")); ");
}
}  // namespace

BEZEL_TEST(decimal_to_double_5298, spec_5_2_example_prints_568) {
    BEZEL_EXPECT_EQ(
        out("Console.WriteLine(double(0.12345678901234567890123456789m).ToString()); "),
        "0.12345678901234568");
}
BEZEL_TEST(decimal_to_double_5298, spec_5_2_example) {
    BEZEL_EXPECT_EQ(row("0.12345678901234567890123456789", "0.12345678901234568"), "True");
}
BEZEL_TEST(decimal_to_double_5298, max_value) {
    BEZEL_EXPECT_EQ(row("79228162514264337593543950335", "7.922816251426434e28"), "True");
}
BEZEL_TEST(decimal_to_double_5298, full96_scale28) {
    BEZEL_EXPECT_EQ(row("7.9228162514264337593543950335", "7.9228162514264335"), "True");
}
BEZEL_TEST(decimal_to_double_5298, full96_scale28_neg) {
    BEZEL_EXPECT_EQ(row("-7.9228162514264337593543950335", "-7.9228162514264335"), "True");
}
BEZEL_TEST(decimal_to_double_5298, tiny_1e_28) {
    BEZEL_EXPECT_EQ(row("0.0000000000000000000000000001", "1e-28"), "True");
}
BEZEL_TEST(decimal_to_double_5298, tiny_neg_3e_28) {
    BEZEL_EXPECT_EQ(row("-0.0000000000000000000000000003", "-3e-28"), "True");
}
BEZEL_TEST(decimal_to_double_5298, int_2p53_plus1_tie_even) {
    BEZEL_EXPECT_EQ(row("9007199254740993", "9007199254740992.0"), "True");
}
BEZEL_TEST(decimal_to_double_5298, int_2p53_plus3_tie_even) {
    BEZEL_EXPECT_EQ(row("9007199254740995", "9007199254740996.0"), "True");
}
BEZEL_TEST(decimal_to_double_5298, int_neg_2p53_plus1) {
    BEZEL_EXPECT_EQ(row("-9007199254740993", "-9007199254740992.0"), "True");
}
BEZEL_TEST(decimal_to_double_5298, int_27_digits) {
    BEZEL_EXPECT_EQ(row("123456789012345678901234567", "1.2345678901234568e26"), "True");
}
BEZEL_TEST(decimal_to_double_5298, scale28_one_point) {
    BEZEL_EXPECT_EQ(row("1.2345678901234567890123456789", "1.2345678901234567"), "True");
}
BEZEL_TEST(decimal_to_double_5298, mixed_17_int_digits) {
    BEZEL_EXPECT_EQ(row("12345678901234567.890123456789", "1.2345678901234568e16"), "True");
}
BEZEL_TEST(decimal_to_double_5298, zero) {
    BEZEL_EXPECT_EQ(row("0", "0.0"), "True");
}
BEZEL_TEST(decimal_to_double_5298, half_plus_tiny) {
    BEZEL_EXPECT_EQ(row("0.5000000000000000000000000001", "0.5"), "True");
}
