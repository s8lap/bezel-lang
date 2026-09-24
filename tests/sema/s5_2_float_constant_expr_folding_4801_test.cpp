// fix #4801 / §5.2 / §17.14 (v0.307): a floating-point or `decimal`
// COMPILE-TIME-CONSTANT operand (§17.14) of an explicit conversion that would
// raise `OverflowException` is a compile error at the conversion site — not
// only a bare literal (#4724), but every constant expression §17.14 admits:
// `const` references (local, class-level, qualified), the primitive-wrapper
// constants (`double.MaxValue`, `double.NaN`, `decimal.MaxValue`, …), unary
// `+`/`-` at any depth, and binary arithmetic on constants.
//
// Per §5.2 (v0.307): "where the operand of an explicit conversion is a
// compile-time constant (§17.14) whose value would raise `OverflowException`
// under any rule of this section — … and the floating-point and `decimal`
// conversions below — the conversion is a compile error at the conversion
// site"; "Floating-point to integer truncates toward zero; `NaN`,
// `±Infinity`, and out-of-range values raise `OverflowException`"; and
// "`decimal(x)` for binary-float `x` … `NaN`, `±Infinity`, and magnitudes
// outside `decimal`'s range raise `OverflowException`". Per §17.14 the
// operand is evaluated under run-time semantics: §6.3 IEEE 754 (overflow to
// ±Infinity and NaN are VALUES — so `0.0 / 0.0` is a NaN operand, which the
// conversion then rejects), §6.4 decimal.
//
// Every REJECT row asserts the diagnostic substring unique to its case (the
// folded value and the reason clause), not just "constant". Every ACCEPT row
// is a runAot VALUE row: compile, run, and compare exact stdout, so a false
// reject (no run) and a wrong fold both fail. The non-constant control keeps
// the run-time OverflowException.
//
// Authority: spec §5.2 / §17.14 / §6.3 / §6.4 / §4.3 / §4.4, stdlib §9.2 /
// §9.3, v0.307.

#include <string_view>

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff ONE error diagnostic carries every needle.
bool hasDiagWithAll(const bezel::test::SemaResult& r,
                    std::initializer_list<std::string_view> needles) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        bool all = true;
        for (auto n : needles)
            if (d.message.find(n) == std::string::npos) all = false;
        if (all) return true;
    }
    return false;
}

// Any diagnostic that mentions OverflowException at all — the broad probe for
// the NO-cascade rows (a narrow needle would make an EXPECT_FALSE vacuous).
bool hasAnyOverflowDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("OverflowException") != std::string::npos)
            return true;
    return false;
}

const char* kHeader =
    "module M; class C "
    "public static const CK: double := 1.0e30; "
    "public static const CV: double := 42.9; "
    "public static method Main(): int ";
const char* kFooter = " return 0; end Main; end C;";

bezel::test::SemaResult check(const std::string& body) {
    return parseSemaAll(std::string(kHeader) + body + kFooter);
}

// The §5.2 reason clauses the fix #4801 diagnostic appends.
constexpr std::string_view kNonFinite = "have no integer value";
constexpr std::string_view kTrunc = "after truncation toward zero";
constexpr std::string_view kNoDecimal = "have no `decimal` value";

// Compile and run `Console.WriteLine(<expr>);` after `prelude`.
bezel::test::JitResult runPrint(const std::string& prelude,
                                const std::string& expr) {
    return runAot(std::string(kHeader) + prelude + "Console.WriteLine(" +
                  expr + ");" + kFooter);
}

}  // namespace

// =====================================================================
// REJECT — the seven shapes of issue #4801.
// =====================================================================

// §6.3: `0.0 / 0.0` is NaN (a value, not a fault); §5.2: NaN → int raises.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_zero_over_zero_nan_rejected) {
    auto r = check("var x: int := int(0.0 / 0.0);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(
        r, {"a `double` constant expression whose value is `NaN`",
            "conversion to `int`", kNonFinite}));
}

