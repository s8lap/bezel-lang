// Pin tests for issue #7576: `TimeSpan.From<Unit>(double)` has no
// overflow/NaN guard.
//
// stdlib §2.5 (v0.355) declares the `From<Unit>` family and states its
// contract (quoted verbatim from `docs/Bezel_Standard_Library.md` §2.5,
// "The tick, and the `From`/`Total` pairs"):
//
//   "`TimeSpan.From<Unit>(n)` is the span of `n` of that unit --
//    `FromHours(1.5)` is ninety minutes -- computed over ticks in `double`
//    and truncated toward zero to a whole number of ticks, raising
//    `OverflowException` when the result falls outside the tick range or
//    the argument is `NaN` (the `*` operator's contract above)."
//
// and the `*` operator's own contract (§2.5, the TimeSpan declaration):
//
//   "computes over Ticks in double; OverflowException on a product outside
//    the tick range or a NaN operand (pinned). The range check reads the
//    double product, so MaxValue * 1.0 raises (its Ticks convert to 2^63);
//    an in-range product is truncated toward zero to whole ticks, as
//    From<Unit> is"
//
// The `From<Unit>` lowering computed `fptosi(fmul(arg, ratio))` with NO
// guard. `fptosi` of an out-of-range or NaN product is POISON in LLVM (on
// x86 the `cvttsd2si` yields the "integer indefinite" value), so the
// compiler emitted a program that silently produced an arbitrary tick
// count. MEASURED on r30-c1 (this lane's base):
//
//   TimeSpan.FromDays(1e300)        -> ticks 140341758457675   (no raise)
//   TimeSpan.FromSeconds(0.0/0.0)   -> ticks -9223372036854775808 (no raise)
//
// Every row below is on the AOT oracle with the CHECKED (ASan/LSan) leg,
// and pins the program's exact stdout. Each catch arm prints a DISTINCT
// marker, so a row that prints "OVF" proves OverflowException itself was
// raised (not some other fault).
//
// Cite: stdlib §2.5, v0.355; spec §31.4 (OverflowException), v0.355.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " return 0; end Main; end C;";
}

// The catch arms: a DISTINCT marker per exception type, so "OVF" proves
// OverflowException was raised and "OTHER" proves some other fault was.
const char* kCatch =
    "catch OverflowException => Console.WriteLine(\"OVF\"); "
    "catch Exception => Console.WriteLine(\"OTHER\"); end; ";

// runAotChecked + the VALUE channel: the program's stdout with ONE trailing
// line-ending stripped (runAotOut's rule), or a sentinel when the program was
// rejected / crashed / tripped the sanitizer.
std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

// A `From<Unit>` call inside a try, printing either the exception marker or
// the span's tick count.
std::string fromCall(const std::string& expr) {
    return "try var s := " + expr + "; "
           "Console.WriteLine($\"ticks {s.Ticks}\"); " + kCatch;
}

}  // namespace

// ---------------------------------------------------------------------------
// The issue's own repros.
// ---------------------------------------------------------------------------

// Issue #7576's first repro: `TimeSpan.FromDays(1e300)` -- the double
// product (1e300 * 864000000000.0) is far outside the tick range, so the
// call raises OverflowException.
BEZEL_TEST(timespan_from_overflow_7576, from_days_huge_raises_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(fromCall("TimeSpan.FromDays(1e300)"))),
                    "OVF");
}

// Issue #7576's second repro: `TimeSpan.FromSeconds(0.0/0.0)` -- a NaN
// ARGUMENT, which the stdlib names separately ("or the argument is `NaN`").
BEZEL_TEST(timespan_from_overflow_7576, from_seconds_nan_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(fromCall("TimeSpan.FromSeconds(0.0 / 0.0)"))), "OVF");
}

// A finite out-of-range product: 1e15 ms * 10000 = 1e19 ticks, past the
// 2^63 boundary.
BEZEL_TEST(timespan_from_overflow_7576, from_milliseconds_huge_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(fromCall("TimeSpan.FromMilliseconds(1e15)"))), "OVF");
}

// The NEGATIVE side of the range: -1e300 days is below the tick range too.
BEZEL_TEST(timespan_from_overflow_7576, from_days_negative_huge_raises_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(fromCall("TimeSpan.FromDays(-1e300)"))),
                    "OVF");
}

// ---------------------------------------------------------------------------
// In-range controls: the truncation toward zero and the exact tick counts.
// The stdlib's own example is `FromHours(1.5)` = ninety minutes.
// ---------------------------------------------------------------------------

BEZEL_TEST(timespan_from_overflow_7576, from_unit_in_range_is_exact) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        "Console.WriteLine($\"{TimeSpan.FromDays(1.5).Ticks} "
        "{TimeSpan.FromHours(1.5).Ticks} "
        "{TimeSpan.FromMinutes(1.5).Ticks} "
        "{TimeSpan.FromSeconds(1.5).Ticks} "
        "{TimeSpan.FromMilliseconds(1.5).Ticks}\"); ")),
        "1296000000000 54000000000 900000000 15000000 15000");
}

// The admitting twin of the raise rows: the SAME shape with a legal value.
// Green before the fix and after it -- a positive control that the shape is
// genuinely admitted rather than rejected for an unrelated reason.
BEZEL_TEST(timespan_from_overflow_7576, from_unit_in_range_control) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        "Console.WriteLine($\"{TimeSpan.FromDays(0.0).Ticks} "
        "{TimeSpan.FromSeconds(1e11).Ticks}\"); ")),
        "0 1000000000000000000");
}

// ---------------------------------------------------------------------------
// The exact boundary. The `*` operator's contract says the range check
// "reads the double product", so the boundary is a property of the DOUBLE
// PRODUCT, not of the truncated tick count:
//
//   922337203685477.5  * 10000.0  =  9223372036854774784  (= 2^63 - 1024)
//       -> IN range: the largest double below 2^63, and the tick count is
//          that value exactly (the truncation toward zero changes nothing).
//   922337203685477.75 * 10000.0  =  9223372036854776832  (> 2^63)
//       -> OUT of range: OverflowException.
//
// (Both products are IEEE-754 doubles: the first is the largest double
// strictly below 2^63, the second the first double above it.)
// ---------------------------------------------------------------------------

BEZEL_TEST(timespan_from_overflow_7576, from_milliseconds_top_of_range_is_admitted) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        fromCall("TimeSpan.FromMilliseconds(922337203685477.5)"))),
        "ticks 9223372036854774784");
}

BEZEL_TEST(timespan_from_overflow_7576, from_milliseconds_past_top_of_range_raises) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        fromCall("TimeSpan.FromMilliseconds(922337203685477.75)"))), "OVF");
}
