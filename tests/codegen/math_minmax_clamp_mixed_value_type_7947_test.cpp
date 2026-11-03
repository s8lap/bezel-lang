// Issue #7947 (spec §14.5, stdlib §2.2 *Math*, stdlib §2.5 *DateTime /
// TimeSpan / DateOnly / TimeOnly*, v0.350) -- a `Math.Min` / `Max` /
// `Clamp` whose operands do NOT agree on ONE ordering type is REJECTED with
// the §14.5 inconsistent-inference diagnostic, and the compiler never emits
// malformed IR for it.
//
// THE GOVERNING TEXT.
//
//   * stdlib §2.2 (v0.350) declares ONE type parameter for the whole call:
//         public static method Min<T>(a: T, b: T): T
//             where T: IComparable<T>;
//         public static method Max<T>(a: T, b: T): T
//             where T: IComparable<T>;
//         public static method Clamp<T>(value: T, low: T, high: T): T
//             where T: IComparable<T>;
//     and its prose ("What `Min`, `Max`, and `Clamp` compute") fixes the
//     ordering as `T`'s OWN `CompareTo` -- so there is exactly ONE `T` per
//     call, and it is the type of every operand.
//   * spec §14.5: "all arguments for a type parameter must agree" -- so an
//     `int` beside a `TimeSpan` is not a `Math` call at all; it is a call
//     whose type argument cannot be inferred.
//   * stdlib §2.5 (v0.350): `DateTime`, `TimeSpan`, `DateOnly` and `TimeOnly`
//     each declare `IComparable<Self>`, so each of the four is a `T` of its
//     own -- and a `DateTime`'s order is TICK order ("CompareTo = tick order
//     (Kind not normalized, matching .NET)").
//
// THE DEFECT. The front end's #7900 check takes its probe from the FIRST
// argument only, so it fires for `Math.Max(ts, 1)` and stays SILENT for
// `Math.Max(1, ts)` -- the mirror image. The silent case reached codegen,
// and the lowering compared the two operands' raw words: the `int` is an
// i32 and the `TimeSpan` an i64, so the compiler emitted
// `icmp sgt i32 1, i64 %7` and the IR VERIFIER rejected the module --
// "Both operands to ICmp instruction are not of the same type!" -- so the
// compiler ABORTED on a program that must be REJECTED with a diagnostic.
//
// The same shape with a `long` beside a `DateTime` did not even abort: both
// are i64, so the comparison was well-typed and the call returned the
// PRIMITIVE where the order says the `DateTime` -- a SILENTLY WRONG answer.
//
// THE FIX. The `Math` lowering now reads the ORDERING HEAD of every operand
// (a builtin value type -- `DateTime` / `TimeSpan` / `DateOnly` / `TimeOnly`
// -- or nothing at all) and FAILS CLOSED when the operands do not agree on
// one: it reports the §14.5 diagnostic the front end reports for the
// mirror-image spelling, so the two orders of the same mistake read the
// same. (The front end's own check stops the build before this arm is
// reached wherever it already fires, so the diagnostic is never doubled.)
//
// ORACLE. `runAot` (the AOT path). Every row asserts the POSITIVE: the
// program was REJECTED at compile time, the §14.5 diagnostic TEXT is the one
// stdlib §2.2's contract calls for, and the rejection carries a REAL source
// location -- and, for the defect rows, that the compiler no longer reports
// the malformed-IR failure. The control row is a VALUE oracle: the same
// `Math` surface over two operands of ONE type still compiles and still
// returns the value the order selects.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// The §14.5 inconsistent-inference diagnostic stdlib §2.2's single type
// parameter calls for, in the front end's own wording. The HEAD is the
// ordering type the operands must agree on.
std::string inconsistentInference(const char* member, const char* head) {
    return std::string("inconsistent type-argument inference: type parameter "
                       "`T` of `Math.") + member + "` is `" + head +
           "`, but this argument is not (§14.5, stdlib §2.2); all arguments "
           "for a type parameter must agree";
}

// The row's verdict: the program was REJECTED at compile time with the
// §14.5 diagnostic, at a real source location, and the compiler did NOT
// abort on malformed IR.
void expectRejectedWith(const std::string& src, const std::string& message) {
    JitResult r = runAot(src);
    // `!r.ran` alone is ambiguous across four outcomes (see the header of
    // aot_result_asserts.hpp); pair it with the real-location discriminator
    // so this row cannot pass on a crash or a timeout.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, message.c_str()));
    // The measured defect: the compiler ABORTED on the malformed IR it
    // emitted for this shape ("IR verification failed (malformed IR
    // emitted; see report above)"). It must not any more.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

}  // namespace