// §6.3: `1.0e308 * 10.0` overflows to +Infinity; §5.2: ±Infinity raises.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_overflowing_product_infinity_rejected) {
    auto r = check("var x: int := int(1.0e308 * 10.0);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(
        r, {"a `double` constant expression whose value is `Infinity`",
            kNonFinite}));
}

// §17.14: unary `-` at any depth — `-(-1.0e30)` is 1e30.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_double_negation_rejected) {
    auto r = check("var x: int := int(-(-1.0e30));");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(
        r, {"a `double` constant expression whose value is `1e+30`",
            kTrunc}));
}

// §17.14 / stdlib §9.2: `double.MaxValue` is a compiler-provided constant.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_double_max_value_rejected) {
    auto r = check("var x: int := int(double.MaxValue);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(
        r, {"whose value is `1.7976931348623157e+308`", kTrunc}));
}

// stdlib §9.2: `double.NaN`.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_double_nan_rejected) {
    auto r = check("var x: int := int(double.NaN);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {"whose value is `NaN`", kNonFinite}));
}

// §17.14: a local `const` reference.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_local_double_const_rejected) {
    auto r = check("const K: double := 1.0e30; var x: int := int(K);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {"whose value is `1e+30`", kTrunc}));
}

// §5.2: float → decimal outside decimal's range raises.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           decimal_of_double_beyond_decimal_range_rejected) {
    auto r = check("var x: decimal := decimal(1.0e30);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(
        r, {"whose value is `1e+30`", "conversion to `decimal`",
            kNoDecimal}));
}

// =====================================================================
// REJECT — further §17.14 forms.
// =====================================================================

// A class-level const, by bare name and qualified.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_class_double_const_rejected) {
    auto r = check("var x: int := int(CK);");
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {"whose value is `1e+30`", kTrunc}));
}

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_qualified_class_double_const_rejected) {
    auto r = check("var x: int := int(C.CK);");
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {"whose value is `1e+30`", kTrunc}));
}

// stdlib §9.3: `decimal.MaxValue`, folded exactly.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           long_of_decimal_max_value_rejected) {
    auto r = check("var x: long := long(decimal.MaxValue);");
    BEZEL_EXPECT_TRUE(hasDiagWithAll(
        r, {"a `decimal` constant expression whose value is "
            "`79228162514264337593543950335`",
            "conversion to `long`", kTrunc}));
}

// §6.4 exact decimal arithmetic: 1.5m * 200m = 300.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           byte_of_decimal_product_rejected) {
    auto r = check("var x: byte := byte(1.5m * 200m);");
    BEZEL_EXPECT_TRUE(hasDiagWithAll(
        r, {"a `decimal` constant expression whose value is `300`", kTrunc}));
}

// A `float` node computes in single precision: float.MaxValue * 2.0f is
// +Infinity in float (it would be finite in double).
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_float_overflow_single_precision_rejected) {
    auto r = check("var x: int := int(float.MaxValue * 2.0f);");
    BEZEL_EXPECT_TRUE(hasDiagWithAll(
        r, {"a `float` constant expression whose value is `Infinity`",
            kNonFinite}));
}

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           long_of_negative_infinity_rejected) {
    auto r = check("var x: long := long(-double.PositiveInfinity);");
    BEZEL_EXPECT_TRUE(hasDiagWithAll(
        r, {"whose value is `-Infinity`", kNonFinite}));
}

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           decimal_of_double_nan_rejected) {
    auto r = check("var x: decimal := decimal(double.NaN);");
    BEZEL_EXPECT_TRUE(hasDiagWithAll(r, {"whose value is `NaN`", kNoDecimal}));
}

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           decimal_of_float_max_value_rejected) {
    auto r = check("var x: decimal := decimal(float.MaxValue);");
    BEZEL_EXPECT_TRUE(hasDiagWithAll(
        r, {"a `float` constant expression whose value is `3.4028234663852886e+38`",
            kNoDecimal}));
}

// A literal that already failed §4.3 formation (`1e400`) gives the operand
// no value: exactly the formation error, no cascaded conversion error.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           formation_overflow_inside_expression_not_rediagnosed) {
    auto r = check("var x: int := int(1e400 * 1.0);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAnyOverflowDiag(r));
}

