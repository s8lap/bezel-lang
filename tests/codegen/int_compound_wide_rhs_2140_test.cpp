// Codegen — #2140 (§6.6 / §6.2 / §6.1): an ARITHMETIC compound assignment whose
// §6.2-promoted result is WIDER than the LHS type must run the operation at the
// promoted width and apply the §6.6 CHECKED narrow-integer write-back — matching
// the explicit-cast form — not silently truncate the RHS to the LHS width, and
// not emit an invalid narrowing cast (crash).
//
// Per §6.6 (spec v0.214) the compound `x op= e` is `x := x op e` with the result
// typed as R = type of `x op e`; §6.2 promotes the smaller operand to the wider
// type, so `int + long` runs in `long` (R = long). When T (the LHS type) is an
// integer and R is a wider integer, the result is converted back to T by the
// §5.2 CHECKED narrowing: a value outside T's range raises OverflowException
// ("§6.1's checked posture; never a silent truncation").
//
// Two sibling defects (same class as the #1996 char fix, 0535081), both
// pre-existing in emitCompoundOp:
//   (a) `int/uint += long/ulong` hit the #1042 tail `CreateIntCast(r, l->type)`
//       BEFORE the arithmetic — truncating the i64 RHS to i32. `i += 2^32L`
//       (2^32) truncated to 0 and STORED 0 with no raise; `i += (2^32+65)L`
//       truncated to a WRONG in-range 65.
//   (b) `byte/sbyte/short/ushort += long` entered the sub-i32 checked-narrow
//       branch, whose extend-to-i32 emitted a narrowing ZExt/SExt on the i64 RHS
//       — an invalid cast (malformed IR / assert crash).
// The fix runs the op at the promoted (wider) width per §6.2 — each operand
// extended by its OWN signedness — then narrow-CHECKS back to the LHS type, so a
// `long` RHS reaches the check untruncated and a signed `long` computes signed
// (`byte(5) += -1L` = 4, not a spurious unsigned overflow).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Body-only program returning an int exit code.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Returns 1 if `body` raises OverflowException, 0 if it runs to completion,
// -999 on a front-end failure.
int throwsOverflow(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") +
        body +
        " return 0; catch OverflowException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// --- Bug (a): int += long truncation. The exact issue repro `i += 2^32L` must
// RAISE (§6.6 checked narrow), not truncate 2^32 → 0 and store 0. FAILS WITHOUT
// the fix (stored 0, exit 0). ---
BEZEL_TEST(codegen_2140_int_wide_rhs, int_plus_pow2_32_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var i: int := 0; i += 4294967296L;"), 1);
}

// The sharpest case: `int(0) += (2^32+65)L` truncated to a VALID in-range 65 —
// a WRONG value silently stored. The full-width result is out of int range →
// raise. FAILS WITHOUT the fix (stored 65, exit 0).
BEZEL_TEST(codegen_2140_int_wide_rhs, int_plus_pow2_32_plus_65_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var i: int := 0; i += 4294967361L;"), 1);
}

// PARITY CONTROL — the explicit-cast form raises on the identical input; the
// compound (above) must agree (§6.6 compound-vs-cast parity).
BEZEL_TEST(codegen_2140_int_wide_rhs, explicit_cast_pow2_32_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var i: int := 0; var e: int := int(i + 4294967296L);"), 1);
}

// In-range `long` RHS still stores (no false raise): int(0) += 65L → 65.
BEZEL_TEST(codegen_2140_int_wide_rhs, int_plus_in_range_long_stores) {
    BEZEL_EXPECT_EQ(run("var i: int := 0; i += 65L; return i;"), 65);
}

// A wide multiply overflows the promoted width: 65536 *= 65536L = 2^32 → raise.
BEZEL_TEST(codegen_2140_int_wide_rhs, int_times_wide_overflow_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var i: int := 65536; i *= 65536L;"), 1);
}

// In-range wide multiply/divide still store: 3 *= 1_000_000L = 3_000_000;
// 100 /= 3L = 33.
BEZEL_TEST(codegen_2140_int_wide_rhs, int_times_wide_in_range_stores) {
    // (Result exceeds the 8-bit exit code, so verify via equality.)
    BEZEL_EXPECT_EQ(run(
        "var i: int := 3; i *= 1000000L; "
        "if i = 3000000 then return 1; end; return 0;"), 1);
}
BEZEL_TEST(codegen_2140_int_wide_rhs, int_div_wide_stores) {
    BEZEL_EXPECT_EQ(run("var i: int := 100; i /= 3L; return i;"), 33);
}

// A wide SIGNED long RHS computes signed — `int(5) += -1L` = 4 (a spurious
// unsigned overflow would raise). In range → stores 4.
BEZEL_TEST(codegen_2140_int_wide_rhs, int_plus_negative_long_stores) {
    BEZEL_EXPECT_EQ(run("var i: int := 5; i += -1L; return i;"), 4);
}

// --- uint += ulong. `uint(0) += 2^32UL` is out of uint range (max 2^32-1) →
// raise (was truncated to 0). ---
BEZEL_TEST(codegen_2140_int_wide_rhs, uint_plus_pow2_32_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var u: uint := 0U; u += 4294967296UL;"), 1);
}

// An UNSIGNED wide sum that lands in uint range (but exceeds int range) must be
// stored, not spuriously raise: uint(5) += 3_000_000_000UL = 3_000_000_005U.
BEZEL_TEST(codegen_2140_int_wide_rhs, uint_plus_ulong_in_range_stores) {
    BEZEL_EXPECT_EQ(run(
        "var u: uint := 5U; u += 3000000000UL; "
        "if u = 3000000005U then return 1; end; return 0;"), 1);
}

// --- Bug (b): byte/sbyte/short/ushort += long. A CONSTANT long RHS truncated
// (silent wrong value); a VARIABLE long RHS emitted an invalid narrowing cast
// (crash). Both forms must run the checked narrow at full width. ---
// byte(200) += 100L = 300, out of byte range → raise (constant RHS).
BEZEL_TEST(codegen_2140_subint_wide_rhs, byte_plus_long_overflow_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var b: byte := 200; b += 100L;"), 1);
}

// A VARIABLE long RHS is the crash repro — the invalid i64→i32 narrowing cast.
// In range (byte(200) += 30L = 230) it must compile AND store 230.
BEZEL_TEST(codegen_2140_subint_wide_rhs, byte_plus_variable_long_in_range) {
    BEZEL_EXPECT_EQ(
        run("var b: byte := 200; var v: long := 30L; b += v; return int(b);"),
        230);
}

// A VARIABLE long RHS that overflows byte range must raise (not crash, not
// truncate): byte(200) += variable 100L = 300 → raise.
BEZEL_TEST(codegen_2140_subint_wide_rhs, byte_plus_variable_long_overflow) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var b: byte := 200; var v: long := 100L; b += v;"), 1);
}

