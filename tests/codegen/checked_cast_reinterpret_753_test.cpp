// fix #753 / §5.2 + stdlib §2.12: a same-width signed↔unsigned explicit cast
// (int↔uint, long↔ulong, short↔ushort, byte↔sbyte) is a CHECKED value
// conversion — it raises OverflowException when the value is not representable
// under the destination's signedness (Reading A). Previously the same-width
// case fell through `lowerConversion`'s `srcTy == dstTy` early-return and
// silently bit-reinterpreted (e.g. int(4294967295u) yielded -1 with no check).
//
// The zero-cost bit reinterpretation is now the explicit, named surface
// `BitConverter.Reinterpret<TTo>(value)` (stdlib §2.12): target type explicit,
// source inferred, same bit width required, lowered to an LLVM bitcast (a no-op
// when the LLVM types coincide, as for int↔uint). A width mismatch is a clean
// compile error.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
bool rejectedAtCompile(const std::string& program) {
    auto r = runJit(program);
    return !r.ran && r.diagnostics.hasErrors();
}
// Wrap `body` (which performs a cast expected to overflow) in a try/catch that
// returns 0 when OverflowException is raised (pass) and 7 otherwise (fail).
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

// §5.2/#753: a negative value cast to a same-width unsigned type does not fit →
// OverflowException (was: silently reinterpreted to a large positive value).
BEZEL_TEST(checked_cast_reinterpret_753, signed_to_unsigned_negative_throws) {
    BEZEL_EXPECT_EQ(expectsThrow(
        "var n: int := -1; var u: uint := uint(n); ok := (1 if u > 0u else 2);"), 0);
}

// §5.2/#753: an unsigned value above the signed maximum cast to the same-width
// signed type does not fit → OverflowException (was: reinterpreted to -1).
BEZEL_TEST(checked_cast_reinterpret_753, unsigned_to_signed_above_max_throws) {
    BEZEL_EXPECT_EQ(expectsThrow(
        "var u: uint := 4294967295u; var i: int := int(u); ok := (1 if i = i else 2);"), 0);
}

// §5.2/#753: long↔ulong is checked too — a negative long to ulong overflows.
BEZEL_TEST(checked_cast_reinterpret_753, long_to_ulong_negative_throws) {
    BEZEL_EXPECT_EQ(expectsThrow(
        "var m: long := -5L; var u: ulong := ulong(m); ok := (1 if u > 0UL else 2);"), 0);
}

// §5.2: an in-range same-width flip succeeds in BOTH directions and preserves
// the numeric value (no spurious throw for representable values).
BEZEL_TEST(checked_cast_reinterpret_753, same_width_in_range_ok) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  var n: int := 5; "
        "  var u: uint := uint(n); "
        "  var n2: int := int(u); "
        "  return (0 if (u = 5u and n2 = 5) else 1); "
        "end Main; end C;"), 0);
}

// §5.2: cross-width narrowing remains checked (regression guard — the #753 fix
// must not disturb the existing wider→narrower overflow check).
BEZEL_TEST(checked_cast_reinterpret_753, cross_width_still_checked) {
    BEZEL_EXPECT_EQ(expectsThrow(
        "var big: long := 5000000000L; var i: int := int(big); ok := (1 if i = i else 2);"), 0);
}

// stdlib §2.12/#753: Reinterpret<int> of an unsigned value above int.MaxValue
// yields the bit-identical signed value (0xFFFFFFFF → -1) and never throws.
BEZEL_TEST(checked_cast_reinterpret_753, reinterpret_uint_to_int) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  var u: uint := 4294967295u; "
        "  return (0 if BitConverter.Reinterpret<int>(u) = -1 else 1); "
        "end Main; end C;"), 0);
}

// stdlib §2.12/#753: the reverse direction — Reinterpret<uint> of -1 is 2^32-1.
BEZEL_TEST(checked_cast_reinterpret_753, reinterpret_int_to_uint) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  var n: int := -1; "
        "  return (0 if BitConverter.Reinterpret<uint>(n) = 4294967295u else 1); "
        "end Main; end C;"), 0);
}

// stdlib §2.12/#753: 64-bit reinterpret — long.MinValue's bits are exactly 2^63
// when viewed as ulong (the case the rewritten int_literal test now relies on).
BEZEL_TEST(checked_cast_reinterpret_753, reinterpret_long_to_ulong) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  var m: long := -9223372036854775808L; "
        "  return (0 if BitConverter.Reinterpret<ulong>(m) = 9223372036854775808UL else 1); "
        "end Main; end C;"), 0);
}

// stdlib §2.12/#753: Reinterpret never raises, even for a value the checked cast
// would reject — it is a pure bit view, not a value conversion.
BEZEL_TEST(checked_cast_reinterpret_753, reinterpret_never_throws) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  var n: int := -42; "
        "  var u: uint := BitConverter.Reinterpret<uint>(n); "
        "  return (0 if BitConverter.Reinterpret<int>(u) = -42 else 1); "  // round-trips
        "end Main; end C;"), 0);
}

// stdlib §2.12/#753: a width mismatch (int source, byte target) is a clean
// compile-time error, not a malformed bitcast.
BEZEL_TEST(checked_cast_reinterpret_753, reinterpret_width_mismatch_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  var n: int := 5; "
        "  var b: byte := BitConverter.Reinterpret<byte>(n); "
        "  return int(b); "
        "end Main; end C;"));
}
