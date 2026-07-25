// Regression tests for stdlib §5.1 (v0.226) no-selector `Sum()`/`Min()`/`Max()`
// result-type: the no-selector aggregations return the element's OWN numeric
// type, not `int` (fix #1836, the no-selector counterpart to the #1757
// selector-`Sum` fix).
//
// stdlib §5.1 declares the no-selector overloads as per-type extensions:
//     IEnumerable<long>.Sum():   long
//     IEnumerable<ulong>.Sum():  ulong
//     IEnumerable<double>.Sum(): double
//     … (int/uint/float/decimal likewise) …
//     IEnumerable<T>.Min(): T     // T: IComparable<T>
//     IEnumerable<T>.Max(): T
//
// Before the fix, sema hard-defaulted the no-selector result to `int` and
// codegen accumulated in a hardwired i32, so a `long`/`ulong`/`double` element
// type was truncated: `Sum()` over `Array<long>` returned `9_000_000_000 mod
// 2^32 = 410_065_408`; `Min()`/`Max()` over `Array<long>` compared the
// 32-bit-truncated values. The fix keys both sema's result type and codegen's
// accumulator off the element type per §5.1.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

// stdlib §5.1: `Array<long>.Sum(): long`. The summed 9_000_000_000 exceeds the
// 32-bit range, so it is correct only if the accumulator is 64-bit. Pre-fix the
// i32 accumulator truncated it (9_000_000_000 mod 2^32 = 410_065_408).
BEZEL_TEST(no_selector_aggregation_element_type_1836, sum_long_not_truncated) {
    // stdlib §5.1, v0.226
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<long>(); "
        "    a.Add(3000000000L); a.Add(6000000000L); "
        "    var total := a.Sum(); "           // Sum(): long per §5.1
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C;"), "9000000000");
}

// stdlib §5.1: `Array<ulong>.Sum(): ulong`. 5e9 + 5e9 = 1e10, above int range;
// the unsigned accumulator must not wrap or sign-misread the elements.
BEZEL_TEST(no_selector_aggregation_element_type_1836, sum_ulong_not_truncated) {
    // stdlib §5.1, v0.226
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<ulong>(); "
        "    a.Add(5000000000UL); a.Add(5000000000UL); "
        "    var total := a.Sum(); "           // Sum(): ulong per §5.1
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C;"), "10000000000");
}

// stdlib §5.1: `Array<double>.Sum(): double`. 1.25 + 2.5 = 3.75. Pre-fix the
// i32 accumulator emitted malformed IR (truncating a double element to i32), so
// the result was a miscompile; with the fix the double accumulator yields 3.75.
BEZEL_TEST(no_selector_aggregation_element_type_1836, sum_double_is_double) {
    // stdlib §5.1, v0.226
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<double>(); "
        "    a.Add(1.25); a.Add(2.5); "
        "    var total := a.Sum(); "           // Sum(): double per §5.1
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C;"), "3.75");
}

// stdlib §5.1 (lazy receiver): `IEnumerable<long>.Sum(): long`. A projected
// lazy source (Select over a long Range) materializes through the same concrete
// terminal; the no-selector result must still be the element type. 3e9 + 6e9 +
// 9e9 = 1.8e10, above int range.
BEZEL_TEST(no_selector_aggregation_element_type_1836, sum_lazy_long_not_truncated) {
    // stdlib §5.1, v0.226
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var q := (1L..3L).Select(lambda(x: long) => x * 3000000000L); "
        "    var total := q.Sum(); "           // IEnumerable<long>.Sum(): long
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C;"), "18000000000");
}

// stdlib §5.1: `Array<long>.Max(): long`. The max (3_000_000_000) exceeds int
// range; pre-fix the i32 accumulator truncated each element and picked the
// 32-bit-signed max (1_294_967_296).
BEZEL_TEST(no_selector_aggregation_element_type_1836, max_long_not_truncated) {
    // stdlib §5.1, v0.226
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<long>(); "
        "    a.Add(-3000000000L); a.Add(7L); a.Add(3000000000L); "
        "    var m := a.Max(); "               // Max(): long per §5.1
        "    Console.WriteLine(m); "
        "    return 0; "
        "  end Main; "
        "end C;"), "3000000000");
}

// stdlib §5.1: `Array<long>.Min(): long`. The min (-3_000_000_000) is below int
// range; pre-fix the i32 accumulator truncated and picked -1_294_967_296.
BEZEL_TEST(no_selector_aggregation_element_type_1836, min_long_not_truncated) {
    // stdlib §5.1, v0.226
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<long>(); "
        "    a.Add(-3000000000L); a.Add(7L); a.Add(3000000000L); "
        "    var m := a.Min(); "               // Min(): long per §5.1
        "    Console.WriteLine(m); "
        "    return 0; "
        "  end Main; "
        "end C;"), "-3000000000");
}

// stdlib §5.1: `Array<ulong>.Max(): ulong`. Max(1, 4_000_000_000) =
// 4_000_000_000. Pre-fix the accumulator was a SIGNED i32 (4e6e6 truncates to a
// negative value), so the signed compare picked 1 instead. The fix derives both
// the 64-bit width AND unsigned compare from the element type.
BEZEL_TEST(no_selector_aggregation_element_type_1836, max_ulong_unsigned_compare) {
    // stdlib §5.1, v0.226
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<ulong>(); "
        "    a.Add(1UL); a.Add(4000000000UL); "
        "    var m := a.Max(); "               // Max(): ulong per §5.1
        "    Console.WriteLine(m); "
        "    return 0; "
        "  end Main; "
        "end C;"), "4000000000");
}

// Positive (unchanged): `Array<int>.Sum(): int` keeps accumulating as int — the
// §5.1 element-typed result is `int` for an int element, so 1+2+3 = 6 stands.
BEZEL_TEST(no_selector_aggregation_element_type_1836, sum_int_unchanged) {
    // stdlib §5.1, v0.226
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    return a.Sum(); "                 // Sum(): int per §5.1
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}
