// Codegen slice CG25 (subset) — 64-bit `long` integers (§5). An
// `L`-suffixed literal is typed `long` and lowers to i64; long arithmetic
// is overflow-checked at 64 bits and mixed long/int operands promote the
// int to i64 (§6.2). Values beyond the i32 range round-trip correctly.
// Observed through comparisons (the exit code is 32-bit).
//
// (Unsigned types, narrower widths, and context-typing of unsuffixed
// out-of-range literals are deferred — see the literal-typing issue.)

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

BEZEL_TEST(codegen_cg25, long_literal_roundtrips_beyond_i32) {
    BEZEL_EXPECT_EQ(run(
        "var a := 5000000000L; var b := 5000000000L; "
        "return (1 if a = b else 0);"), 1);
}

BEZEL_TEST(codegen_cg25, long_value_exceeds_i32_range) {
    // 5e9 > 2^31-1, so as a real i64 it is positive and > 1e9.
    BEZEL_EXPECT_EQ(run(
        "var a := 5000000000L; return (1 if a > 1000000000L else 0);"), 1);
}

BEZEL_TEST(codegen_cg25, long_addition_no_i32_overflow) {
    // 3e9 + 3e9 = 6e9 fits in i64 (would overflow i32).
    BEZEL_EXPECT_EQ(run(
        "var a := 3000000000L; var b := 3000000000L; var c := a + b; "
        "return (1 if c = 6000000000L else 0);"), 1);
}

BEZEL_TEST(codegen_cg25, long_multiplication) {
    BEZEL_EXPECT_EQ(run(
        "var a := 100000L; var b := 100000L; var c := a * b; "
        "return (1 if c = 10000000000L else 0);"), 1);
}

BEZEL_TEST(codegen_cg25, mixed_long_and_int_promotes) {
    // `a + 1` promotes the int 1 to i64 (§6.2).
    BEZEL_EXPECT_EQ(run(
        "var a := 5000000000L; "
        "return (1 if a + 1 = 5000000001L else 0);"), 1);
}

BEZEL_TEST(codegen_cg25, long_division) {
    BEZEL_EXPECT_EQ(run(
        "var a := 10000000000L; var b := 4L; "
        "return (1 if a / b = 2500000000L else 0);"), 1);
}
