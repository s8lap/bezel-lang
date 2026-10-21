// Regression tests for #6774 (language spec §16.3, v0.340, Range operations).
//
// §16.3 defines `IsEmpty` by iteration: a range is empty when iteration yields
// no values, which is the lower bound exceeding the upper bound in the
// direction of iteration. "Exceeds" is the order of the element type T, so an
// unsigned T must compare its endpoints as unsigned values. Before this fix
// the member read compared the sign-extended endpoints as signed i64 whatever
// T was, so:
//   - 9000000000000000000UL .. 11000000000000000000UL reported IsEmpty true
//     (11e18 is negative as an i64), and the reversed range reported false;
//   - byte 200 .. 50 and uint 4000000000U .. 100U were ordered by their
//     sign-extended storage, not by value.
// The `Start`/`End` reads (stdlib §9.8: `Start: T?`, `End: T?`) truncated every
// endpoint to i32, so long and ulong values outside i32 came back wrong. A long
// or ulong endpoint now carries its full i64 width; a byte or ushort endpoint is
// zero-extended and a signed narrow one sign-extended. uint stays i32 (its bits
// are right for a typed read, and a uint consumer is typed i32).
//
// Deliberately NOT asserted here, because each is a separate defect filed as
// its own issue: an UNTYPED `Console.WriteLine` of a ulong endpoint at or above
// 2^63, or of a uint endpoint at or above 2^31, prints it signed (sema leaves
// `Range.Start`/`End` untyped, so the codegen has no unsigned type to print by);
// the ToString rendering of a byte, ushort or uint endpoint is sign-extended; and
// iteration over a ulong range that straddles 2^63 visits no values. The ToString
// rows below assert only the IsEmpty field and the correctly rendered Start,
// never the endpoint text that is wrong.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// Captured stdout of the program on the AOT path (the trailing line ending is
// stripped by runAotOut, so two lines are joined by "\n"). A program that does
// not compile or run yields runAotOut's "<did-not-run>" sentinel, which fails
// the EXPECT.
std::string outOf(const std::string& prog) {
    return runAotOut(prog);
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

// §16.3, v0.340: a ulong range straddling 2^63 is ordered as unsigned values.
// 9e18 < 11e18, so `lo .. hi` is non-empty and `hi .. lo` is empty.
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_ulong_straddle_isempty_inclusive_both_orders) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: ulong := 9000000000000000000UL; "
        "  var hi: ulong := 11000000000000000000UL; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.IsEmpty); "
        "  var e := hi .. lo; "
        "  Console.WriteLine(e.IsEmpty); "
        "  return 0; "
        "end Main; end C;"),
        "False\nTrue");
}

// §16.3, v0.340: the same ordering for the exclusive upper bound `..<`.
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_ulong_straddle_isempty_exclusive_both_orders) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: ulong := 9000000000000000000UL; "
        "  var hi: ulong := 11000000000000000000UL; "
        "  var r := lo ..< hi; "
        "  Console.WriteLine(r.IsEmpty); "
        "  var e := hi ..< lo; "
        "  Console.WriteLine(e.IsEmpty); "
        "  return 0; "
        "end Main; end C;"),
        "False\nTrue");
}

// §16.3, v0.340: a descending range with a negative step is non-empty when its
// direction agrees with the step, and the unsigned order decides that too.
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_ulong_straddle_descending_step_not_empty) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: ulong := 9000000000000000000UL; "
        "  var hi: ulong := 11000000000000000000UL; "
        "  var d := hi .. lo step -1L; "
        "  Console.WriteLine(d.IsEmpty); "
        "  return 0; "
        "end Main; end C;"),
        "False");
}

// stdlib §9.8 / §16.3, v0.340: a ulong endpoint at or above 2^63 read through a typed
// local keeps its value (an unsigned read of the 64 stored bits).
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_ulong_straddle_start_end_typed_read) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: ulong := 9000000000000000000UL; "
        "  var hi: ulong := 11000000000000000000UL; "
        "  var r := lo .. hi; "
        "  var s: ulong := r.Start; "
        "  var e: ulong := r.End; "
        "  Console.WriteLine(s); "
        "  Console.WriteLine(e); "
        "  return 0; "
        "end Main; end C;"),
        "9000000000000000000\n11000000000000000000");
}

// §16.3, v0.340: the rendered `IsEmpty` field agrees with the member read
// (§18.5 renders the same emptiness). Only the IsEmpty suffix and the
// correctly rendered Start are asserted; the End text is a separate defect.
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_ulong_straddle_tostring_is_empty_agrees) {
    const std::string s = outOf(
        "module M; class C public static method Main(): int "
        "  var lo: ulong := 9000000000000000000UL; "
        "  var hi: ulong := 11000000000000000000UL; "
        "  var a := lo .. hi; "
        "  Console.WriteLine(a.ToString()); "
        "  var b := hi .. lo; "
        "  Console.WriteLine(b.ToString()); "
        "  return 0; "
        "end Main; end C;");
    const auto nl = s.find('\n');
    const std::string first = nl == std::string::npos ? s : s.substr(0, nl);
    const std::string second = nl == std::string::npos ? std::string() : s.substr(nl + 1);
    BEZEL_EXPECT_TRUE(first.rfind("Range(Start: 9000000000000000000, End: ", 0) == 0);
    BEZEL_EXPECT_TRUE(endsWith(first, "IsEmpty: False)"));
    BEZEL_EXPECT_TRUE(endsWith(second, "IsEmpty: True)"));
}

