// Codegen slice CG1 — an expression-bodied `Main(): int` returns its
// integer-literal value as the process exit code (§33). Lowers integer
// literals (decimal + 0x/0b/0o, `_` separators) and the entry point.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

std::string mainReturning(const std::string& expr) {
    return std::string("module M; class C "
        "public static method Main(): int => ") + expr + "; end C;";
}

} // namespace

BEZEL_TEST(codegen_cg1, returns_literal_42) {
    auto r = runJit(mainReturning("42"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(codegen_cg1, returns_zero) {
    auto r = runJit(mainReturning("0"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_cg1, returns_large_value) {
    auto r = runJit(mainReturning("123"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 123);
}

BEZEL_TEST(codegen_cg1, underscore_separators) {
    auto r = runJit(mainReturning("1_000"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1000);
}

BEZEL_TEST(codegen_cg1, hex_literal) {
    auto r = runJit(mainReturning("0x10"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 16);
}

BEZEL_TEST(codegen_cg1, binary_literal) {
    auto r = runJit(mainReturning("0b101"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// §3.4: static properties on primitive type names (int.MaxValue, int.MinValue).
BEZEL_TEST(codegen_cg1, int_max_value) {
    // int.MaxValue = 2^31 - 1; return 1 to fit in exit code.
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  return (1 if int.MaxValue > 0 else 0); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(codegen_cg1, int_min_value) {
    // int.MinValue = -2^31; must be negative.
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  return (1 if int.MinValue < 0 else 0); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
