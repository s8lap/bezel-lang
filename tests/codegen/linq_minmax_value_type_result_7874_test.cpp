// Issue #7874 (stdlib §5.1 *LINQ* `Min<U>/Max<U>`, stdlib §2.5 *DateTime /
// TimeSpan*, v0.350) -- a LINQ `Min`/`Max` over a SELECTOR whose RESULT
// type is a BUILT-IN VALUE TYPE keeps the value's own representation and
// the value's own ordering.
//
// THE GOVERNING TEXT.
//
//   * stdlib §5.1 (v0.350) declares, on the array/sequence surface,
//         public method Min<U>(selector: (T) -> U): U
//                                     where U: IComparable<U>;
//         public method Max<U>(selector: (T) -> U): U
//                                     where U: IComparable<U>;
//     so the selector's RESULT type U is the RESULT type of the call, and U
//     is constrained only by `IComparable<U>`.
//   * stdlib §2.5 (v0.350) declares `IComparable<Self>` on `DateTime`,
//     `TimeSpan`, `DateOnly` and `TimeOnly`, and fixes `DateTime`'s
//     ordering: "CompareTo = tick order (Kind not normalized, matching
//     .NET)". A `DateTime` is ONE i64 -- ticks in bits 0..61 and `Kind` in
//     bits 62..63 -- and a `TimeSpan` is ONE i64 tick count.
//
// THE DEFECT. The `Min`/`Max` accumulator was keyed off the selector's
// RESULT TYPE only when that type is a numeric PRIMITIVE. A built-in VALUE
// TYPE is not one, so the accumulator stayed the `int` default -- the `i32`
// placeholder -- and the value's own word was TRUNCATED to 32 bits. For a
// `DateTime` (an i64) the truncation is worse than a wrong value: the
// `Ticks` read on the RESULT masks the truncated word with the i64 tick
// mask, so the compiler emitted `and i32 %mm.r, i64 4611686018427387903`
// and the IR VERIFIER REJECTED it -- "Both operands to a binary operator are
// not of the same type!" -- so the compiler ABORTED on a legal program.
// Measured at the pre-fix tree: `Array<DateTime>.Min(sel)` and
// `Array<DateTime>.Max(sel)` did not compile at all.
//
// THE FIX (this commit). The accumulator is keyed off the VALUE TYPE: its
// own word width (`i64` for `DateTime`/`TimeSpan`, `i32` for
// `DateOnly`/`TimeOnly`), the selector is lowered against the VALUE type
// (so the closure returns the value's word), a `DateTime` compares by TICKS
// (the `Kind` bits never take part, stdlib §2.5), and the SELECT keeps the
// ORIGINAL word -- so the winner keeps its own `Kind`.
//
// ORACLE. `runAot` (the AOT path). Every row asserts the EXACT STDOUT -- a
// VALUE oracle, never "no diagnostic was produced" -- so a legal
// `Min`/`Max` over a value-type selector must compile and print the value
// stdlib §2.5's ordering selects.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;

