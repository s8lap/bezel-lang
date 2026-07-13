// Tests for #525 and #526 — TimeSpan operator +/- and Encoding.UTF8.GetBytes
// Count fix.
//
// #525: stdlib §2.5 L264–265 declare
//   public static method operator +(a: TimeSpan, b: TimeSpan): TimeSpan;
//   public static method operator -(a: TimeSpan, b: TimeSpan): TimeSpan;
// Before the fix, `t1 + t2` (both TimeSpan) produced a compile-time error
// "operator + is not defined for type TimeSpan".
//
// #526: stdlib §3.2 declares
//   public abstract method GetBytes(s: string): owned Array<byte>;
// The inferred-type (.Count on a directly-chained call) returned 0 / garbage
// because the sema type of GetBytes's result was a lazy-sentinel (no astNode),
// so the Count-property codegen path did not recognise it as an array.
// The fix changes the sema result type to a concrete Array<byte> TypeRef
// (same synthetic astNode pattern used by ToArray/ToSet).
//
// Each test cites its stdlib section and verifies one specific behaviour.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

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
// #525 — TimeSpan operator + (stdlib §2.5 L264)
// ---------------------------------------------------------------------------

// stdlib §2.5 L264: operator +(a: TimeSpan, b: TimeSpan): TimeSpan.
// 2 days + 3 days = 5 days.
BEZEL_TEST(codegen_timespan_525, add_two_timespans_days) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t1 := TimeSpan.FromDays(2.0); "
        "var t2 := TimeSpan.FromDays(3.0); "
        "var sum := t1 + t2; "
        "return sum.Days; ")), 5);
}

// stdlib §2.5 L264: operator + result has correct Hours component.
// 1h30m + 2h45m = 4h15m (4 hours, 15 minutes).
BEZEL_TEST(codegen_timespan_525, add_two_timespans_hours_minutes) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t1 := TimeSpan.FromMinutes(90.0); "
        "var t2 := TimeSpan.FromMinutes(165.0); "
        "var sum := t1 + t2; "
        "return sum.Hours; ")), 4);
}

// stdlib §2.5 L264: operator + with zero span is identity.
// t + FromDays(0) = t.
BEZEL_TEST(codegen_timespan_525, add_zero_is_identity) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t1 := TimeSpan.FromDays(7.0); "
        "var zero := TimeSpan.FromDays(0.0); "
        "var sum := t1 + zero; "
        "return sum.Days; ")), 7);
}

// stdlib §2.5 L264: three-span addition is left-associative.
// 1h + 2h + 3h = 6h → Days = 0, Hours = 6.
BEZEL_TEST(codegen_timespan_525, add_three_timespans) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t1 := TimeSpan.FromHours(1.0); "
        "var t2 := TimeSpan.FromHours(2.0); "
        "var t3 := TimeSpan.FromHours(3.0); "
        "var sum := t1 + t2 + t3; "
        "return sum.Hours; ")), 6);
}

// ---------------------------------------------------------------------------
// #525 — TimeSpan operator - (stdlib §2.5 L265)
// ---------------------------------------------------------------------------

// stdlib §2.5 L265: operator -(a: TimeSpan, b: TimeSpan): TimeSpan.
// 5 days - 3 days = 2 days.
BEZEL_TEST(codegen_timespan_525, subtract_two_timespans_days) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t1 := TimeSpan.FromDays(5.0); "
        "var t2 := TimeSpan.FromDays(3.0); "
        "var diff := t1 - t2; "
        "return diff.Days; ")), 2);
}

// stdlib §2.5 L265: subtracting a larger span from a smaller gives a negative span.
// 2 days - 5 days = -3 days (Days == -3).
BEZEL_TEST(codegen_timespan_525, subtract_larger_yields_negative) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var t1 := TimeSpan.FromDays(2.0); "
        "var t2 := TimeSpan.FromDays(5.0); "
        "var diff := t1 - t2; "
        "Console.WriteLine(diff.Days); return 0; ")), "-3");
}

