// Regression tests for stdlib §5 `Sum(selector)` result-type classification
// (spec §22.6, v0.196; fix #1757, the overload-ranking follow-up to #1697).
//
// The seven `Sum(selector)` overloads (stdlib §5) pair each numeric result type
// with a selector returning that same type:
//     Sum(selector: (T) -> int):     int
//     Sum(selector: (T) -> long):    long
//     Sum(selector: (T) -> uint):    uint
//     Sum(selector: (T) -> ulong):   ulong
//     Sum(selector: (T) -> float):   float
//     Sum(selector: (T) -> double):  double
//     Sum(selector: (T) -> decimal): decimal
//
// Before the fix, sema hard-defaulted the result of `Sum(selector)` to int (or
// decimal) instead of classifying the selector's inferred return type against
// these seven candidates by §22.6 overload resolution, and codegen accumulated
// the sum in a hardwired i32. Consequences:
//   * a `long`-returning selector was truncated to 32 bits;
//   * a `double`-returning selector produced an i32 sum that mismatched its
//     double-typed use site — malformed IR (verifyModule failure);
//   * a selector whose return type converts co-equally to more than one overload
//     with no exact match (e.g. `byte`, which widens to int/long/uint/ulong/
//     float/double/decimal) was resolved silently to int rather than reported
//     as an ambiguous call (§22.6).
//
// The fix classifies the selector's inferred return type against the seven
// candidates exactly as §22.6 ranks a single argument position (exact match
// wins; a lone widening wins; two-or-more co-equal widenings are ambiguous),
// and codegen accumulates in the selected overload's result type.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

// A `long`-returning selector selects `Sum(selector: (T) -> long): long` (spec
// §22.6 exact match). The per-element product 3_000_000_000 exceeds the 32-bit
// range, so a summed 9_000_000_000 is correct only if the accumulator is 64-bit.
// Pre-fix the i32 accumulator truncated it (9_000_000_000 mod 2^32 =
// 410_065_408), so this asserts the un-truncated value.
BEZEL_TEST(sum_selector_result_type_1757, long_selector_not_truncated) {
    // spec §22.6, v0.196 / stdlib §5
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); "
        "    var total := a.Sum(lambda(x: int) => x * 3000000000L); "
        "    Console.WriteLine(total); return 0; "
        "  end Main; "
        "end C;"), "9000000000");
}

// A `double`-returning selector selects `Sum(selector: (T) -> double): double`.
// 3*0.25 + 7*0.25 = 2.5. Pre-fix, sema mistyped the result as int and codegen
// summed in i32, so the result was either garbage (int reading of a double) or
// malformed IR at the double-typed WriteLine.
BEZEL_TEST(sum_selector_result_type_1757, double_selector_result_is_double) {
    // spec §22.6, v0.196 / stdlib §5
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(3); a.Add(7); "
        "    var total := a.Sum(lambda(x: int) => x * 0.25); "
        "    Console.WriteLine(total); return 0; "
        "  end Main; "
        "end C;"), "2.5");
}

// A `uint`-returning selector selects `Sum(selector: (T) -> uint): uint`.
// 3 x 1_000_000_000 = 3_000_000_000 is inside uint's range and ABOVE int's
// (2_147_483_647), so an `int`-typed accumulator could not produce it: the row
// pins the unsigned 32-bit result type and the unsigned formatting.
//
// UPDATED BY #4695 (stdlib §5 + spec §6.1, v0.307). This row previously summed
// 3 x 4_000_000_000 and asserted 3_410_065_408 — the 32-bit WRAPAROUND of
// 12_000_000_000 — using silent wrap as the observable for "the accumulator is
// uint-wide". §6.1 makes integer `+` checked at every width and stdlib §5 says
// `Sum` "is overflow-checked in" its result type, so that input now raises
// `OverflowException` and the old expectation pinned behaviour the spec
// forbids. The property the row exists to pin is preserved and SPLIT IN TWO,
// which is strictly stronger than the single wrap assertion: the in-range sum
// below rules out an `int` accumulator, and `uint_selector_overflow_is_checked`
// rules out a `long`/`ulong` one (neither would raise at 12e9).
BEZEL_TEST(sum_selector_result_type_1757, uint_selector_result_is_uint) {
    // spec §22.6, v0.196 / stdlib §5 / spec §6.1, v0.307
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    var total := a.Sum(lambda(x: int) => 1000000000U); "
        "    Console.WriteLine(total); return 0; "
        "  end Main; "
        "end C;"), "3000000000");
}

// The other half of the split above: 3 x 4_000_000_000 = 12_000_000_000 does
// not fit `uint`, so §6.1's checked `+` raises `OverflowException` rather than
// wrapping to 3_410_065_408. A `long`/`ulong` accumulator would print the exact
// total instead of raising, so this row is what now pins the accumulator's
// 32-bit width (#4695).
BEZEL_TEST(sum_selector_result_type_1757, uint_selector_overflow_is_checked) {
    // spec §6.1 / stdlib §5, v0.307
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    try "
        "      Console.WriteLine(a.Sum(lambda(x: int) => 4000000000U)); "
        "    catch "
        "      OverflowException => Console.WriteLine(\"caught\") "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), "caught");
}

// A selector whose return type converts co-equally to more than one Sum
// overload's result type with no exact match — here `byte`, which widens to
// int/long/uint/ulong/float/double/decimal — is an ambiguous call (§22.6), not
// a silent pick of int. This is the literal #1757 case (a `bs.Sum((x) => x)`
// over `Array<byte>`).
BEZEL_TEST(sum_selector_result_type_1757, byte_selector_is_ambiguous) {
    // spec §22.6, v0.196 / stdlib §5
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<byte>(); "
        "    a.Add(1); a.Add(2); "
        "    var total := a.Sum(lambda(x: byte) => x); "
        "    Console.WriteLine(total); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("ambiguous call to Sum(selector)") !=
                std::string::npos &&
            d.message.find("(§22.6)") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// The `byte` ambiguity is resolved by casting the selector's result to a single
// numeric type — `int(x)` selects `Sum(selector: (T) -> int): int` (exact
// match), which compiles and sums as int (1+2+3 = 6).
BEZEL_TEST(sum_selector_result_type_1757, byte_selector_cast_disambiguates) {
    // spec §22.6, v0.196 / stdlib §5
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<byte>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    return a.Sum(lambda(x: byte) => int(x)); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// The common `int` selector is unchanged: exact match on
// `Sum(selector: (T) -> int): int` (9+1+16 = 26).
BEZEL_TEST(sum_selector_result_type_1757, int_selector_unchanged) {
    // spec §22.6, v0.196 / stdlib §5
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(3); a.Add(1); a.Add(4); "
        "    return a.Sum(lambda(x: int) => x * x); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 26);
}
