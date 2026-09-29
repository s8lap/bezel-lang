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
#include "sema/SymbolCollection.hpp"
#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
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

// Mirror for `compileUncalled` (the runAot-driven helper for fold-fault
// rows that need the §17.14 evaluation-fault path, which the sema-only
// `check()` helper cannot reach — the fold-fault diagnostic is emitted
// from a non-sema pass that only runs in the full AOT pipeline).
bool hasDiagWithAllJit(const bezel::test::JitResult& r,
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
// fix #4844 / §17.14 (v0.307): a `decimal` evaluation fault at a constant
// expression — overflow, division or `mod` by zero, or a `mod` whose scale
// alignment overflows the 96-bit mantissa — is a compile error AT THE
// CONSTANT EXPRESSION, not a deferred runtime fault. The three rows below
// flip the two #4855 rows that previously pinned the silent-accept
// behaviour: with the §17.14 rule applied at the fold, the constant
// expression itself carries the rejection.
//
// Pre-#4844 (the #4855 commitment, when these folded silently to nullopt
// and the conversion stayed a runtime op): the rows asserted
// `r.diagnostics.hasErrors() == false`. Post-#4844 they assert the §17.14
// diagnostic substring unique to each case.
//
// BUG-PIN mechanism: the diagnostic substring is unique to the §17.14
// arm — it cites "§17.14" and "evaluation fault at the constant
// expression" — and a wrong fix (one that only added a warning, or moved
// the diagnostic to the conversion site without the §17.14 wording) would
// fail the needle.
// =====================================================================

namespace {
bezel::test::JitResult compileUncalled(const std::string& expr) {
    return runAot(
        "module M; class C "
        "public static method F(): int return " + expr + "; end F; "
        "public static method Main(): int "
        "Console.WriteLine(\"compiled\"); return 0; end Main; end C;");
}

// True iff ONE error diagnostic carries the §17.14 constant-expression-
// fault marker. Pin: the substring names §17.14, the source class
// (evaluation fault), and the constant-expression location, so the row
// discriminates from the conversion-site §5.2 reject (which cites §5.2 and
// "conversion site").
bool hasSec1714FaultDiag(const bezel::test::JitResult& r,
                         std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        const bool sec17_14 = d.message.find("§17.14") != std::string::npos;
        const bool exprLoc =
            d.message.find("constant expression") != std::string::npos;
        const bool fault = d.message.find("evaluation fault") !=
                           std::string::npos;
        const bool specific = needle.empty() ||
                              d.message.find(needle) != std::string::npos;
        if (sec17_14 && exprLoc && fault && specific) return true;
    }
    return false;
}
}  // namespace

// The two repros from #4844 — `decimal.MaxValue + 1m` overflows and
// `1m / 0m` divides by zero. Both previously compiled and faulted at
// runtime; post-#4844 each carries a §17.14 compile error at the
// constant expression.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           decimal_max_value_plus_one_overflow_rejected_4844) {
    auto r = compileUncalled("long(decimal.MaxValue + 1m)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasSec1714FaultDiag(r, "OverflowException"));
    BEZEL_EXPECT_FALSE(r.ran);  // no exec; compile error
}

BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           one_m_over_zero_rejected_4844) {
    auto r = compileUncalled("int(1m / 0m)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasSec1714FaultDiag(r, "DivideByZeroException"));
    BEZEL_EXPECT_FALSE(r.ran);  // no exec; compile error
}

// narrow #4855 follow-up: `mod` with a zero divisor (the other abort()
// path the #4855 fix declined). Post-#4844 it carries the §17.14
// divide-by-zero diagnostic.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           decimal_mod_zero_divisor_rejected_4844) {
    auto r = compileUncalled("int(5m mod 0m)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasSec1714FaultDiag(r, "DivideByZeroException"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// narrow #4855 follow-up: `mod` with scale alignment that would round the
// divisor down to 0 (`decimal.MaxValue mod 0.4m`). The runtime op
// SIGABRT'd bezelc itself before #4855; #4855 declined the fold; #4844
// reports the §17.14 overflow error at the constant expression.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           decimal_mod_alignment_overflow_rejected_4844) {
    auto r = compileUncalled("int(decimal.MaxValue mod 0.4m)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasSec1714FaultDiag(r, "96-bit decimal mantissa"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ACCEPT control: in-range decimal arithmetic still compiles and folds.
// Pins that the §17.14 arm is targeted, not blanket-rejecting decimal
// arithmetic. The operand chain must not fault at any intermediate step:
// `decimal.MaxValue + 1m` already overflows, so the fold's left-to-right
// evaluation would reject the whole expression. Use a chain whose every
// step is in-range: `decimal.MaxValue - decimal.MaxValue + 1m` evaluates
// as `(decimal.MaxValue - decimal.MaxValue) + 1m = 1m`, which fits. The
// outer `int(...)` then narrows 1m to int and the return type is satisfied.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           decimal_max_value_minus_max_plus_one_compiles_4844) {
    auto r = compileUncalled(
        "int(decimal.MaxValue - decimal.MaxValue + 1m)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("compiled\n"));
}

// =====================================================================
// #4843 / §5.3 / §17.14 (v0.307) — three residuals of #4801 fold.
//
// 1. Imported float / decimal / bool consts (#3230's integer pattern).
// 2. `if` constant expression — (if c then a else b) folds when c, a, b
//    are all §17.14 constant expressions (§17.14's inductive set).
// 3. Nullable primitive cast `T?(x)` — per §5.3, `T?(x)` carries its
//    constant-operand compile error (§5.2, §5.4); the nullable target
//    does NOT lift the §5.2 / §17.14 fault.
// =====================================================================

// --- Sub-issue 2: if constant expression --------------------------------

// ACCEPT control: `if true then 1.0 else 2.0` folds to 1.0 — pins that the
// ConditionalExpr arm of foldNumConstExprTC (fix #4843) admits a literal
// condition. Without the fix the fold path falls through to nullopt and the
// outer explicit conversion `int((if ...))` is NOT a §5.2 compile error (the
// fold never reaches the value-check arm), so the program would compile.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           if_constant_expression_folds_4843) {
    auto r = compileUncalled("int((if true then 1.0 else 2.0))");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("compiled\n"));
}

// REJECT: `if true then 1.0e30 else 0.0` folds to 1.0e30 (a constant
// expression), so `int((if true then 1.0e30 else 0.0))` is a §5.2
// compile error at the conversion site (the constant is out of `int`'s
// range). Without the fix, the if-expression arm of foldNumConstExprTC
// returned nullopt and the §5.2 reject at the explicit conversion never
// fired — the program would compile.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_of_if_constant_out_of_range_rejected_4843) {
    auto r = compileUncalled("int((if true then 1.0e30 else 0.0))");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAllJit(
        r, {"a `double` constant expression whose value is `1e+30`",
            "conversion to `int`", kTrunc}));
}

// --- Sub-issue 3: nullable cast `T?(x)` ---------------------------------

// REJECT: per §5.3, `T?(x)` carries "its constant-operand compile error
// (§5.2, §5.4)". The fold's `int?(1.0e30)` shape: a §5.2 reject at the
// explicit conversion site, mirroring `int(1.0e30)` (which #4801 already
// rejects). The diagnostic must name the nullable-target form and cite
// §5.2 + §17.14. Without the fix (a helper called from the
// NullCondCallExpr handler that mirrors typeCall's primitive-cast arm),
// the Nullable arm of §5.2 silently admits the conversion and the program
// compiles; runtime raises OverflowException on first call.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_question_of_double_out_of_range_rejected_4843) {
    auto r = compileUncalled("int?(1.0e30)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAllJit(
        r, {"a `double` constant expression whose value is `1e+30`",
            "conversion to `int`", kTrunc, "nullable"}));
}

// REJECT: `uint?(-1)` — integer narrowing / same-width signedness change
// at a compile-time-constant integer operand. The non-nullable `uint(-1)`
// is already rejected by typeCall's primitive-cast arm; the nullable
// `uint?(-1)` mirrors that rule per §5.3. Without the fix the program
// compiles and runtime raises OverflowException on first call.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           uint_question_of_negative_one_rejected_4843) {
    auto r = compileUncalled("uint?(-1)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // Pin the §5.2 wording AND the "nullable" marker so a non-nullable
    // arm's diagnostic (which lacks the marker) cannot satisfy the row.
    BEZEL_EXPECT_TRUE(hasDiagWithAllJit(
        r, {"the compile-time-constant operand `-1`",
            "explicit conversion to `uint`", "nullable"}));
}

// REJECT: `byte?(300)` — integer narrowing at a compile-time-constant
// integer operand (300 is out of byte's 0..255 range). Without the fix the
// program compiles; runtime raises OverflowException on first call.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           byte_question_of_out_of_range_rejected_4843) {
    auto r = compileUncalled("byte?(300)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAllJit(
        r, {"the compile-time-constant operand `300`",
            "explicit conversion to `byte`", "nullable"}));
}

// REJECT: `char?(0xD800)` — integer-to-`char` code-point range at a
// constant operand (0xD800 is in the surrogate range U+D800..U+DFFF, which
// §5.2 forbids). The non-nullable `char(0xD800)` is already rejected; the
// nullable `char?(0xD800)` mirrors that rule per §5.3.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           char_question_of_surrogate_rejected_4843) {
    auto r = compileUncalled("char?(0xD800)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAllJit(
        r, {"the compile-time-constant operand `55296`",
            "is not a valid Unicode code point", "nullable"}));
}

