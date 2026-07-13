// Codegen slice CG1 — an expression-bodied `Main(): int` returns its
// integer-literal value as the process exit code (§33). Lowers integer
// literals (decimal + 0x/0b/0o, `_` separators) and the entry point.

#include "support/aot_support.hpp"   // runAot/runAotOut — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string mainReturning(const std::string& expr) {
    return std::string("module M; class C "
        "public static method Main(): int => ") + expr + "; end C;";
}

// A process exit status is 8-bit (WEXITSTATUS masks to 0..255) — a value
// outside that range must round-trip through stdout instead (see
// aot_support.hpp "TWO SEMANTIC DIFFERENCES", #1). Prints `expr` and
// returns 0, for use with runAotOut.
std::string mainPrinting(const std::string& expr) {
    return std::string("module M; class C "
        "public static method Main(): int Console.WriteLine(") + expr +
        "); return 0; end Main; end C;";
}

} // namespace

BEZEL_TEST(codegen_cg1, returns_literal_42) {
    auto r = runAot(mainReturning("42"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(codegen_cg1, returns_zero) {
    auto r = runAot(mainReturning("0"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_cg1, returns_large_value) {
    auto r = runAot(mainReturning("123"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 123);
}

// 1000 exceeds the 8-bit exit-code range (class C, aot_support.hpp #1) — the
// value is observed via stdout instead of the exit code.
BEZEL_TEST(codegen_cg1, underscore_separators) {
    BEZEL_EXPECT_EQ(runAotOut(mainPrinting("1_000")), "1000");
}

BEZEL_TEST(codegen_cg1, hex_literal) {
    auto r = runAot(mainReturning("0x10"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 16);
}

BEZEL_TEST(codegen_cg1, binary_literal) {
    auto r = runAot(mainReturning("0b101"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// §3.4: static properties on primitive type names (int.MaxValue, int.MinValue).
BEZEL_TEST(codegen_cg1, int_max_value) {
    // int.MaxValue = 2^31 - 1; return 1 to fit in exit code.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  return (1 if int.MaxValue > 0 else 0); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(codegen_cg1, int_min_value) {
    // int.MinValue = -2^31; must be negative.
    auto r = runAot(
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
    auto r = runAot(
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
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  return (0 if -9223372036854775808L < -9223372036854775807L else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #1155 / §4.2: a suffixed integer literal names its type, and its value must
// be representable in that type's range. A value that exceeds the suffix's
// range was once silently accepted and wrapped (`9223372036854775808L` (2^63,
// > long.MaxValue) → long.MinValue; `5000000000U` (> uint.MaxValue) → wrapped
// uint). It must now be a clean compile error, not a silent wrap.

// True iff some Error diagnostic's message contains `substr`.
namespace {
bool anyErrorContains(const bezel::test::JitResult& r, const char* substr) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(substr) != std::string::npos)
            return true;
    }
    return false;
}
} // namespace

// §4.2: `9223372036854775808L` is 2^63, one past long.MaxValue (2^63 − 1) —
// rejected (it once wrapped to long.MinValue and compiled).
BEZEL_TEST(codegen_cg1, suffixed_long_over_max_rejected_1155) {
    auto r = runAot(
        "module M; class App public static method Main(): int "
        "  var a: long := 9223372036854775808L; "
        "  return (1 if a < 0 else 0); end Main; end App;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(anyErrorContains(r, "does not fit in `long`"));
}

// §4.2: `5000000000U` exceeds uint.MaxValue (2^32 − 1) — rejected (it once
// wrapped silently).
BEZEL_TEST(codegen_cg1, suffixed_uint_over_max_rejected_1155) {
    auto r = runAot(
        "module M; class App public static method Main(): int "
        "  var a: uint := 5000000000U; "
        "  return (1 if a < 100 else 0); end Main; end App;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(anyErrorContains(r, "does not fit in `uint`"));
}

// §4.2: `9223372036854775807L` IS long.MaxValue (2^63 − 1) — in range, still
// types and runs correctly (a > 0).
BEZEL_TEST(codegen_cg1, suffixed_long_at_max_accepted_1155) {
    auto r = runAot(
        "module M; class App public static method Main(): int "
        "  var a: long := 9223372036854775807L; "
        "  return (1 if a > 0 else 0); end Main; end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §4.2: `4000000000U` is within uint's range (< 2^32) — in range, runs (a > 0).
BEZEL_TEST(codegen_cg1, suffixed_uint_in_range_accepted_1155) {
    auto r = runAot(
        "module M; class App public static method Main(): int "
        "  var a: uint := 4000000000U; "
        "  return (1 if a > 0 else 0); end Main; end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §4.2: the negative-boundary literal `-9223372036854775808L` is long.MinValue
// (−2^63); its 2^63 magnitude is admitted under a unary minus even though the
// bare `9223372036854775808L` is rejected above. Must still compile and be < 0.
BEZEL_TEST(codegen_cg1, long_min_value_negation_still_accepted_1155) {
    auto r = runAot(
        "module M; class App public static method Main(): int "
        "  var a: long := -9223372036854775808L; "
        "  return (1 if a < 0 else 0); end Main; end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §4.2: a magnitude one past |long.MinValue| (`-9223372036854775809L`, 2^63 + 1)
// exceeds long even under a unary minus — rejected.
BEZEL_TEST(codegen_cg1, negation_beyond_long_min_rejected_1155) {
    auto r = runAot(
        "module M; class App public static method Main(): int "
        "  var a: long := -9223372036854775809L; "
        "  return 0; end Main; end App;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(anyErrorContains(r, "does not fit in `long`"));
}