// A signed sub-i32 LHS with a wide SIGNED long RHS computes signed:
// sbyte(10) -= 100L = -90 (in sbyte range -128..127) → stores -90. (A spurious
// unsigned overflow would raise; a truncation would store a wrong value.)
BEZEL_TEST(codegen_2140_subint_wide_rhs, sbyte_minus_long_negative_stores) {
    BEZEL_EXPECT_EQ(run(
        "var s: sbyte := 10; s -= 100L; "
        "if s = -90 then return 1; end; return 0;"), 1);
}

// short += 2^40L is far out of short range → raise (was truncated to 0).
BEZEL_TEST(codegen_2140_subint_wide_rhs, short_plus_wide_long_throws) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var s: short := 0; s += 1099511627776L;"), 1);
}

// --- Signedness fix also corrects a sub-i32 LHS with a wider SIGNED int RHS
// (the promoted type is `int`, signed): `byte(5) += -1` = 4, not a spurious
// unsigned overflow. (Latent in the prior #1043 branch, which ran the i32
// arithmetic with the LHS/byte unsigned posture.) ---
BEZEL_TEST(codegen_2140_subint_wide_rhs, byte_plus_negative_int_stores) {
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b += -1; return int(b);"), 4);
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b += -1L; return int(b);"), 4);
}

// --- Collateral controls — neighbor cells the fix must NOT perturb. ---
// int += int (same width): no promotion, checked at i32. In range and overflow.
BEZEL_TEST(codegen_2140_neighbors, int_plus_int_unchanged) {
    BEZEL_EXPECT_EQ(
        run("var i: int := 2000000000; i += 100; return i - 2000000000;"), 100);
    BEZEL_EXPECT_EQ(
        throwsOverflow("var i: int := 2000000000; i += 2000000000;"), 1);
}

// long += int (RHS pre-widened up to i64): 9_000_000_000L + 5.
BEZEL_TEST(codegen_2140_neighbors, long_plus_int_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "var l: long := 9000000000L; l += 5; "
        "if l = 9000000005L then return 1; end; return 0;"), 1);
}

// int += byte (narrower RHS, ZExt-widened up at the call site): byte 200 keeps
// its non-negative value (not sign-extended to -56).
BEZEL_TEST(codegen_2140_neighbors, int_plus_byte_unchanged) {
    BEZEL_EXPECT_EQ(
        run("var i: int := 0; var b: byte := 200; i += b; return i;"), 200);
}

// byte += int (in range) and the sub-i32 checked narrow (#1042/#1043) unchanged.
BEZEL_TEST(codegen_2140_neighbors, byte_plus_int_unchanged) {
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b += 3; return int(b);"), 8);
    BEZEL_EXPECT_EQ(throwsOverflow("var b: byte := 200; b += 100;"), 1);
}

// byte += byte (same width): 200 + 100 = 300, out of byte range → raise.
BEZEL_TEST(codegen_2140_neighbors, byte_plus_byte_overflow_unchanged) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var b: byte := 200; var c: byte := 100; b += c;"), 1);
}

// Bitwise / shift compounds stay on the LHS-width path (not the narrow-check):
// byte &= 3 → 1; byte <<= 1 → 10.
BEZEL_TEST(codegen_2140_neighbors, byte_bitwise_shift_unchanged) {
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b &= 3; return int(b);"), 1);
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b <<= 1; return int(b);"), 10);
}
