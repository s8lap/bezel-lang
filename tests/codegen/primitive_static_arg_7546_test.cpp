// Regression tests for #7546: a primitive static member whose stdlib signature
// is declared never had its arguments checked against the declared parameter
// types. `Float.IeeeEquals(d, d)` over `double` operands compiled (codegen, since
// #7274, narrowed them silently) although stdlib §9.2 (v0.350) declares
// `IeeeEquals(a: float, b: float)` and spec §5.1 (v0.350) has no implicit
// `double` -> `float` conversion (`var f: float := d` is rejected). The same
// gap covered the other `Double` / `Float` statics (`IsNaN`, `IsInfinity`,
// `IsPositiveInfinity`, `IsNegativeInfinity`, `IsFinite`, `BitwiseEquals`) and
// the `char` statics (stdlib §9.5: `IsDigit` ... `ToLower`). The Parse / TryParse /
// ParseOrNull / ToString rows of every primitive and `Math` / `Convert` were
// already checked (#7271, #7421). The rows below pin one illegal call per
// newly declared member, and the legal widenings of §5.1 (`int`, `long`, `char`
// -> `float` / `double`; `float` -> `double`) as controls with exact stdout.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

void expectRejected(const std::string& decls, const std::string& expr,
                    const char* needle) {
    std::string src = "module M; import System; class C "
                      "  public static method Main(): int " + decls +
                      "    var r := " + expr + "; return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

void expectOutput(const std::string& body, const std::string& expected) {
    std::string src = "module M; import System; class C "
                      "  public static method Main(): int " + body +
                      "    return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

BEZEL_TEST(primitive_static_arg_7546, reject_float_ieee_equals_double_a_5_1) {
    expectRejected("var d: double := 1.5;", "Float.IeeeEquals(d, d)",
                   "`Float.IeeeEquals` — stdlib §9.2 declares `IeeeEquals(a: float, b: float): bool`, so the `a` argument must be a `float`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_float_ieee_equals_double_b_5_1) {
    expectRejected("var f: float := 1.5f; var d: double := 1.5;", "Float.IeeeEquals(f, d)",
                   "so the `b` argument must be a `float`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_float_bitwise_equals_double_literal_5_1) {
    expectRejected("", "Float.BitwiseEquals(1.5, 2.5f)",
                   "so the `a` argument must be a `float`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_double_ieee_equals_string_5_1) {
    expectRejected("var s: string := \"x\"; var d: double := 1.5;", "Double.IeeeEquals(d, s)",
                   "so the `b` argument must be a `double`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_double_bitwise_equals_bool_5_1) {
    expectRejected("var d: double := 1.5;", "Double.BitwiseEquals(true, d)",
                   "so the `a` argument must be a `double`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_float_is_nan_double_5_1) {
    expectRejected("var d: double := 1.5;", "Float.IsNaN(d)",
                   "so the `value` argument must be a `float`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_float_is_finite_double_literal_5_1) {
    expectRejected("", "Float.IsFinite(3.0)",
                   "so the `value` argument must be a `float`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_float_is_infinity_double_5_1) {
    expectRejected("var d: double := 1.5;", "Float.IsInfinity(d)",
                   "so the `value` argument must be a `float`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_float_is_positive_infinity_double_5_1) {
    expectRejected("var d: double := 1.5;", "Float.IsPositiveInfinity(d)",
                   "so the `value` argument must be a `float`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_float_is_negative_infinity_double_5_1) {
    expectRejected("var d: double := 1.5;", "Float.IsNegativeInfinity(d)",
                   "so the `value` argument must be a `float`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_double_is_nan_string_5_1) {
    expectRejected("var s: string := \"x\";", "Double.IsNaN(s)",
                   "so the `value` argument must be a `double`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_double_is_finite_bool_5_1) {
    expectRejected("", "Double.IsFinite(true)",
                   "so the `value` argument must be a `double`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_double_is_infinity_decimal_5_1) {
    expectRejected("var m: decimal := 1.5m;", "Double.IsInfinity(m)",
                   "so the `value` argument must be a `double`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_double_is_positive_infinity_string_5_1) {
    expectRejected("var s: string := \"x\";", "Double.IsPositiveInfinity(s)",
                   "so the `value` argument must be a `double`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_double_is_negative_infinity_string_5_1) {
    expectRejected("var s: string := \"x\";", "Double.IsNegativeInfinity(s)",
                   "so the `value` argument must be a `double`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_double_is_nan_wrong_arity_5_1) {
    expectRejected("", "Double.IsNaN()",
                   "supplies 0 argument(s)");
}

BEZEL_TEST(primitive_static_arg_7546, reject_char_is_digit_int_5_1) {
    expectRejected("var i: int := 5;", "char.IsDigit(i)",
                   "so the `c` argument must be a `char`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_char_is_letter_double_pre_existing_control_5_1) {
    expectRejected("var d: double := 1.5;", "char.IsLetter(d)",
                   "requires a `char` argument");
}

BEZEL_TEST(primitive_static_arg_7546, reject_char_is_letter_or_digit_long_5_1) {
    expectRejected("var l: long := 5;", "char.IsLetterOrDigit(l)",
                   "so the `c` argument must be a `char`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_char_is_white_space_bool_pre_existing_control_5_1) {
    expectRejected("", "char.IsWhiteSpace(true)",
                   "requires a `char` argument");
}

BEZEL_TEST(primitive_static_arg_7546, reject_char_is_upper_int_5_1) {
    expectRejected("var i: int := 65;", "char.IsUpper(i)",
                   "so the `c` argument must be a `char`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_char_is_lower_int_5_1) {
    expectRejected("var i: int := 97;", "char.IsLower(i)",
                   "so the `c` argument must be a `char`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_char_is_control_int_5_1) {
    expectRejected("var i: int := 7;", "char.IsControl(i)",
                   "so the `c` argument must be a `char`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_char_is_punctuation_int_5_1) {
    expectRejected("var i: int := 33;", "char.IsPunctuation(i)",
                   "so the `c` argument must be a `char`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_char_to_upper_int_5_1) {
    expectRejected("var i: int := 97;", "char.ToUpper(i)",
                   "so the `c` argument must be a `char`");
}

BEZEL_TEST(primitive_static_arg_7546, reject_char_to_lower_int_5_1) {
    expectRejected("var i: int := 65;", "char.ToLower(i)",
                   "so the `c` argument must be a `char`");
}

BEZEL_TEST(primitive_static_arg_7546, accept_double_member_float_int_long_char_widen_5_1) {
    expectOutput("Console.WriteLine(Double.IsNaN(1.5f)); Console.WriteLine(Double.IsFinite(3)); Console.WriteLine(Double.IsInfinity(5L)); Console.WriteLine(Double.IsPositiveInfinity('a'));",
                 "False\nTrue\nFalse\nFalse");
}

BEZEL_TEST(primitive_static_arg_7546, accept_double_member_equality_mixed_legal_5_1) {
    expectOutput("var f: float := 2.5f; var l: long := 4L; Console.WriteLine(Double.IeeeEquals(f, 2.5)); Console.WriteLine(Double.IeeeEquals(l, 4)); Console.WriteLine(Double.BitwiseEquals(l, 4.0));",
                 "True\nTrue\nTrue");
}

BEZEL_TEST(primitive_static_arg_7546, accept_float_member_int_long_uint_literal_widen_5_1) {
    expectOutput("var l: long := 8L; var u: uint := 16u; Console.WriteLine(Float.IsNaN(3)); Console.WriteLine(Float.IsFinite(l)); Console.WriteLine(Float.IeeeEquals(2, 2.0f)); Console.WriteLine(Float.BitwiseEquals(u, 16.0f)); Console.WriteLine(Float.IsInfinity(1.0f)); Console.WriteLine(Float.IsPositiveInfinity(Float.PositiveInfinity)); Console.WriteLine(Float.IsNegativeInfinity(Float.NegativeInfinity));",
                 "False\nTrue\nTrue\nTrue\nFalse\nTrue\nTrue");
}

BEZEL_TEST(primitive_static_arg_7546, accept_double_member_negative_infinity_and_nan_9_2) {
    expectOutput("var n: double := Double.NaN; Console.WriteLine(Double.IsNegativeInfinity(Double.NegativeInfinity)); Console.WriteLine(Double.IsNaN(n));",
                 "True\nTrue");
}

BEZEL_TEST(primitive_static_arg_7546, accept_char_members_char_argument_9_5) {
    expectOutput("Console.WriteLine(char.IsDigit('7')); Console.WriteLine(char.IsLetter('7')); Console.WriteLine(char.IsLetterOrDigit('x')); Console.WriteLine(char.IsWhiteSpace(' ')); Console.WriteLine(char.IsUpper('A')); Console.WriteLine(char.IsLower('A')); Console.WriteLine(char.IsControl('a')); Console.WriteLine(char.IsPunctuation('!')); Console.WriteLine(char.ToUpper('a')); Console.WriteLine(char.ToLower('Q'));",
                 "True\nFalse\nTrue\nTrue\nTrue\nFalse\nFalse\nTrue\nA\nq");
}
