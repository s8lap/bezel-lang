// Regression tests for #5299 (spec §4.3, v0.313): a floating-point literal
// denotes the nearest representable value of its type, rounded as IEEE 754
// rounds to nearest (a tie to the even significand); only a literal whose value
// so rounded is NOT FINITE is a compile error. Underflow (a subnormal or zero
// result) is in range. The literal range check used to key on strtod's
// errno == ERANGE, which glibc also sets on underflow, so `5e-324`, `1e-400`
// and `1e-50f` were rejected as "magnitude exceeds the largest finite double".
//
// Expected values (independent computation, see the scratch note):
//   5e-324   -> 4.94e-324 (double.Epsilon, minimum subnormal)
//   4.9e-324 -> double.Epsilon (nearer to 2^-1074 than to 0 or 2^-1073)
//   2e-324   -> 0   (< 2^-1075 = 2.47e-324, the midpoint to zero)
//   1e-400   -> 0, -1e-400 -> -0
//   1e-45f   -> float.Epsilon (2^-149 = 1.4013e-45); 1e-46f -> 0f.
// The overflow side stays rejected (lit: literal_out_of_range_5299__*).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {
std::string out(const std::string& body) {
    return runAotOut("module M; class C public static method Main(): int " +
                     body + " return 0; end Main; end C;");
}
}  // namespace

BEZEL_TEST(float_literal_underflow_5299, double_5e_324_is_epsilon) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine(5e-324 = double.Epsilon); "), "True");
}
BEZEL_TEST(float_literal_underflow_5299, double_4_9e_324_rounds_to_epsilon) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine(4.9e-324 = double.Epsilon); "), "True");
}
BEZEL_TEST(float_literal_underflow_5299, double_2e_324_rounds_to_zero) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine(2e-324 = 0.0); "), "True");
}
BEZEL_TEST(float_literal_underflow_5299, double_2_5e_324_is_above_half_epsilon) {
    // 2.5e-324 > 2^-1075 (2.4703e-324): rounds up to double.Epsilon.
    BEZEL_EXPECT_EQ(out("Console.WriteLine(2.5e-324 = double.Epsilon); "), "True");
}
BEZEL_TEST(float_literal_underflow_5299, double_1e_400_is_zero) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine((1e-400).ToString()); "), "0");
}
BEZEL_TEST(float_literal_underflow_5299, double_neg_1e_400_is_negative_zero) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine((-1e-400).ToString()); "), "-0");
}
BEZEL_TEST(float_literal_underflow_5299, double_epsilon_prints_5e_324) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine((5e-324).ToString()); "), "5e-324");
}
BEZEL_TEST(float_literal_underflow_5299, double_subnormal_1e_310_in_range) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine((1e-310).ToString()); "), "1e-310");
}
BEZEL_TEST(float_literal_underflow_5299, float_1e_45f_is_float_epsilon) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine(1e-45f = float.Epsilon); "), "True");
}
BEZEL_TEST(float_literal_underflow_5299, float_1e_46f_rounds_to_zero) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine((1e-46f).ToString()); "), "0");
}
BEZEL_TEST(float_literal_underflow_5299, float_1e_50f_rounds_to_zero) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine((1e-50f).ToString()); "), "0");
}
BEZEL_TEST(float_literal_underflow_5299, float_neg_1e_50f_is_negative_zero) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine((-1e-50f).ToString()); "), "-0");
}
BEZEL_TEST(float_literal_underflow_5299, float_epsilon_prints_1e_45) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine((1.4e-45f).ToString()); "), "1e-45");
}
