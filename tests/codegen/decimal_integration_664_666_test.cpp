// Codegen tests for fix #664 and #666: decimal explicit casts and LINQ aggregates
// over decimal (§5.2, stdlib §5).
//
// Spec reference:
//   §5.2 (L436): explicit conversions:
//     - "var i: int := int(d);" — truncates toward zero
//     - "var f: double := double(m);" — decimal to double
//   §5.1 (L413–L422): int → decimal is implicit widening; explicit decimal(x) also works.
//   stdlib §5 (Sum selector decimal, Sum() no-selector, Min, Max, Average decimal).
//
// Bug #664: lowerConversion() in LlvmBackend_Exprs.cpp called lowerPrimitive(P::Decimal)
//   and then tried to use FP/integer cast instructions on the 16-byte struct — crash.
//   Explicit int/double → decimal called fptosi / cast<IntegerType> — crash.
//
// Bug #666: LINQ Sum/Min/Max/Average hardcoded i32 accumulator even for decimal
//   element type; selector trampoline passed 16-byte struct through a 64-bit slot.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

bool hasErrors(const std::string& program) {
    auto r = runAot(program);
    return r.diagnostics.hasErrors();
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

} // namespace

// ---------------------------------------------------------------------------
// #664: §5.2 explicit decimal conversions
// ---------------------------------------------------------------------------

// §5.2: decimal → int truncates toward zero (positive fraction).
BEZEL_TEST(codegen_664_decimal_casts, decimal_to_int_truncate_positive_spec_5_2) {
    // 3.9m → int: truncate toward zero → 3
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 3.9m; "
        "var i := int(d); "
        "if i <> 3 then return 1; end; "
        "return 42; ")), 42);
}

// §5.2: decimal → int truncates toward zero (negative fraction, not floor).
BEZEL_TEST(codegen_664_decimal_casts, decimal_to_int_truncate_negative_spec_5_2) {
    // -3.9m → int: truncate toward zero → -3 (not -4)
    BEZEL_EXPECT_EQ(run(prog(
        "var d := -3.9m; "
        "var i := int(d); "
        "if i <> -3 then return 1; end; "
        "return 42; ")), 42);
}

// §5.2: decimal → int exact integer (no fractional part).
BEZEL_TEST(codegen_664_decimal_casts, decimal_to_int_exact_spec_5_2) {
    // 5.0m → int = 5
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 5.0m; "
        "var i := int(d); "
        "if i <> 5 then return 1; end; "
        "return 42; ")), 42);
}

// §5.2: double → decimal explicit conversion (decimal(f)).
// The result is a decimal representation of the double value.
BEZEL_TEST(codegen_664_decimal_casts, double_to_decimal_explicit_spec_5_2) {
    // decimal(1.5) should equal 1.5m
    BEZEL_EXPECT_EQ(run(prog(
        "var f := 1.5; "
        "var d := decimal(f); "
        "if d = 1.5m then return 42; end; "
        "return 1; ")), 42);
}

// §5.2: int → decimal explicit cast (decimal(i)).
// §5.1 already makes this an implicit widening; explicit also works.
BEZEL_TEST(codegen_664_decimal_casts, int_to_decimal_explicit_spec_5_2) {
    // decimal(5) should equal 5m
    BEZEL_EXPECT_EQ(run(prog(
        "var i := 5; "
        "var d := decimal(i); "
        "if d <> 5m then return 1; end; "
        "return 42; ")), 42);
}

// §5.2: decimal → double explicit conversion (double(m)).
BEZEL_TEST(codegen_664_decimal_casts, decimal_to_double_explicit_spec_5_2) {
    // double(1.5m) should equal 1.5
    BEZEL_EXPECT_EQ(run(prog(
        "var m := 1.5m; "
        "var f := double(m); "
        "if f = 1.5 then return 42; end; "
        "return 1; ")), 42);
}

// §5.2: decimal → int overflow raises OverflowException.
// int.MaxValue is 2_147_483_647; 2_147_483_648m is one above.
BEZEL_TEST(codegen_664_decimal_casts, decimal_to_int_overflow_raises_spec_5_2) {
    // 2_147_483_648m is above int.MaxValue; int() should raise OverflowException
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  var d := 2147483648m; "
        "  var i := int(d); "
        "  return 1; "  // should not reach here
        "catch OverflowException => return 42; "
        "end; "
        "return 2; ")), 42);
}

// §5.2: decimal → int, negative overflow raises OverflowException.
BEZEL_TEST(codegen_664_decimal_casts, decimal_to_int_neg_overflow_raises_spec_5_2) {
    // -2_147_483_649m is below int.MinValue; int() should raise OverflowException
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  var d := -2147483649m; "
        "  var i := int(d); "
        "  return 1; "
        "catch OverflowException => return 42; "
        "end; "
        "return 2; ")), 42);
}

