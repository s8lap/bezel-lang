// Regression tests for #1152 (§19.3): a `case` over a `decimal` scrutinee with
// a range / relational / constant pattern aborted the compiler. A decimal value
// lowers to decimalStructType() (a 128-bit struct, neither integer nor floating-
// point); the comparing-pattern handlers in patternMatches (LlvmBackend_Exprs.cpp)
// branched only on isFloatingPointTy (FCmp) vs else (ICmp), so a decimal scrutinee
// hit ICmp on struct operands → ICmpInst::AssertOK fired and bezelc aborted (rc
// 134). The fix routes a decimal scrutinee through the runtime comparison
// bz_decimal_cmp — the same path a plain `x < 2m` / a `when v < 2m` guard uses —
// and lowers each pattern operand directly as a decimal struct (a decimal literal
// in pattern position frequently lacks a sema exprType and would otherwise lower
// to a `double`, feeding a double bit-pattern to bz_decimal_cmp). §19.3 does not
// restrict a case scrutinee to integer types.
//
// runJit returns the raw exit code (0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// A `case` over a non-nullable decimal scrutinee with one comparing arm + `_`.
std::string decCase(const std::string& init, const std::string& arm) {
    return std::string("module M; class App public static method Main(): int "
                       "  var x: decimal := ") + init + "; "
           "  return case x of " + arm + ", _ => 4 end; end Main; end App;";
}
}  // namespace

// The #1152 repro verbatim: a half-open range pattern over a decimal matches.
BEZEL_TEST(case_decimal_pattern_1152, range_half_open_matches) {
    BEZEL_EXPECT_EQ(run(decCase("1.5m", "0m..<2m => 2")), 2);  // 0 <= 1.5 < 2
}

// Exclusive upper bound: the boundary value is NOT in `0m..<2m`.
BEZEL_TEST(case_decimal_pattern_1152, range_half_open_excludes_upper) {
    BEZEL_EXPECT_EQ(run(decCase("2m", "0m..<2m => 2")), 4);  // 2 not < 2 → `_`
}

// Inclusive range includes its upper bound.
BEZEL_TEST(case_decimal_pattern_1152, range_inclusive_includes_upper) {
    BEZEL_EXPECT_EQ(run(decCase("2m", "0m..2m => 2")), 2);   // 0 <= 2 <= 2
}

// A value below the lower bound falls through to `_`.
BEZEL_TEST(case_decimal_pattern_1152, range_below_lower) {
    BEZEL_EXPECT_EQ(run(decCase("-1m", "0m..2m => 2")), 4);
}

// Negative bounds round-trip through the decimal parser's sign.
BEZEL_TEST(case_decimal_pattern_1152, range_negative_bounds) {
    BEZEL_EXPECT_EQ(run(decCase("-1.5m", "-2m..<0m => 2")), 2);  // -2 <= -1.5 < 0
}

// Relational pattern `< 2m` over a decimal.
BEZEL_TEST(case_decimal_pattern_1152, relational_lt_matches) {
    BEZEL_EXPECT_EQ(run(decCase("1.5m", "< 2m => 1")), 1);
}

// Relational `< 2m` not satisfied → `_`.
BEZEL_TEST(case_decimal_pattern_1152, relational_lt_falls_through) {
    BEZEL_EXPECT_EQ(run(decCase("5m", "< 2m => 1")), 4);
}

// `>=` and `<=` lower correctly (boundary inclusive).
BEZEL_TEST(case_decimal_pattern_1152, relational_ge_le_boundary) {
    BEZEL_EXPECT_EQ(run(decCase("3m", ">= 3m => 6")), 6);
    BEZEL_EXPECT_EQ(run(decCase("3m", "<= 3m => 7")), 7);
}

// Constant (fractional) pattern over a decimal — exact value equality.
BEZEL_TEST(case_decimal_pattern_1152, constant_fractional_matches) {
    BEZEL_EXPECT_EQ(run(decCase("2.5m", "2.5m => 7")), 7);
}

// Constant pattern non-match → `_`.
BEZEL_TEST(case_decimal_pattern_1152, constant_non_match) {
    BEZEL_EXPECT_EQ(run(decCase("2.5m", "2.4m => 7")), 4);
}

// Scale-insensitive equality: `1.0m` and `1.00m` are equal (bz_decimal_cmp
// normalises scale).
BEZEL_TEST(case_decimal_pattern_1152, constant_scale_insensitive) {
    BEZEL_EXPECT_EQ(run(decCase("1.00m", "1.0m => 7")), 7);
}

// A nullable `decimal?` scrutinee: range pattern on a present value matches.
BEZEL_TEST(case_decimal_pattern_1152, nullable_range_present_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class App public static method Main(): int "
        "  var x: decimal? := 1.5m; "
        "  return case x of 0m..<2m => 2, null => 6, _ => 4 end; end Main; end App;"),
        2);
}

// A null `decimal?` scrutinee never matches a comparing pattern → the null arm.
BEZEL_TEST(case_decimal_pattern_1152, nullable_null_falls_to_null_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; class App public static method Main(): int "
        "  var x: decimal? := null; "
        "  return case x of 0m..<2m => 2, null => 6, _ => 4 end; end Main; end App;"),
        6);
}

// A nullable `decimal?` with relational then constant arms.
BEZEL_TEST(case_decimal_pattern_1152, nullable_relational_then_constant) {
    BEZEL_EXPECT_EQ(run(
        "module M; class App public static method Main(): int "
        "  var x: decimal? := 3.5m; "
        "  return case x of < 2m => 1, 3.5m => 7, _ => 4 end; end Main; end App;"),
        7);
}

// Regression guard: an INT scrutinee with the same shapes is unaffected.
BEZEL_TEST(case_decimal_pattern_1152, int_scrutinee_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class App public static method Main(): int "
        "  var x: int := 5; "
        "  return case x of 0..<2 => 2, 5 => 3, _ => 4 end; end Main; end App;"),
        3);
}
