// Regression tests for #7274: a `float` operand at `Double.IeeeEquals` /
// `Double.BitwiseEquals` emitted an fcmp / bitcast over MISMATCHED types, so the compiler aborted
// with "IR verification failed".
//
// stdlib §9.2 (v0.350) declares `IeeeEquals(a: double, b: double): bool` and
// `BitwiseEquals(a: double, b: double): bool` (and the `float` pair on
// `Float`). spec §5.1 (v0.350): a `float` widens to `double`; spec §5.2
// (v0.350): "Such a cast is a *value* conversion, not a reinterpretation of
// bits: it preserves the numeric value or fails." So each operand converts BY
// VALUE to the member's own parameter width before the comparison.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;

namespace {

void expectValue(const std::string& body, const std::string& expected) {
    std::string src = "module M; import System; class C "
                      "  public static method Main(): int " + body +
                      "    return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// The issue's repro: uint + float at the Double member (aborted pre-fix).
BEZEL_TEST(ieee_equals_float_width_7274, double_member_uint_and_float_variables_5_1) {
    expectValue("    var u: uint := 4000000000u; var f: float := 4000000000.0f; "
                "    Console.WriteLine(Double.IeeeEquals(u, f));",
                "True");
}

// The quiet half: both operands share a width but not the member's.
BEZEL_TEST(ieee_equals_float_width_7274, double_member_uint_literal_and_float_literal_5_1) {
    expectValue("    Console.WriteLine(Double.IeeeEquals(200u, 200.0f));", "True");
}

BEZEL_TEST(ieee_equals_float_width_7274, double_member_float_and_double_literal_5_1) {
    expectValue("    Console.WriteLine(Double.IeeeEquals(200.0f, 200.0));", "True");
}

BEZEL_TEST(ieee_equals_float_width_7274, double_member_bitwise_float_and_double_variable_5_1) {
    expectValue("    var f: float := 4.0f; var d: double := 4.0; "
                "    Console.WriteLine(Double.BitwiseEquals(f, d));",
                "True");
}

// The Float member with legal operands (uint widens to float, §5.1).
BEZEL_TEST(ieee_equals_float_width_7274, float_member_uint_and_float_variables_5_1) {
    expectValue("    var u: uint := 16777216u; var f: float := 16777216.0f; "
                "    Console.WriteLine(Float.IeeeEquals(u, f));",
                "True");
}

// Mirrors, green on both arms: same-width operands, NaN and signed zero.
BEZEL_TEST(ieee_equals_float_width_7274, same_width_mirror_nan_and_signed_zero_9_2) {
    expectValue("    var z: double := 0.0; var nz: double := -0.0; var n: double := Double.NaN; "
                "    var f: float := 4.0f; "
                "    Console.WriteLine(Double.IeeeEquals(z, nz)); "
                "    Console.WriteLine(Double.BitwiseEquals(z, nz)); "
                "    Console.WriteLine(Double.IeeeEquals(n, n)); "
                "    Console.WriteLine(Float.IeeeEquals(f, f)); "
                "    Console.WriteLine(Double.IeeeEquals(4000000000u, 4e9));",
                "True\nFalse\nFalse\nTrue\nTrue");
}
