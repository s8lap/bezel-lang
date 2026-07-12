// fix #1159 / §5.2 (spec v0.154): a SIGNED → wider-UNSIGNED explicit integer
// cast is a CHECKED value conversion. An explicit integer conversion "preserves
// the numeric value or fails"; a negative value is not representable in any
// unsigned type, so it must raise OverflowException — not silently sign-extend.
//
// Previously the widening branch of lowerConversion sign-extended a negative
// signed source into a huge positive value (e.g. ulong(-1) = 2^64-1) with no
// check. The narrowing path (#1133) already range-checks and the same-width
// path (#753) checks the sign bit; this closes the widening-to-unsigned gap.
//
// Widening-to-unsigned family (signed source narrower than unsigned dest):
//   sbyte -> {ushort, uint, ulong}, short -> {uint, ulong}, int -> ulong.
// (long -> ulong and every uint->uint pair are SAME width — handled by #753.)

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
// Wrap `body` (a cast expected to overflow) in try/catch returning 0 when
// OverflowException is raised (pass) and 7 otherwise (fail).
int expectsThrow(const std::string& body) {
    return runFull(
        "module M; class C public static method Main(): int "
        "  var ok: int := 7; "
        "  try " + body + " "
        "  catch var ex: OverflowException => do ok := 0; end "
        "  end; "
        "  return ok; "
        "end Main; end C;");
}
}  // namespace

// §5.2/#1159: the issue's primary repro — int(-1) → ulong sign-extends to
// 2^64-1 without the fix; must raise OverflowException.
BEZEL_TEST(widen_to_unsigned_1159, int_neg_to_ulong_throws) {
    BEZEL_EXPECT_EQ(expectsThrow(
        "var a: int := -1; var b: ulong := ulong(a); ok := (1 if b > 0 else 2);"), 0);
}

// §5.2/#1159: short(-1) → uint widening (i16 → i32) is checked.
BEZEL_TEST(widen_to_unsigned_1159, short_neg_to_uint_throws) {
    BEZEL_EXPECT_EQ(expectsThrow(
        "var a: short := -1; var b: uint := uint(a); ok := (1 if b > 0 else 2);"), 0);
}

// §5.2/#1159: short(-1) → ulong widening (i16 → i64) is checked.
BEZEL_TEST(widen_to_unsigned_1159, short_neg_to_ulong_throws) {
    BEZEL_EXPECT_EQ(expectsThrow(
        "var a: short := -1; var b: ulong := ulong(a); ok := (1 if b > 0 else 2);"), 0);
}

// §5.2/#1159: sbyte(-1) → ushort widening (i8 → i16) is checked.
BEZEL_TEST(widen_to_unsigned_1159, sbyte_neg_to_ushort_throws) {
    BEZEL_EXPECT_EQ(expectsThrow(
        "var a: sbyte := -1; var b: ushort := ushort(a); ok := (1 if b > 0 else 2);"), 0);
}

// §5.2/#1159: sbyte(-1) → uint widening (i8 → i32) is checked.
BEZEL_TEST(widen_to_unsigned_1159, sbyte_neg_to_uint_throws) {
    BEZEL_EXPECT_EQ(expectsThrow(
        "var a: sbyte := -1; var b: uint := uint(a); ok := (1 if b > 0 else 2);"), 0);
}

// §5.2/#1159: sbyte(-1) → ulong widening (i8 → i64) is checked.
BEZEL_TEST(widen_to_unsigned_1159, sbyte_neg_to_ulong_throws) {
    BEZEL_EXPECT_EQ(expectsThrow(
        "var a: sbyte := -1; var b: ulong := ulong(a); ok := (1 if b > 0 else 2);"), 0);
}

// §5.2/#1159: a NON-negative signed → wider-unsigned cast is in range — it
// preserves the numeric value and must NOT throw (regression guard).
BEZEL_TEST(widen_to_unsigned_1159, nonneg_signed_to_unsigned_ok) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  var a: int := 5; "
        "  var b: ulong := ulong(a); "
        "  var c: short := 7; "
        "  var d: uint := uint(c); "
        "  return (0 if (b = 5UL and d = 7u) else 1); "
        "end Main; end C;"), 0);
}

// §5.2/#1159: signed → wider-SIGNED widening is unaffected — a negative value
// sign-extends and is preserved (long(int -1) = -1), no spurious throw.
BEZEL_TEST(widen_to_unsigned_1159, signed_to_signed_widen_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  var a: int := -1; "
        "  var b: long := long(a); "
        "  return (0 if b = -1L else 1); "
        "end Main; end C;"), 0);
}

// §5.2/#1159: unsigned → wider-unsigned widening is unaffected (zero-extends),
// and unsigned → wider-signed widening fits without a check.
BEZEL_TEST(widen_to_unsigned_1159, unsigned_widen_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  var a: uint := 4294967295u; "
        "  var b: ulong := ulong(a); "       // unsigned → wider unsigned: zext
        "  var c: long := long(a); "          // unsigned → wider signed: fits
        "  return (0 if (b = 4294967295UL and c = 4294967295L) else 1); "
        "end Main; end C;"), 0);
}
