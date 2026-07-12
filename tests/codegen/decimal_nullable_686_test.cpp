// Codegen tests for fix #686: LINQ Sum/Min/Max/Average over Array<decimal?> —
// the isDecimalSum path loaded decTy (16 bytes) from a {i8, decimal} nullable
// slot (17 bytes), producing garbage accumulation; the closure ABI was also
// wrong (expected decTy param but got the {i8,decimal} struct).
//
// Spec reference: stdlib §5 (LINQ aggregates), §7.1 (nullable value types),
//   §4.4 (decimal), issue #663 ({i8,decimal} nullable repr).
//
// Root cause: isDecimalSum triggered when selector returns decimal, but element
// load used CreateLoad(decTy, ep) instead of the full {i8,decimal} struct.
// Fix: detect nullable-decimal element, load full struct, use correct ABI.

#include <string>
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

} // namespace

// ---------------------------------------------------------------------------
// §7.1 / stdlib §5: Array<decimal?>.Sum(selector: (decimal?)->decimal)
// Bug #686: element loaded as decTy (16 bytes) from {i8,decimal} slot (17B).
// ---------------------------------------------------------------------------

// #686: Sum over two non-null decimal? elements via ?? operator in selector.
// Probe shape: nullable.Sum((x) => x ?? 0.0m) where all elements have values.
BEZEL_TEST(codegen_686_nullable_decimal_linq, sum_selector_nullable_both_nonnull_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal?>(); "
        "a.Add(1.5m); "
        "a.Add(2.5m); "
        "var s := a.Sum((x: decimal?) => x ?? 0.0m); "
        "if s = 4.0m then return 42; end; "
        // Encode actual result for diagnostics if wrong.
        "var enc := int(s * 10.0m); "
        "return enc; ")), 42);
}

// #686: Sum over Array<decimal?> with a null element — null contributes 0 via ??.
BEZEL_TEST(codegen_686_nullable_decimal_linq, sum_selector_nullable_with_null_element_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal?>(); "
        "a.Add(1.0m); "
        "a.Add(null); "
        "a.Add(3.0m); "
        "var s := a.Sum((x: decimal?) => x ?? 0.0m); "
        "if s = 4.0m then return 42; end; "
        "var enc := int(s * 10.0m); "
        "return enc; ")), 42);
}

// #686: Sum over empty Array<decimal?> returns 0m.
BEZEL_TEST(codegen_686_nullable_decimal_linq, sum_selector_nullable_empty_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal?>(); "
        "var s := a.Sum((x: decimal?) => x ?? 0.0m); "
        "if s = 0.0m then return 42; end; "
        "return 1; ")), 42);
}

// #686: Sum where selector projects decimal? → decimal via explicit non-null path.
// Five elements: 10m + 20m + 30m + 0m(null) + 40m = 100m
BEZEL_TEST(codegen_686_nullable_decimal_linq, sum_selector_nullable_five_elements_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal?>(); "
        "a.Add(10.0m); "
        "a.Add(20.0m); "
        "a.Add(30.0m); "
        "a.Add(null); "
        "a.Add(40.0m); "
        "var s := a.Sum((x: decimal?) => x ?? 0.0m); "
        "if s = 100.0m then return 42; end; "
        "var enc := int(s); "
        "return enc; ")), 42);
}

// #686 / non-nullable baseline: Array<decimal>.Sum(selector) must still work.
// Ensures the fix for nullable didn't break the non-nullable path.
BEZEL_TEST(codegen_686_nullable_decimal_linq, sum_selector_nonnullable_decimal_unaffected_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal>(); "
        "a.Add(1.5m); "
        "a.Add(2.5m); "
        "var s := a.Sum((x: decimal) => x); "
        "if s = 4.0m then return 42; end; "
        "var enc := int(s * 10.0m); "
        "return enc; ")), 42);
}

// ---------------------------------------------------------------------------
// §7.1 / stdlib §5.1: Min/Max over Array<decimal?> (selector forms)
// These use the selector path (elemTy == {i8,decimal}) → same class of bug.
// ---------------------------------------------------------------------------

// #686: Min with selector over Array<decimal?> — min of {5m, 2m, 8m} = 2m.
BEZEL_TEST(codegen_686_nullable_decimal_linq, min_selector_nullable_decimal_stdlib_5_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal?>(); "
        "a.Add(5.0m); "
        "a.Add(2.0m); "
        "a.Add(8.0m); "
        "var m := a.Min((x: decimal?) => x ?? 9999.0m); "
        "if m = 2.0m then return 42; end; "
        "var enc := int(m); "
        "return enc; ")), 42);
}

// #686: Max with selector over Array<decimal?> — max of {5m, 2m, 8m} = 8m.
BEZEL_TEST(codegen_686_nullable_decimal_linq, max_selector_nullable_decimal_stdlib_5_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal?>(); "
        "a.Add(5.0m); "
        "a.Add(2.0m); "
        "a.Add(8.0m); "
        "var m := a.Max((x: decimal?) => x ?? -9999.0m); "
        "if m = 8.0m then return 42; end; "
        "var enc := int(m); "
        "return enc; ")), 42);
}

// #686: Average with selector over Array<decimal?> — avg of {3m, null, 9m} = 4m.
// (null contributes 0m; sum = 12m, count = 3 → avg = 4m)
BEZEL_TEST(codegen_686_nullable_decimal_linq, average_selector_nullable_decimal_stdlib_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Array<decimal?>(); "
        "a.Add(3.0m); "
        "a.Add(null); "
        "a.Add(9.0m); "
        "var avg := a.Average((x: decimal?) => x ?? 0.0m); "
        "if avg = 4.0m then return 42; end; "
        "var enc := int(avg * 10.0m); "
        "return enc; ")), 42);
}