std::string prog(const std::string& body) {
    return "module M;\n"
           "import System;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectStdout(const std::string& body, const std::string& expected) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// =====================================================================
// ROW 1 -- the issue's own shape: `Array<DateTime>.Min(sel)` and
// `Array<DateTime>.Max(sel)` at `U = DateTime`. stdlib §2.5's tick order
// selects the 3-tick value and the 5-tick value.
//
// Pre-fix: the compiler ABORTED -- the IR verifier rejected the truncated
//           result (`and i32 %mm.r, i64 ...`).
// Post-fix: "3" and "5".
// =====================================================================
BEZEL_TEST(linq_minmax_value_type_result_7874,
           array_of_datetime_min_and_max_over_a_datetime_selector) {
    expectStdout("    var arr: Array<DateTime> := new Array<DateTime>();\n"
                 "    arr.Add(DateTime.FromTicks(5L));\n"
                 "    arr.Add(DateTime.FromTicks(3L));\n"
                 "    Console.WriteLine(arr.Min(lambda(x: DateTime) => x)"
                 ".Ticks);\n"
                 "    Console.WriteLine(arr.Max(lambda(x: DateTime) => x)"
                 ".Ticks);\n",
                 "3\n5");
}

// =====================================================================
// ROW 2 -- the ORDERING, not just the width: a `DateTime`'s `Kind` rides in
// the word's top two bits, and stdlib §2.5 says the order is TICK order
// ("Kind not normalized"). The `Local` value's raw word is far larger than
// the `Unspecified` one's, so an order on the RAW word would pick the
// 100-tick `Local` value as the MINIMUM; the tick order picks the 10-tick
// `Unspecified` one.
//
// Pre-fix: the compiler ABORTED (the malformed-IR verifier failure).
// Post-fix: "10" and "100".
// =====================================================================
BEZEL_TEST(linq_minmax_value_type_result_7874,
           a_datetime_selector_orders_by_ticks_and_not_by_the_raw_word) {
    expectStdout("    var arr: Array<DateTime> := new Array<DateTime>();\n"
                 "    arr.Add(DateTime.FromTicks(10L));\n"
                 "    arr.Add(DateTime.FromTicks(100L).ToLocal());\n"
                 "    Console.WriteLine(arr.Min(lambda(x: DateTime) => x)"
                 ".Ticks);\n"
                 "    Console.WriteLine(arr.Max(lambda(x: DateTime) => x)"
                 ".Ticks);\n",
                 "10\n100");
}

// =====================================================================
// ROW 3 -- the WIDTH, on a `TimeSpan` (one i64 tick count, stdlib §2.5):
// the tick counts here do NOT fit in 32 bits, so a 32-bit accumulator
// truncates them.
//
// Pre-fix: the compiler ABORTED (the malformed-IR verifier failure).
// Post-fix: "4000000000" and "5000000000".
// =====================================================================
BEZEL_TEST(linq_minmax_value_type_result_7874,
           an_array_of_timespan_min_and_max_keep_the_full_tick_width) {
    expectStdout("    var arr: Array<TimeSpan> := new Array<TimeSpan>();\n"
                 "    arr.Add(TimeSpan.FromTicks(5000000000L));\n"
                 "    arr.Add(TimeSpan.FromTicks(4000000000L));\n"
                 "    Console.WriteLine(arr.Min(lambda(x: TimeSpan) => x)"
                 ".Ticks);\n"
                 "    Console.WriteLine(arr.Max(lambda(x: TimeSpan) => x)"
                 ".Ticks);\n",
                 "4000000000\n5000000000");
}

// =====================================================================
// CONTROL -- the NUMERIC selector path is unchanged: a `long`-returning
// selector over an `Array<int>` still returns the element's own numeric
// type. A VALUE oracle, green on both arms.
//
// Pre-fix and post-fix: "1" and "9".
// =====================================================================
BEZEL_TEST(linq_minmax_value_type_result_7874,
           the_numeric_selector_path_is_unchanged) {
    expectStdout("    var arr: Array<int> := new Array<int>();\n"
                 "    arr.Add(9);\n"
                 "    arr.Add(1);\n"
                 "    Console.WriteLine(arr.Min(lambda(x: int) => x));\n"
                 "    Console.WriteLine(arr.Max(lambda(x: int) => x));\n",
                 "1\n9");
}

// =====================================================================
// CONTROL -- a selector that projects to a PRIMITIVE (`Ticks`) over the same
// `Array<DateTime>` is unchanged: the projection's type is an `int`, and the
// `int` accumulator is the right one.
//
// Pre-fix and post-fix: "3" and "5".
// =====================================================================
BEZEL_TEST(linq_minmax_value_type_result_7874,
           a_selector_projecting_to_a_primitive_is_unchanged) {
    expectStdout("    var arr: Array<DateTime> := new Array<DateTime>();\n"
                 "    arr.Add(DateTime.FromTicks(5L));\n"
                 "    arr.Add(DateTime.FromTicks(3L));\n"
                 "    Console.WriteLine(arr.Min(lambda(x: DateTime) => x.Ticks));\n"
                 "    Console.WriteLine(arr.Max(lambda(x: DateTime) => x.Ticks));\n",
                 "3\n5");
}
