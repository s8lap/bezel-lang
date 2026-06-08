// Codegen slice CG24 — float / double (§5, §6.3). Float/double literals,
// IEEE arithmetic (fadd/fsub/fmul/fdiv, fneg) and ordered comparisons,
// plus mixed int/float promotion (§6.2). No overflow/div0 checks (IEEE).
// Results are observed through comparisons that yield an int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg24, double_comparison) {
    BEZEL_EXPECT_EQ(run(
        "var x := 1.5; var y := 2.5; return (1 if x < y else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, double_addition) {
    BEZEL_EXPECT_EQ(run(
        "var x := 1.5; var y := 2.5; return (1 if x + y = 4.0 else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, double_subtraction) {
    BEZEL_EXPECT_EQ(run(
        "var x := 5.0; var y := 1.25; "
        "return (1 if x - y = 3.75 else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, double_multiplication) {
    BEZEL_EXPECT_EQ(run(
        "var x := 1.5; var y := 4.0; return (1 if x * y = 6.0 else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, double_division) {
    BEZEL_EXPECT_EQ(run(
        "var x := 9.0; var y := 2.0; return (1 if x / y = 4.5 else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, double_negation) {
    BEZEL_EXPECT_EQ(run(
        "var x := 3.0; return (1 if -x < 0.0 else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, mixed_int_promoted_to_double) {
    // `x + 1` promotes the int 1 to double (§6.2).
    BEZEL_EXPECT_EQ(run(
        "var x := 2.5; return (1 if x + 1 = 3.5 else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, division_is_not_integer_truncated) {
    // 7.0 / 2.0 = 3.5 (not 3): confirms FP, not integer, division.
    BEZEL_EXPECT_EQ(run(
        "var x := 7.0; var y := 2.0; return (1 if x / y > 3.4 else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, float_literal_suffix) {
    // An `F`-suffixed literal is a float; arithmetic still works.
    BEZEL_EXPECT_EQ(run(
        "var x := 2.0F; return (1 if x * x = 4.0F else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, no_overflow_check_on_float) {
    // A huge float product yields +inf, not an exception (IEEE, §6.3).
    BEZEL_EXPECT_EQ(run(
        "var x := 1.0e300; return (1 if x * x > x else 0);"), 1);
}

// §6.2: a `float` mixed with a `double` widens the float to double — the
// operands must share a width before the FP op (previously an f32 and an
// f64 reached the comparison with mismatched types).
BEZEL_TEST(codegen_cg24, float_double_mixed_comparison) {
    BEZEL_EXPECT_EQ(run("var d := 3.5; var f := float(d); "
                        "return (1 if f = 3.5 else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, float_double_mixed_arithmetic) {
    BEZEL_EXPECT_EQ(run("var f := float(2.5); var d := 1.0; "
                        "return (1 if f + d = 3.5 else 0);"), 1);
}

// §4.3.1 (L308/L314): "NaN = NaN is true" — reflexive equality, deliberate
// (needed for Set<double>/Dictionary key semantics, and `case` expressions).
// `<>` is the negation of `=`, so `NaN <> NaN` is false.
// IEEE <,<=,>,>= comparisons on NaN remain false (unchanged).
// These tests were updated from the pre-fix expected values when #514 was fixed.
BEZEL_TEST(codegen_cg24, nan_equality_is_true_spec_4_3_1) {
    // §4.3.1: NaN = NaN must be true (reflexive equality).
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; "
                        "return (1 if nan = nan else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, nan_inequality_is_false_spec_4_3_1) {
    // §4.3.1: NaN <> NaN is false (negation of `=` which is true).
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; "
                        "return (1 if nan <> nan else 0);"), 0);
}

BEZEL_TEST(codegen_cg24, non_nan_inequality_unchanged) {
    BEZEL_EXPECT_EQ(run("var a := 3.0; var b := 4.0; "
                        "return (1 if a <> b else 0);"), 1);
}

// §18.1: the `f`/`F` suffix gives type `float` (f32); no suffix or `d`/`D`
// gives `double` (f64). After this fix, float literals with `f` suffix are
// correctly typed as float (not double) in sema, so they lower to f32 values.

BEZEL_TEST(codegen_cg24, float_suffix_typing) {
    // 1.5f is f32, not f64.
    BEZEL_EXPECT_EQ(run("var x := 1.5f; return (1 if x = 1.5f else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, float_record_equality) {
    // Record with float fields: equality uses FCmpOEQ on f32 values.
    // Use runJit directly so we can define a record at module scope.
    auto r = runJit(
        "module M; record F(x: float, y: float); "
        "class C public static method Main(): int "
        "  var a := new F(1.0f, 2.0f); var b := new F(1.0f, 2.0f); "
        "  return (1 if a = b else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

BEZEL_TEST(codegen_cg24, float_record_inequality) {
    // Two float records with different y field compare unequal.
    auto r = runJit(
        "module M; record F(x: float, y: float); "
        "class C public static method Main(): int "
        "  var a := new F(1.0f, 2.0f); var b := new F(1.0f, 2.5f); "
        "  return (1 if a <> b else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// §3.4: static properties on primitive type names.
// §4.3.1: NaN = NaN is true (reflexive equality) — test updated from
// the pre-#514-fix value (was 0 per IEEE) to 1 per spec §4.3.1 (L308).
BEZEL_TEST(codegen_cg24, double_nan_equals_itself_reflexive_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var x := double.NaN; return (1 if x = x else 0);"), 1);
}

BEZEL_TEST(codegen_cg24, double_positive_infinity_greater_than_zero) {
    BEZEL_EXPECT_EQ(run("var x := double.PositiveInfinity; return (1 if x > 0.0 else 0);"), 1);
}
