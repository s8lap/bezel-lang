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

// §4.2 (#749 codegen counterpart): the `long.MinValue` literal
// `-9223372036854775808L` must carry the exact bit pattern 0x8000000000000000.
// parseIntLiteral parsed non-`U` literals with signed strtoll, which SATURATES
// at long.MaxValue for the magnitude 2^63 — so the literal lowered to
// long.MaxValue and `-` made it -(2^63-1) == INT64_MIN+1 (off by one). The
// magnitude is now parsed with strtoull and the negation is constant-folded.
BEZEL_TEST(codegen_cg1, long_min_value_literal_exact_bits) {
    // §5.2 / #753: `ulong(m)` of long.MinValue is now a CHECKED same-width cast
    // and would throw OverflowException (the value is negative). Observe the
    // literal's exact bit pattern via the unchecked reinterpret surface instead:
    // BitConverter.Reinterpret<ulong> of long.MinValue is exactly 2^63.
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  var m := -9223372036854775808L; "
        "  return (0 if BitConverter.Reinterpret<ulong>(m) = 9223372036854775808UL else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §4.2: long.MinValue is the smallest long — strictly less than long.MinValue+1
// (a fold to INT64_MIN+1 would have made this comparison false).
BEZEL_TEST(codegen_cg1, long_min_value_literal_is_smallest) {
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  return (0 if -9223372036854775808L < -9223372036854775807L else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