// ACCEPT control: `int?(42)!` — in-range nullable integer narrowing
// compiles and unwraps to 42. Pins that the §5.3 arm of #4843 is
// targeted, not blanket-rejecting nullable primitive casts. The `!`
// postfix unwraps the nullable to its underlying `int` (§7.4); since
// 42 is in `int`'s range the §5.2 reject never fires.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           int_question_of_in_range_compiles_4843) {
    auto r = compileUncalled("int?(42)!");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("compiled\n"));
}

// --- Sub-issue 1: imported float / decimal / bool consts -----------------
//
// The ModuleLoader-populated typed-value channels (`floatValue`,
// `decimalValue`, `boolValue`, `declaredPrimitiveType` on
// `ConstMemberSymbol`) carry the typed value for an IMPORTED
// `const K: T;` because the symbol has no AST initializer (the loader
// reconstructs it from dependency metadata, fix #3230's pattern).
// Without the fix, the loader drops `MetaMember::valueText` on the floor
// for a non-integer primitive and the channels the fold reads are empty.
//
// This row is a STRUCTURAL test on the loader — it builds a dependency
// module's metadata, loads it into a fresh importer context, and asserts
// that `C.K`'s `ConstMemberSymbol` carries the typed `double` value
// `1.0e30` (the same one the local `CK` test reads through the AST
// path). The full fold → §5.2 reject path cannot be exercised here
// because the cross-module qualified-const reach is out of scope (filed
// as #2986 — the fold's MemberAccessExpr arm only resolves a local
// `C.K`, not a `Lib.C.K`); the loader side is what #4843 actually
// touches.
BEZEL_TEST(s5_2_float_constant_expr_folding_4801,
           imported_double_const_float_value_populated_4843) {
    using bezel::DiagnosticEngine;
    using bezel::Lexer;
    using bezel::Parser;
    DiagnosticEngine diags;
    auto libSrc = std::string(
        "module Lib; "
        "public class C "
        "  public static const K: double := 1.0e30; "
        "end C;");
    Lexer lexer(libSrc, diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(sf != nullptr);
    bezel::sema::SemaContext libCtx(diags);
    bezel::sema::collectSymbols(*sf, libCtx);
    bezel::sema::resolveTypes(*sf, libCtx);
    bezel::sema::typecheckExpressions(*sf, libCtx);
    BEZEL_EXPECT_FALSE(diags.hasErrors());
    auto dep = bezel::module::buildModuleMetadata(libCtx, libSrc);

    auto importerSrc = std::string(
        "module App; "
        "import Lib; "
        "public class M "
        "  public static method Main(): int return 0; end Main; "
        "end M;");
    DiagnosticEngine impDiags;
    Lexer impLexer(importerSrc, impDiags);
    Parser impParser(impLexer, impDiags);
    auto impSf = impParser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(impSf != nullptr);
    bezel::sema::SemaContext impCtx(impDiags);
    bezel::sema::collectSymbols(*impSf, impCtx);
    bezel::module::loadMetadata(dep, impCtx);
    bezel::sema::resolveTypes(*impSf, impCtx);
    BEZEL_EXPECT_FALSE(impDiags.hasErrors());
    // Find the imported `C` class via the importer's
    // `importedByModule_` table (the loader registers the dependency's
    // type symbols there under the module's dotted name, §2.8.3 rule 3).
    // Its `K` member is a `ConstMemberSymbol` whose `floatValue` must
    // carry `1.0e30` (the typed value the loader reads from
    // `MetaMember::valueText`).
    auto* libTypes = impCtx.importedTypesOf("Lib");
    BEZEL_EXPECT_TRUE(libTypes != nullptr);
    bool foundK = false;
    if (libTypes) {
        for (auto* ts : *libTypes) {
            if (!ts || ts->name != "C") continue;
            for (auto* sym : ts->members) {
                auto* cms = dynamic_cast<bezel::sema::ConstMemberSymbol*>(sym);
                if (!cms || cms->name != "K") continue;
                foundK = true;
                BEZEL_EXPECT_TRUE(cms->floatValue.has_value());
                if (cms->floatValue) {
                    // The textual carrier is rounded to the destination
                    // precision (Double here, so no rounding); 1.0e30
                    // fits in a double exactly.
                    BEZEL_EXPECT_TRUE(*cms->floatValue == 1.0e30);
                }
                BEZEL_EXPECT_TRUE(cms->declaredPrimitiveType.has_value());
                if (cms->declaredPrimitiveType) {
                    BEZEL_EXPECT_TRUE(*cms->declaredPrimitiveType ==
                                      bezel::sema::PrimitiveType::Double);
                }
                break;
            }
            break;
        }
    }
    BEZEL_EXPECT_TRUE(foundK);
}