// stdlib §2.5 L265: t - t = 0 days (self-subtraction).
BEZEL_TEST(codegen_timespan_525, subtract_self_yields_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t1 := TimeSpan.FromHours(48.0); "
        "var diff := t1 - t1; "
        "return diff.Days; ")), 0);
}

// Control: DateTime.Add(TimeSpan) still works (per §2.5 L220).
// 2024-01-01 + 5 days → day 6.
BEZEL_TEST(codegen_timespan_525, control_datetime_add_timespan) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 1, 1); "
        "var span := TimeSpan.FromDays(5.0); "
        "var dt2 := dt.Add(span); "
        "return dt2.Day; ")), 6);
}

// Control: DateTime.Subtract(DateTime) → TimeSpan (per §2.5 L222).
// 2024-01-11 - 2024-01-01 = 10 days.
BEZEL_TEST(codegen_timespan_525, control_datetime_subtract_datetime) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new DateTime(2024, 1, 1); "
        "var b := new DateTime(2024, 1, 11); "
        "var span := b.Subtract(a); "
        "return span.Days; ")), 10);
}

// ---------------------------------------------------------------------------
// #526 — Encoding.UTF8.GetBytes Count field (stdlib §3.2)
// ---------------------------------------------------------------------------

// stdlib §3.2: GetBytes("Hello").Count == 5 (five ASCII bytes).
// This test uses .Count (not .Length) on the inferred-type result to exercise
// the bug path — the type must flow correctly to dispatch array-len codegen.
BEZEL_TEST(codegen_encoding_526, getbytes_count_ascii_hello_spec_3_2) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"Hello\"; "
        "using var b := Encoding.UTF8.GetBytes(s); "
        "return b.Count; ")), 5);
}

// stdlib §3.2: GetBytes("").Count == 0 (empty string → zero bytes).
BEZEL_TEST(codegen_encoding_526, getbytes_count_empty_spec_3_2) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := Encoding.UTF8.GetBytes(\"\"); "
        "return b.Count; ")), 0);
}

// stdlib §3.2: GetBytes("A").Count == 1 (single ASCII character).
BEZEL_TEST(codegen_encoding_526, getbytes_count_single_char_spec_3_2) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := Encoding.UTF8.GetBytes(\"A\"); "
        "return b.Count; ")), 1);
}

// stdlib §3.2: GetBytes for a multi-byte UTF-8 string — "café" (4 code points,
// 5 UTF-8 bytes: c=1, a=1, f=1, é=2).  Count must equal byte count (5).
BEZEL_TEST(codegen_encoding_526, getbytes_count_multibyte_spec_3_2) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"caf\\u00e9\"; "
        "using var b := Encoding.UTF8.GetBytes(s); "
        "return b.Count; ")), 5);
}

// stdlib §3.2: a 3-byte UTF-8 code point — U+4E2D (中) → 3 bytes.
// GetBytes("中").Count == 3.
BEZEL_TEST(codegen_encoding_526, getbytes_count_3byte_codepoint_spec_3_2) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"\\u4e2d\"; "
        "using var b := Encoding.UTF8.GetBytes(s); "
        "return b.Count; ")), 3);
}

// Control: GetString(GetBytes(s)) == s still works after the fix (§3.2).
BEZEL_TEST(codegen_encoding_526, control_roundtrip_ascii_spec_3_2) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"Hello, World\"; "
        "using var b := Encoding.UTF8.GetBytes(s); "
        "using var back: string := Encoding.UTF8.GetString(b); "
        "return (1 if back = s else 0); ")), 1);
}

// Control: explicit typed annotation (Array<byte>) still resolves Count (§3.2).
BEZEL_TEST(codegen_encoding_526, control_explicit_type_count_spec_3_2) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b: Array<byte> := Encoding.UTF8.GetBytes(\"ABCDE\"); "
        "return b.Count; ")), 5);
}
