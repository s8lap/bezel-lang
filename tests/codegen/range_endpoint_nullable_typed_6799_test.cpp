// Regression tests for #6799, #6801 and #6798 (language spec §16.3, stdlib
// §9.8, v0.340).
//
// `Range<T>.Start` and `.End` are `T?`: "a `null` endpoint indicates the range
// is open-ended on that side" (§16.3; stdlib §9.8 `End: T?; // null if
// open-ended on the upper side`).
//   #6799: an open-ended side read its stored 0 instead of null.
//   #6801: an untyped print of a Range<ulong>/Range<uint> endpoint printed it
//          signed, because the read carried no unsigned type.
//   #6798: sema left the read untyped, so `var q: string := r.Start` compiled.
// Also pinned: a typed `r.End!` / `P(r.Start!)` with a byte, sbyte, short or
// ushort return or parameter failed with "IR verification failed" because the
// endpoint was always carried at i32.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

std::string outOf(const std::string& body) {
    return runAotOut("module M; class C public static method Main(): int " +
                     body + " return 0; end Main; end C;");
}

// A byte/sbyte/short/ushort/uint/ulong/long/int/char element type: unwrapped
// endpoints flow through a same-typed parameter and return.
std::string typedFlow(const std::string& ty, const std::string& lo,
                      const std::string& hi) {
    return runAotOut(
        "module M; class C "
        "static method P(x: " + ty + "): " + ty + " return x; end P; "
        "static method G(r: Range<" + ty + ">): " + ty + " return r.End!; end G; "
        "static method H(r: Range<" + ty + ">): " + ty + " return P(r.Start!); end H; "
        "public static method Main(): int "
        "  var lo: " + ty + " := " + lo + "; "
        "  var hi: " + ty + " := " + hi + "; "
        "  var r := lo .. hi; "
        "  Console.WriteLine($\"{G(r)} {H(r)}\"); "
        "  return 0; "
        "end Main; end C;");
}

}  // namespace

// ---- #6799: open-ended sides are null --------------------------------------

BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_open_end_reads_null) {
    BEZEL_EXPECT_EQ(outOf(
        "  var lo: int := 3; var o := lo..; "
        "  Console.WriteLine(o.End = null); "
        "  Console.WriteLine(o.Start = null); "
        "  Console.WriteLine(o.Start!); "),
        "True\nFalse\n3");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_open_end_untyped_print_is_null) {
    // The issue's repro: the untyped print of an open End is the null text,
    // not the stored 0. Printed through a hole: a `T?` argument to
    // `Console.WriteLine` is a compile error (#6778, §7.1), and the hole
    // renders an absent value as `null` (§18.5).
    BEZEL_EXPECT_EQ(outOf(
        "  var lo: int := 3; var o := lo..; "
        "  Console.WriteLine($\"{o.End}\"); "
        "  Console.WriteLine(o.ToString()); "),
        "null\nRange(Start: 3, End: null, Step: 1, IsEndInclusive: True, "
        "IsOpenEnded: True, IsEmpty: False)");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_open_start_reads_null) {
    BEZEL_EXPECT_EQ(outOf(
        "  var hi: int := 9; var o := ..hi; "
        "  Console.WriteLine(o.Start = null); "
        "  Console.WriteLine(o.End!); "),
        "True\n9");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_open_end_null_coalesce) {
    BEZEL_EXPECT_EQ(outOf(
        "  var lo: int := 3; var o := lo..; "
        "  Console.WriteLine(o.End ?? -1); "
        "  Console.WriteLine(o.Start ?? -1); "),
        "-1\n3");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_closed_range_endpoints_non_null) {
    BEZEL_EXPECT_EQ(outOf(
        "  var lo: int := 3; var hi: int := 7; var r := lo .. hi; "
        "  Console.WriteLine(r.Start = null); "
        "  Console.WriteLine(r.End = null); "
        "  Console.WriteLine(r.Start! + r.End!); "),
        "False\nFalse\n10");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_open_ended_ulong_end_null) {
    BEZEL_EXPECT_EQ(outOf(
        "  var lo: ulong := 11000000000000000000UL; var o := lo..; "
        "  Console.WriteLine(o.End = null); "
        "  Console.WriteLine(o.Start!); "),
        "True\n11000000000000000000");
}

// ---- #6801: unsigned endpoints print unsigned ------------------------------
// The endpoint is `T?`, so it is printed through a hole `$"{r.Start}"`: passed
// bare to `Console.WriteLine` it is a compile error (#6778, §7.1, v0.340).

BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_ulong_untyped_print_unsigned) {
    BEZEL_EXPECT_EQ(outOf(
        "  var lo: ulong := 9000000000000000000UL; "
        "  var hi: ulong := 11000000000000000000UL; "
        "  var r := lo .. hi; "
        "  Console.WriteLine($\"{r.Start}\"); Console.WriteLine($\"{r.End}\"); "),
        "9000000000000000000\n11000000000000000000");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_uint_untyped_print_unsigned) {
    BEZEL_EXPECT_EQ(outOf(
        "  var ulo: uint := 4000000000U; var uhi: uint := 4000000005U; "
        "  var u := ulo .. uhi; "
        "  Console.WriteLine($\"{u.Start}\"); Console.WriteLine($\"{u.End}\"); "),
        "4000000000\n4000000005");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_ulong_max_interpolated) {
    BEZEL_EXPECT_EQ(outOf(
        "  var lo: ulong := 1UL; var hi: ulong := 18446744073709551615UL; "
        "  var r := lo .. hi; "
        "  Console.WriteLine($\"{r.Start} {r.End}\"); "),
        "1 18446744073709551615");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_byte_untyped_print) {
    BEZEL_EXPECT_EQ(outOf(
        "  var lo: byte := 100; var hi: byte := 200; var r := lo .. hi; "
        "  Console.WriteLine($\"{r.Start}\"); Console.WriteLine($\"{r.End}\"); "),
        "100\n200");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_ushort_untyped_print) {
    BEZEL_EXPECT_EQ(outOf(
        "  var lo: ushort := 100; var hi: ushort := 60000; var r := lo .. hi; "
        "  Console.WriteLine($\"{r.Start}\"); Console.WriteLine($\"{r.End}\"); "),
        "100\n60000");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_sbyte_short_long_untyped_print) {
    BEZEL_EXPECT_EQ(outOf(
        "  var a: sbyte := -100; var b: sbyte := 100; var r := a .. b; "
        "  Console.WriteLine($\"{r.Start}\"); Console.WriteLine($\"{r.End}\"); "
        "  var c: short := -30000; var d: short := 30000; var s := c .. d; "
        "  Console.WriteLine($\"{s.Start}\"); Console.WriteLine($\"{s.End}\"); "
        "  var e: long := -5000000000L; var f: long := 5000000000L; var t := e .. f; "
        "  Console.WriteLine($\"{t.Start}\"); Console.WriteLine($\"{t.End}\"); "),
        "-100\n100\n-30000\n30000\n-5000000000\n5000000000");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_char_untyped_print) {
    BEZEL_EXPECT_EQ(outOf(
        "  var lo: char := 'a'; var hi: char := 'z'; var r := lo .. hi; "
        "  Console.WriteLine($\"{r.Start}\"); Console.WriteLine($\"{r.End}\"); "),
        "a\nz");
}

// ---- typed flow at every element type (sign boundary) ----------------------

BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_typed_flow_byte) {
    BEZEL_EXPECT_EQ(typedFlow("byte", "3", "200"), "200 3");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_typed_flow_sbyte) {
    BEZEL_EXPECT_EQ(typedFlow("sbyte", "-100", "100"), "100 -100");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_typed_flow_short) {
    BEZEL_EXPECT_EQ(typedFlow("short", "-30000", "30000"), "30000 -30000");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_typed_flow_ushort) {
    BEZEL_EXPECT_EQ(typedFlow("ushort", "3", "60000"), "60000 3");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_typed_flow_int) {
    BEZEL_EXPECT_EQ(typedFlow("int", "-2000000000", "2000000000"),
                    "2000000000 -2000000000");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_typed_flow_uint) {
    BEZEL_EXPECT_EQ(typedFlow("uint", "3U", "4000000000U"), "4000000000 3");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_typed_flow_long) {
    BEZEL_EXPECT_EQ(typedFlow("long", "-5000000000L", "5000000000L"),
                    "5000000000 -5000000000");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_typed_flow_ulong) {
    BEZEL_EXPECT_EQ(typedFlow("ulong", "3UL", "11000000000000000000UL"),
                    "11000000000000000000 3");
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_typed_flow_char) {
    BEZEL_EXPECT_EQ(typedFlow("char", "'a'", "'z'"), "z a");
}

// ---- #6798: the read is typed T? -------------------------------------------

BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_start_to_string_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var r := 1 .. 5; var q: string := r.Start; "
        "  Console.WriteLine(q); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "the initializer for `q` is nullable but the target type is non-nullable"));
}
BEZEL_TEST(range_endpoint_nullable_typed_6799, spec_16_3_nullable_end_to_int_return_rejected) {
    // `T?` into an `int` return needs the `!` (or `??`); the bare read is a
    // nullable-to-non-nullable conversion (§16.3, stdlib §9.8).
    auto r = runAot(
        "module M; class C static method F(r: Range<int>): int "
        "  return r.End; end F; "
        "public static method Main(): int return F(1 .. 5); end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "the returned value is nullable but the target type is non-nullable"));
}