// §16.3, v0.340: a uint range whose lower bound has the top bit set (4000000000
// is negative as i32) is ordered as unsigned: 4e9 > 100, so `lo .. hi` is empty.
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_uint_top_bit_isempty_flips) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: uint := 4000000000U; "
        "  var hi: uint := 100U; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.IsEmpty); "
        "  var e := hi .. lo; "
        "  Console.WriteLine(e.IsEmpty); "
        "  return 0; "
        "end Main; end C;"),
        "True\nFalse");
}

// §16.3, v0.340 control: both endpoints of a uint range have the top bit set and
// are ordered the same way by signed and unsigned comparison.
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_uint_both_top_bit_control) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: uint := 3000000000U; "
        "  var hi: uint := 4000000000U; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.IsEmpty); "
        "  var e := hi .. lo; "
        "  Console.WriteLine(e.IsEmpty); "
        "  return 0; "
        "end Main; end C;"),
        "False\nTrue");
}

// stdlib §9.8, v0.340 control: a uint endpoint above 2^31 reads back at its value through
// typed locals. The untyped print of such an endpoint is not asserted (see the
// header): it is a separate defect.
BEZEL_TEST(range_unsigned_isempty_6774, stdlib_9_8_uint_top_bit_start_end_typed_read) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: uint := 4000000000U; "
        "  var hi: uint := 4000000005U; "
        "  var r := lo .. hi; "
        "  var s: uint := r.Start; "
        "  var e: uint := r.End; "
        "  Console.WriteLine(s); "
        "  Console.WriteLine(e); "
        "  return 0; "
        "end Main; end C;"),
        "4000000000\n4000000005");
}

// §16.3, v0.340: a byte range whose lower bound has the top bit set (200 is
// negative as i8) is ordered as unsigned: 200 > 50, so `lo .. hi` is empty.
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_byte_top_bit_isempty_flips) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: byte := 200; "
        "  var hi: byte := 50; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.IsEmpty); "
        "  var e := hi .. lo; "
        "  Console.WriteLine(e.IsEmpty); "
        "  return 0; "
        "end Main; end C;"),
        "True\nFalse");
}

// stdlib §9.8, v0.340: byte endpoints with the top bit set read back at their values
// (200 and 255), not as the sign-extended -56 and -1.
BEZEL_TEST(range_unsigned_isempty_6774, stdlib_9_8_byte_top_bit_start_end_values) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: byte := 200; "
        "  var hi: byte := 255; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.Start); "
        "  Console.WriteLine(r.End); "
        "  return 0; "
        "end Main; end C;"),
        "200\n255");
}

// §16.3, v0.340 control: a signed int range that straddles zero.
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_int_signed_control) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: int := -5; "
        "  var hi: int := 5; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.IsEmpty); "
        "  var e := hi .. lo; "
        "  Console.WriteLine(e.IsEmpty); "
        "  return 0; "
        "end Main; end C;"),
        "False\nTrue");
}

// stdlib §9.8, v0.340: a long range straddling zero keeps its endpoints at full i64
// width (5000000000 and -5000000000 do not fit i32).
BEZEL_TEST(range_unsigned_isempty_6774, stdlib_9_8_long_signed_start_end_values) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: long := -5000000000L; "
        "  var hi: long := 5000000000L; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.IsEmpty); "
        "  Console.WriteLine(r.Start); "
        "  Console.WriteLine(r.End); "
        "  return 0; "
        "end Main; end C;"),
        "False\n-5000000000\n5000000000");
}

// stdlib §9.8, v0.340 control: sbyte endpoints of both signs read back sign-extended.
BEZEL_TEST(range_unsigned_isempty_6774, stdlib_9_8_sbyte_signed_start_end_control) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: sbyte := -100; "
        "  var hi: sbyte := 100; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.Start); "
        "  Console.WriteLine(r.End); "
        "  return 0; "
        "end Main; end C;"),
        "-100\n100");
}

// §16.3, v0.340 control: a ulong range that does not straddle 2^63.
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_ulong_small_control) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: ulong := 1UL; "
        "  var hi: ulong := 2UL; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.IsEmpty); "
        "  var e := hi .. lo; "
        "  Console.WriteLine(e.IsEmpty); "
        "  return 0; "
        "end Main; end C;"),
        "False\nTrue");
}

// §16.3, v0.340 control: both endpoints of a ulong range lie above 2^63, where
// signed and unsigned comparison agree.
BEZEL_TEST(range_unsigned_isempty_6774, spec_16_3_ulong_above_2_63_control) {
    BEZEL_EXPECT_EQ(outOf(
        "module M; class C public static method Main(): int "
        "  var lo: ulong := 11000000000000000000UL; "
        "  var hi: ulong := 12000000000000000000UL; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.IsEmpty); "
        "  var e := hi .. lo; "
        "  Console.WriteLine(e.IsEmpty); "
        "  return 0; "
        "end Main; end C;"),
        "False\nTrue");
}
