// Regression tests for #1152 (§19.3): a `case` over a `decimal` scrutinee with
// a relational / constant pattern aborted the compiler. A decimal value
// lowers to decimalStructType() (a 128-bit struct, neither integer nor floating-
// point); the comparing-pattern handlers in patternMatches (LlvmBackend_Exprs.cpp)
// branched only on isFloatingPointTy (FCmp) vs else (ICmp), so a decimal scrutinee
// hit ICmp on struct operands → ICmpInst::AssertOK fired and bezelc aborted (rc
// 134). The fix routes a decimal scrutinee through the runtime comparison
// bz_decimal_cmp — the same path a plain `x < 2m` / a `when v < 2m` guard uses —
// and lowers each pattern operand directly as a decimal struct (a decimal literal
// in pattern position frequently lacks a sema exprType and would otherwise lower
// to a `double`, feeding a double bit-pattern to bz_decimal_cmp).
//
// Range patterns over a decimal scrutinee are NO LONGER tested here: §16.3
// (v0.272, #3319) restricts `Range<T>`'s T to the integer types and `char`,
// so a `0m..<2m` / `0m..2m` / `..` pattern over a decimal is rejected at sema
// (range pattern is not admitted over a `decimal` scrutinee) and never reaches
// codegen. The bz_decimal_cmp path the #1152 fix installed is still exercised
// by the relational and constant-pattern arms below — those are the patterns
// §16.3 leaves in scope over a decimal scrutinee. The range-pattern tests that
// used to live here (`range_half_open_matches`, `range_half_open_excludes_upper`,
// `range_inclusive_includes_upper`, `range_below_lower`, `range_negative_bounds`,
// `nullable_range_present_matches`, `nullable_null_falls_to_null_arm`) were
// removed when the §16.3 gate landed; their coverage is now provided by the
// new tests in case_pattern_range_over_float_scrutinee_rejected_3319_test.cpp
// (rejection) and the existing case_range_* tests for int/char (positive).
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
           "  return (case x of " + arm + ", _ => 4); end Main; end App;";
}
}  // namespace

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

// §19.3 / §7.11 (v0.286, #3298; v0.310, #5143 ruling) — REJECTION for an
// UN-narrowed nullable scrutinee, reversing the acceptance this row used to
// assert. A relational pattern over a NULLABLE scrutinee is a compile error:
// "ordering admits no absent participant (§7.11)" (§19.3). The CONSTANT arm
// over a nullable scrutinee is untouched — §19.3: "Constant patterns over a
// nullable scrutinee are unaffected: a constant pattern is the equality test
// of §8.4, which is total on `null`" — so the second row below is the
// acceptance control that keeps the #1152 bz_decimal_cmp constant path
// covered at `decimal?`.
//
// fix #5173 / §19.3 (v0.310): a §7.4-narrowed non-null read of a nullable
// scrutinee is now ADMITTED. The narrowed variant is covered by the
// `_admitted_5173` test below — runAot over the lowered program selects the
// `< 2m` arm and prints `1`, exercising bz_decimal_cmp on the unwrapped
// `%bz.decimal` payload.
BEZEL_TEST(case_decimal_pattern_1152, nullable_relational_arm_rejected) {
    auto r = runAot(
        "module M; class App public static method Main(x: decimal?): int "
        "  return (case x of < 2m => 1, 3.5m => 7, _ => 4); end Main; end App;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawRule = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("pattern requires a non-nullable scrutinee") !=
                std::string::npos &&
            d.message.find("19.3") != std::string::npos)
            sawRule = true;
    BEZEL_EXPECT_TRUE(sawRule);
}

// fix #5173 / §19.3 (v0.310, #5143 ruling): narrowed nullable decimal
// scrutinee, relational pattern. patternMatches's `isNullableVal` branch
// unwraps the `{i8 hasValue; %bz.decimal payload}` box, and the relational
// pattern routes through `bz_decimal_cmp` (#1152 path) on the bare
// `%bz.decimal` payload.
//
// `Main(x: decimal?)` is NOT a §33.2 entry-point signature (only `Main()` or
// `Main(args: Array<string>)` are), so a parameter-driven form never runs to
// completion — runAot's driver fails the entry-point check. Helper.Get() returns
// `decimal?` (the declared-null form), so `valueProvesNonNull` returns false and
// `x` is NOT narrowed at the declaration; the `if x <> null then` block narrows
// `x` for the case form (§7.4; §19.1 reads the scrutinee's nullability after
// narrowing, as §7.11 reads an operand's).
BEZEL_TEST(case_decimal_pattern_1152,
           nullable_relational_arm_narrowed_admitted_5173) {
    auto r = runAot(
        "module M; class Helper public static method Get(): decimal? "
        "  return 1.0m; end Get; end Helper; "
        "class App public static method Main(): int "
        "  var x: decimal? := Helper.Get(); "
        "  if x <> null then "
        "    return (case x of < 2m => 1, 3.5m => 7, _ => 4); "
        "  end if; "
        "  return 99; "
        "end Main; end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(case_decimal_pattern_1152, nullable_constant_arm_still_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class App public static method Main(): int "
        "  var x: decimal? := 3.5m; "
        "  return (case x of 3.5m => 7, null => 1, _ => 4); end Main; end App;"),
        7);
}

// Regression guard: an INT scrutinee with the same shapes is unaffected.
BEZEL_TEST(case_decimal_pattern_1152, int_scrutinee_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class App public static method Main(): int "
        "  var x: int := 5; "
        "  return (case x of 0..<2 => 2, 5 => 3, _ => 4); end Main; end App;"),
        3);
}