// =====================================================================
// ACCEPT — in-range constant expressions compile and print the value.
// =====================================================================

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_product_runtime_3) {
    auto r = runPrint("", "int(1.5 * 2.0)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_double_negation_runtime_7) {
    auto r = runPrint("", "int(-(-7.9))");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_local_const_runtime_42) {
    auto r = runPrint("const K: double := 42.9; ", "int(K)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_qualified_class_const_runtime_42) {
    auto r = runPrint("", "int(C.CV)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// 1.7976931348623157e308 / 1e300 = 179769313.48623157 → 179769313.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           long_of_scaled_double_max_runtime_179769313) {
    auto r = runPrint("", "long(double.MaxValue / 1e300)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("179769313\n"));
}

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           decimal_of_double_in_range_runtime_1_5) {
    auto r = runPrint("", "decimal(1.5)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1.5\n"));
}

// §6.3: 0.0 / 1.0 is 0.0 (not a division by zero).
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           byte_of_zero_quotient_runtime_0) {
    auto r = runPrint("", "byte(0.0 / 1.0)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// Exact decimal: decimal.MaxValue / 1e10m truncates to 7922816251426433759,
// inside long's range.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           long_of_decimal_quotient_runtime_exact) {
    auto r = runPrint("", "long(decimal.MaxValue / 1e10m)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7922816251426433759\n"));
}

// §6.1: `mod` on decimal follows the dividend's sign: 7.5m mod 2m = 1.5.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_decimal_mod_runtime_1) {
    auto r = runPrint("", "int(7.5m mod 2m)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// §6.2: the int operand promotes to float; 2.5f * 4 = 10.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_float_const_times_int_runtime_10) {
    auto r = runPrint("const KF: float := 2.5f; ", "int(KF * 4)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n"));
}

// 1e19 is above long.MaxValue but inside ulong's range.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           ulong_of_double_sum_runtime_1e19) {
    auto r = runPrint("", "ulong(1.0e19 + 0.0)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10000000000000000000\n"));
}

// =====================================================================
// NON-CONSTANT control: a `var` read is not a §17.14 constant expression,
// so the conversion compiles and raises OverflowException at run time.
// =====================================================================

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_double_local_var_raises_at_runtime) {
    auto r = runAot(std::string(kHeader) +
                    "var d: double := 1e30; "
                    "try Console.WriteLine(int(d)); "
                    "catch OverflowException => "
                    "Console.WriteLine(\"caught OverflowException\"); "
                    "end;" +
                    kFooter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("caught OverflowException\n"));
}

// =====================================================================
// narrow #4855: the fold must never call a runtime decimal op that can
// abort(). bz_decimal_mod aligns scales first; with a divisor that carries
// more scale than a dividend too large to upscale, the alignment rounds the
// DIVISOR down to 0 and the runtime aborts — so `decimal.MaxValue mod 0.4m`
// used to SIGABRT bezelc itself. The conversion sits in a method Main never
// calls: the row pins that COMPILATION survives (the run-time abort of the
// same op is #4855's runtime half, out of scope here), and a compiler crash
// is fork-isolated by runAot as ran == false.
// =====================================================================

namespace {
bezel::test::JitResult compileUncalled(const std::string& expr) {
    return runAot(
        "module M; class C "
        "public static method F(): int return " + expr + "; end F; "
        "public static method Main(): int "
        "Console.WriteLine(\"compiled\"); return 0; end Main; end C;");
}
}  // namespace

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           decimal_mod_alignment_zeroing_divisor_does_not_abort_compiler_4855) {
    auto r = compileUncalled("int(decimal.MaxValue mod 0.4m)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("compiled\n"));
}

// The other two abort() paths among the ops the fold calls: bz_decimal_div
// and bz_decimal_mod on a literal zero divisor (declined before the call).
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           decimal_div_and_mod_by_zero_do_not_abort_compiler_4855) {
    auto r = compileUncalled("int(1m / 0m) + int(5m mod 0m) + int(5m mod 0.0m)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("compiled\n"));
}
