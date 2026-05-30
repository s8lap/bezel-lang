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