// §5.2: chain of casts: int → decimal → double → int.
BEZEL_TEST(codegen_664_decimal_casts, cast_chain_int_decimal_double_int_spec_5_2) {
    // 7 → decimal(7) → double(7.0m) → int(7.0) = 7
    BEZEL_EXPECT_EQ(run(prog(
        "var i := 7; "
        "var d := decimal(i); "
        "var f := double(d); "
        "var j := int(f); "
        "if j <> 7 then return 1; end; "
        "return 42; ")), 42);
}

// ---------------------------------------------------------------------------
// #666: stdlib §5 LINQ aggregates over decimal
// ---------------------------------------------------------------------------

// stdlib §5: Array<decimal>.Sum(selector: (decimal)->decimal): decimal
// Selector form with single element.
BEZEL_TEST(codegen_666_linq_decimal, sum_selector_single_element_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "a.Add(1.5m); "
        "var s := a.Sum(lambda(x: decimal) => x); "
        "if s = 1.5m then return 42; end; "
        "return 1; ")), 42);
}

// stdlib §5: Array<decimal>.Sum(): decimal (no-selector form).
BEZEL_TEST(codegen_666_linq_decimal, sum_no_selector_two_elements_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "a.Add(0.1m); "
        "a.Add(0.2m); "
        "var s: decimal := a.Sum(); "
        // 0.1m + 0.2m is exactly 0.3m in decimal (unlike double)
        "if s = 0.3m then return 42; end; "
        "return 1; ")), 42);
}

// stdlib §5: Sum() on empty Array<decimal> returns 0m.
BEZEL_TEST(codegen_666_linq_decimal, sum_no_selector_empty_returns_zero_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "var s: decimal := a.Sum(); "
        "if s = 0m then return 42; end; "
        "return 1; ")), 42);
}

// stdlib §5: Sum(selector) on empty Array<decimal> returns 0m.
BEZEL_TEST(codegen_666_linq_decimal, sum_selector_empty_returns_zero_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "var s := a.Sum(lambda(x: decimal) => x); "
        "if s = 0m then return 42; end; "
        "return 1; ")), 42);
}

// stdlib §5: Sum with multiple elements accumulates correctly.
BEZEL_TEST(codegen_666_linq_decimal, sum_selector_multiple_elements_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "a.Add(1.0m); "
        "a.Add(2.0m); "
        "a.Add(3.0m); "
        "var s := a.Sum(lambda(x: decimal) => x); "
        "if s = 6m then return 42; end; "
        "return 1; ")), 42);
}

// stdlib §5.1: Array<decimal>.Min(): decimal — throws on empty.
BEZEL_TEST(codegen_666_linq_decimal, min_throws_on_empty_stdlib_5_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "try "
        "  var m := a.Min(); "
        "  return 1; "
        "catch InvalidOperationException => return 42; "
        "end; "
        "return 2; ")), 42);
}

// stdlib §5.1: Array<decimal>.Min(): decimal returns smallest.
BEZEL_TEST(codegen_666_linq_decimal, min_returns_smallest_stdlib_5_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "a.Add(3.0m); "
        "a.Add(1.0m); "
        "a.Add(2.0m); "
        "var m := a.Min(); "
        "if m = 1m then return 42; end; "
        "return 1; ")), 42);
}

// stdlib §5.1: Array<decimal>.Max(): decimal returns largest.
BEZEL_TEST(codegen_666_linq_decimal, max_returns_largest_stdlib_5_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "a.Add(3.0m); "
        "a.Add(1.0m); "
        "a.Add(2.0m); "
        "var m := a.Max(); "
        "if m = 3m then return 42; end; "
        "return 1; ")), 42);
}

// stdlib §5: Average(selector: (decimal)->decimal): decimal.
// Per stdlib §5: "public method Average(selector: (T) -> decimal): decimal;"
// Average of {1.0, 2.0, 3.0} = 2.0.
BEZEL_TEST(codegen_666_linq_decimal, average_selector_decimal_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "a.Add(1.0m); "
        "a.Add(2.0m); "
        "a.Add(3.0m); "
        "var avg := a.Average(lambda(x: decimal) => x); "
        "if avg = 2.0m then return 42; end; "
        "return 1; ")), 42);
}

// stdlib §5.1: IEnumerable<decimal>.Average(): decimal (no-selector form).
// Average of {0.1m, 0.2m, 0.3m} = 0.2m.
BEZEL_TEST(codegen_666_linq_decimal, average_no_selector_decimal_stdlib_5_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "a.Add(0.1m); "
        "a.Add(0.2m); "
        "a.Add(0.3m); "
        "var avg: decimal := a.Average(); "
        "if avg = 0.2m then return 42; end; "
        "return 1; ")), 42);
}