// ===========================================================================
// ROW 1 -- the issue's own shape: a PRIMITIVE first and a TIME VALUE
// second. The front end's #7900 check takes its probe from the first
// argument, so it stays SILENT here and the call reached codegen.
//
// Pre-fix: the compiler ABORTED -- "Both operands to ICmp instruction are
//           not of the same type!" -- and the IR verifier rejected the
//           module.
// Post-fix: the call is REJECTED with the §14.5 diagnostic.
// ===========================================================================
BEZEL_TEST(math_minmax_clamp_mixed_value_type_7947,
           max_with_a_primitive_first_and_a_timespan_second_is_rejected) {
    expectRejectedWith(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var ts := new TimeSpan(5); "
        "    var x := Math.Max(1, ts); "
        "    Console.WriteLine(x); "
        "    return 0; "
        "  end Main; "
        "end C;",
        inconsistentInference("Max", "TimeSpan"));
}

// ===========================================================================
// ROW 2 -- the SILENTLY WRONG shape: a `long` beside a `DateTime`. Both are
// i64, so the comparison was well-typed and the call returned the
// PRIMITIVE (2) where stdlib §2.2's order says the `DateTime`.
//
// Pre-fix: the program compiled clean and printed 2.
// Post-fix: the call is REJECTED with the §14.5 diagnostic.
// ===========================================================================
BEZEL_TEST(math_minmax_clamp_mixed_value_type_7947,
           min_with_a_long_first_and_a_datetime_second_is_rejected) {
    expectRejectedWith(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var dt := new DateTime(2020, 1, 1); "
        "    var x := Math.Min(2L, dt); "
        "    Console.WriteLine(x); "
        "    return 0; "
        "  end Main; "
        "end C;",
        inconsistentInference("Min", "DateTime"));
}

// ===========================================================================
// ROW 3 -- the three-operand form: `Math.Clamp(value, low, high)` with a
// PRIMITIVE value beside a TIME VALUE range.
//
// Pre-fix: the compiler ABORTED on the malformed IR.
// Post-fix: the call is REJECTED with the §14.5 diagnostic.
// ===========================================================================
BEZEL_TEST(math_minmax_clamp_mixed_value_type_7947,
           clamp_with_a_primitive_value_and_a_timespan_range_is_rejected) {
    expectRejectedWith(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var ts := new TimeSpan(5); "
        "    var dt := new DateTime(2020, 1, 1); "
        "    var x := Math.Clamp(1, ts, dt); "
        "    Console.WriteLine(x); "
        "    return 0; "
        "  end Main; "
        "end C;",
        inconsistentInference("Clamp", "TimeSpan"));
}

// ===========================================================================
// CONTROL -- the MIRROR IMAGE, `Math.Max(ts, 1)`: the front end's own
// #7900 check already rejects it, and it has always read the same way. It
// is GREEN on both arms, and it is what pins that the two orders of the
// same mistake are reported identically.
// ===========================================================================
BEZEL_TEST(math_minmax_clamp_mixed_value_type_7947,
           max_with_a_timespan_first_and_a_primitive_second_reads_the_same) {
    expectRejectedWith(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var ts := new TimeSpan(5); "
        "    var x := Math.Max(ts, 1); "
        "    Console.WriteLine(x); "
        "    return 0; "
        "  end Main; "
        "end C;",
        inconsistentInference("Max", "TimeSpan"));
}

// ===========================================================================
// CONTROL -- the VALUE oracle: the same `Math` surface over two operands
// of ONE ordering type still compiles and still returns the value the order
// selects (stdlib §2.5's tick order for a `DateTime`, the tick count for a
// `TimeSpan`). A guard that rejected these would be over-firing.
//
// Green on both arms: "2021", "9", "2020".
// ===========================================================================
BEZEL_TEST(math_minmax_clamp_mixed_value_type_7947,
           max_and_min_over_two_value_types_of_one_type_still_compare) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var d1 := new DateTime(2020, 1, 1); "
        "    var d2 := new DateTime(2021, 1, 1); "
        "    Console.WriteLine(Math.Max(d1, d2).Year); "
        "    var s1 := new TimeSpan(5); "
        "    var s2 := new TimeSpan(9); "
        "    Console.WriteLine(Math.Max(s1, s2).Ticks); "
        "    Console.WriteLine(Math.Min(d1, d2).Year); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2021\n9\n2020");
}
