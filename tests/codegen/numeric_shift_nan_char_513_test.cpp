// Tests for codegen fixes: #513 shift-count domain, #514 NaN reflexive equality,
// #517 char(n) range guard. All derived from the Bezel spec.
//
// #513: §6.5 — the shift-count cells. ⚠ THE RULE THESE PIN CHANGED AT v0.242
//   (proposal 0079, #3045). They used to pin "The shift count is reduced modulo
//   the operand's bit width", so `1 << 33` was 2 and `n << cnt` with cnt = 32
//   was 1. §6.5 now reads: "The shift count's **domain** is `0` through
//   `width−1` … A count outside the domain — negative, or ≥ width — is a fault:
//   a **compile error** when the count is a compile-time constant (§17.14), and
//   `OverflowException` at runtime otherwise." So every cell below that used to
//   assert a MASKED VALUE now asserts a FAULT, at the arm the count's form
//   selects: constant → compile error, runtime → OverflowException. The
//   in-domain cells (`1 << 0`, `1 << 3`, `-8 >> 1`) are bit-for-bit unchanged —
//   "in-domain shifts are bit-for-bit unchanged" (v0.242 changelog). The
//   deliberate-masking spelling §6.5 prescribes, `v << (k & 31)`, is pinned by
//   the new sibling file shift_count_domain_3045_test.cpp.
//
// #514: §4.3.1 (L308/L314): "NaN = NaN is true" (deliberate, for Set<double>
//   and Dictionary key semantics). IEEE <,<=,>,>= comparisons are unchanged
//   (NaN comparisons remain false per IEEE 754).
//
// #517: §5.2 (L421): "Casting an integer to char requires the integer to fall
//   in U+0000–U+10FFFF excluding the surrogate range; otherwise
//   OverflowException." §5.2 (L2283): "char(0xD800) raises OverflowException."

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// §6.5/§17.14 (v0.242): a CONSTANT out-of-domain count is a compile error.
// Returns true when the program is rejected AND the diagnostic is the §6.5
// domain one (not some unrelated reject).
bool rejectsConstCount(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is outside the domain") != std::string::npos &&
            d.message.find("(\xC2\xA7""6.5/\xC2\xA7""17.14)") !=
                std::string::npos)
            return true;
    return false;
}

// §6.5/§31.4 (v0.242): a RUNTIME out-of-domain count raises OverflowException.
// 1 = raised, 0 = ran to completion, -999 = front-end failure.
int throwsOverflow(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") +
        body +
        " return 0; catch OverflowException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ---------------------------------------------------------------------------
// #513 — §6.5: the shift count's DOMAIN (v0.242, proposal 0079 / #3045).
// Every cell below asserted a masked VALUE before v0.242; each now asserts the
// fault §6.5 prescribes. The "was" value is recorded in each comment.
// ---------------------------------------------------------------------------

// §6.5 (v0.242): a constant count of 32 is outside `int`'s `0..31` domain — a
// compile error (§17.14). WAS: `1 << 32` == 1 (32 % 32 == 0, shift by 0).
BEZEL_TEST(codegen_cg_513, shift_left_const_count_32_rejected_spec_6_5) {
    BEZEL_EXPECT_TRUE(rejectsConstCount("var n := 1; return n << 32;"));
}

// §6.5 (v0.242): likewise 33. WAS: `1 << 33` == 2 (33 % 32 == 1).
BEZEL_TEST(codegen_cg_513, shift_left_const_count_33_rejected_spec_6_5) {
    BEZEL_EXPECT_TRUE(rejectsConstCount("var n := 1; return n << 33;"));
}

// §6.5: `1 << 0` is 1 — IN DOMAIN, unchanged by v0.242.
BEZEL_TEST(codegen_cg_513, shift_left_count_0_is_identity_spec_6_5) {
    BEZEL_EXPECT_EQ(run("var n := 1; return n << 0;"), 1);
}

// §6.5: normal shift `1 << 3` is 8 — IN DOMAIN, unchanged by v0.242.
BEZEL_TEST(codegen_cg_513, shift_left_normal_count_3_spec_6_5) {
    BEZEL_EXPECT_EQ(run("var n := 1; return n << 3;"), 8);
}

// §6.5 (v0.242): a RUNTIME count of 32 raises OverflowException (§31.4) — the
// non-constant arm of the same rule. WAS: `n << cnt` == 1 (masked to 0).
BEZEL_TEST(codegen_cg_513, shift_left_runtime_count_32_raises_spec_6_5) {
    BEZEL_EXPECT_EQ(
        throwsOverflow("var n := 1; var cnt := 32; var r := n << cnt;"), 1);
}

// §6.5 (v0.242): right shift, constant count 32. WAS: `8 >> 32` == 8.
BEZEL_TEST(codegen_cg_513, shift_right_const_count_32_rejected_spec_6_5) {
    BEZEL_EXPECT_TRUE(rejectsConstCount("var n := 8; return n >> 32;"));
}

// §6.5 (v0.242): right shift, constant count 33. WAS: `8 >> 33` == 4.
BEZEL_TEST(codegen_cg_513, shift_right_const_count_33_rejected_spec_6_5) {
    BEZEL_EXPECT_TRUE(rejectsConstCount("var n := 8; return n >> 33;"));
}

// §6.5 / §6.2: signed right-shift fills with sign bit.
// -8 >> 1 == -4 (arithmetic right shift). -4 does not fit an 8-bit exit code,
// so this builds the full program directly and observes stdout (run()'s
// body-only wrapper can't route through runAotOut).
BEZEL_TEST(codegen_cg_513, signed_shift_right_fills_sign_bit_spec_6_5) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var n := -8; Console.WriteLine(n >> 1); return 0; "
        " end Main; end C;"), "-4");
}

// §6.5 (v0.242): a `long` left operand's domain is `0..63`, so a constant 64 is
// outside it — a compile error. WAS: `1L << 64` == 1 (64 % 64 == 0).
BEZEL_TEST(codegen_cg_513, long_shift_left_const_count_64_rejected_spec_6_5) {
    BEZEL_EXPECT_TRUE(
        rejectsConstCount("var n := long(1); return int(n << 64);"));
}

// §6.5 (v0.242): likewise 65. WAS: `2L << 65` == 4 (65 % 64 == 1).
BEZEL_TEST(codegen_cg_513, long_shift_left_const_count_65_rejected_spec_6_5) {
    BEZEL_EXPECT_TRUE(
        rejectsConstCount("var n := long(2); return int(n << 65);"));
}

// §6.5 (v0.242): the domain's upper edge for a 64-bit left operand is IN the
// domain — 63 shifts, 64 faults. Guards the boundary from an off-by-one.
BEZEL_TEST(codegen_cg_513, long_shift_left_count_63_in_domain_spec_6_5) {
    // 1L << 63 is long.MinValue; the sign test keeps it inside an exit code.
    BEZEL_EXPECT_EQ(run("var n := long(1); "
                        "return (if (n << 63) < 0L then 1 else 0);"), 1);
}

// ---------------------------------------------------------------------------
// #514 — §4.3.1: NaN reflexive equality
// ---------------------------------------------------------------------------

// §4.3.1 (L308): "NaN = NaN is true" — reflexive equality.
BEZEL_TEST(codegen_cg_514, nan_equals_nan_is_true_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (if nan = nan then 1 else 0);"), 1);
}

// §4.3.1: `nan <> nan` is false (negation of `=` which is true).
BEZEL_TEST(codegen_cg_514, nan_not_equal_nan_is_false_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (if nan <> nan then 1 else 0);"), 0);
}

// §4.3.1: `1.0 = nan` is false (only same-NaN is reflexively true).
BEZEL_TEST(codegen_cg_514, value_equals_nan_is_false_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (if 1.0 = nan then 1 else 0);"), 0);
}

// §4.3.1: `nan = 1.0` is false.
BEZEL_TEST(codegen_cg_514, nan_equals_value_is_false_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (if nan = 1.0 then 1 else 0);"), 0);
}

// §4.3.1: `1.0 = 1.0` is still true (non-NaN equality unaffected).
BEZEL_TEST(codegen_cg_514, normal_equality_unaffected_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("return (if 1.0 = 1.0 then 1 else 0);"), 1);
}

// §4.3.1: `1.0 <> 2.0` is true (normal inequality unaffected).
BEZEL_TEST(codegen_cg_514, normal_inequality_unaffected_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("return (if 1.0 <> 2.0 then 1 else 0);"), 1);
}

// §4.3.1: CONTROL — relational `nan < nan` remains false (IEEE).
BEZEL_TEST(codegen_cg_514, nan_less_than_nan_is_false_ieee_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (if nan < nan then 1 else 0);"), 0);
}

// §4.3.1: CONTROL — relational `nan <= nan` remains false (IEEE).
BEZEL_TEST(codegen_cg_514, nan_leq_nan_is_false_ieee_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (if nan <= nan then 1 else 0);"), 0);
}

// §4.3.1: CONTROL — relational `nan > nan` remains false (IEEE).
BEZEL_TEST(codegen_cg_514, nan_greater_than_nan_is_false_ieee_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (if nan > nan then 1 else 0);"), 0);
}

// §4.3.1: float variant — `nan = nan` is true for float (f32) too.
BEZEL_TEST(codegen_cg_514, float_nan_equals_nan_is_true_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := float(0.0 / 0.0); return (if nan = nan then 1 else 0);"), 1);
}

// §4.3.1: float variant — `nan <> nan` is false for float (f32).
BEZEL_TEST(codegen_cg_514, float_nan_not_equal_nan_is_false_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := float(0.0 / 0.0); return (if nan <> nan then 1 else 0);"), 0);
}

// ---------------------------------------------------------------------------
// #517 — §5.2: char(n) range guard
// ---------------------------------------------------------------------------

// §5.2 (L421): char(0xD800) — first surrogate — raises OverflowException.
BEZEL_TEST(codegen_cg_517, char_surrogate_low_raises_overflow_spec_5_2) {
    BEZEL_EXPECT_EQ(run(
        "try var c := char(0xD800); return int(c); "
        "catch OverflowException => return 99; end;"), 99);
}

// §5.2: char(0xDFFF) — last surrogate — raises OverflowException.
BEZEL_TEST(codegen_cg_517, char_surrogate_high_raises_overflow_spec_5_2) {
    BEZEL_EXPECT_EQ(run(
        "try var c := char(0xDFFF); return int(c); "
        "catch OverflowException => return 98; end;"), 98);
}

// §5.2 (L2283): char(0x110000) — one above U+10FFFF — raises OverflowException.
BEZEL_TEST(codegen_cg_517, char_above_max_codepoint_raises_overflow_spec_5_2) {
    BEZEL_EXPECT_EQ(run(
        "try var c := char(0x110000); return int(c); "
        "catch OverflowException => return 97; end;"), 97);
}

// §5.2: char(-1) — negative — raises OverflowException.
BEZEL_TEST(codegen_cg_517, char_negative_raises_overflow_spec_5_2) {
    BEZEL_EXPECT_EQ(run(
        "var n := -1; "
        "try var c := char(n); return int(c); "
        "catch OverflowException => return 96; end;"), 96);
}

// §5.2: CONTROL — char(65) == 'A', no exception.
BEZEL_TEST(codegen_cg_517, char_valid_ascii_no_exception_spec_5_2) {
    BEZEL_EXPECT_EQ(run("var c := char(65); return int(c);"), 65);
}

// §5.2: CONTROL — char(0x10FFFF) is the maximum valid code point, no exception.
// 0x10FFFF = 1114111 does not fit an 8-bit exit code (this was already an OOR
// value even before this file's runJit->runAot migration — a pre-existing
// masked-exit-code gap, caught here) — observe on stdout.
BEZEL_TEST(codegen_cg_517, char_max_codepoint_ok_spec_5_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var c := char(0x10FFFF); Console.WriteLine(int(c)); return 0; "
        " end Main; end C;"), "1114111");
}

// §5.2: CONTROL — char(0) is the null character U+0000, valid.
BEZEL_TEST(codegen_cg_517, char_zero_ok_spec_5_2) {
    BEZEL_EXPECT_EQ(run("var c := char(0); return int(c);"), 0);
}

// §5.2: char(0xD7FF) — one below the surrogate range — is valid. 0xD7FF =
// 55295 does not fit an 8-bit exit code (pre-existing masked-exit-code gap,
// caught here) — observe on stdout.
BEZEL_TEST(codegen_cg_517, char_just_below_surrogate_ok_spec_5_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var c := char(0xD7FF); Console.WriteLine(int(c)); return 0; "
        " end Main; end C;"), "55295");
}

// §5.2: char(0xE000) — one above the surrogate range — is valid. 0xE000 =
// 57344 does not fit an 8-bit exit code — observe on stdout.
BEZEL_TEST(codegen_cg_517, char_just_above_surrogate_ok_spec_5_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var c := char(0xE000); Console.WriteLine(int(c)); return 0; "
        " end Main; end C;"), "57344");
}

// ---------------------------------------------------------------------------
// #1094 — §6.5: the count's DOMAIN is bounded by the LEFT operand's bit width,
// and the result keeps the left operand's (narrow) type. Before #1094 a narrow
// left operand with a WIDER count (`byte << int`) widened the left to the
// count's width and used the WRONG width; #1094 fixed the width, and v0.242
// (#3045) turned the resulting `count mod width` into a FAULT. §6.5 is explicit
// that no promotion intervenes: "The right operand supplies only the shift
// count: it does **not** participate in the mixed-type promotion of §6.2, so a
// narrower left operand is *not* widened to match a wider count (`byte << int`
// yields `byte`, not `int`)" — so `byte << 8` is out of `0..7`, even though 8 is
// well inside `int`'s domain and the count itself is an `int`.
// ---------------------------------------------------------------------------

// §6.5 (v0.242): byte's domain is `0..7`; a RUNTIME count of 8 raises
// OverflowException. WAS: `byte(1) << int(8)` == 1 (8 & 7 == 0).
BEZEL_TEST(codegen_cg_1094, byte_shl_int_count_8_raises_byte_domain) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var a: byte := 1; var n: int := 8; var c: byte := a << n;"), 1);
}

// §6.5 (v0.242): same at 9. WAS: `byte(1) << int(9)` == 2 (9 & 7 == 1).
BEZEL_TEST(codegen_cg_1094, byte_shl_int_count_9_raises_byte_domain) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var a: byte := 1; var n: int := 9; var c: byte := a << n;"), 1);
}

// §6.5: the upper edge of byte's domain, 7, still shifts — 1 << 7 == 128.
BEZEL_TEST(codegen_cg_1094, byte_shl_int_count_7_in_byte_domain) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte := 1; var n: int := 7; var c: byte := a << n; return int(c);"), 128);
}

// §6.5: byte(200) << int(1): 400's bits beyond byte's width are discarded
// silently → 144. IN DOMAIN, and the discard half of §6.5 is unchanged by
// v0.242 ("Within the domain, bits shifted beyond the left operand's width are
// discarded silently — that is the operation … and the result never faults").
BEZEL_TEST(codegen_cg_1094, byte_shl_discards_bits_beyond_byte_width) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte := 200; var n: int := 1; var c: byte := a << n; return int(c);"), 144);
}

// §6.5 (v0.242): short's domain is `0..15`; a runtime count of 17 raises.
// WAS: `short(1) << int(17)` == 2 (17 & 15 == 1).
BEZEL_TEST(codegen_cg_1094, short_shl_int_count_17_raises_short_domain) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var a: short := 1; var n: int := 17; var c: short := a << n;"), 1);
}

// §6.5 (v0.242): ushort's domain is `0..15` too; a runtime count of 17 raises.
// WAS: `ushort(0x8000) >> int(17)` == 16384 (17 & 15 == 1, logical >> 1).
BEZEL_TEST(codegen_cg_1094, ushort_shr_int_count_17_raises_ushort_domain) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var a: ushort := 32768; var n: int := 17; var c: ushort := a >> n;"), 1);
}

// §6.5: the ushort logical-fill behaviour the cell above used to carry, kept on
// an IN-DOMAIN count: `ushort(0x8000) >> 1` == 0x4000 (16384), logical not
// arithmetic. 16384 does not fit an 8-bit exit code — observe on stdout.
BEZEL_TEST(codegen_cg_1094, ushort_shr_logical_in_domain) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var a: ushort := 32768; var n: int := 1; var c: ushort := a >> n; "
        "Console.WriteLine(int(c)); return 0; "
        " end Main; end C;"),
        "16384");
}

// sbyte(-128) >> int(1): arithmetic (signed) shift → -64. -64 does not fit an
// 8-bit exit code — observe on stdout.
BEZEL_TEST(codegen_cg_1094, sbyte_shr_arithmetic) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var a: sbyte := -128; var n: int := 1; var c: sbyte := a >> n; "
        "Console.WriteLine(int(c)); return 0; "
        " end Main; end C;"), "-64");
}

// byte(128) >> int(1): logical (unsigned) shift → 64 (not -64).
BEZEL_TEST(codegen_cg_1094, byte_shr_logical) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte := 128; var n: int := 1; var c: byte := a >> n; return int(c);"), 64);
}

// §6.5 (v0.242): int's own domain is `0..31`, so a same-width runtime count of
// 32 raises too — the narrow-operand fix did not change the int case, and
// neither does the domain rule. WAS: `int(1) << int(32)` == 1 (32 & 31 == 0).
BEZEL_TEST(codegen_cg_1094, int_shl_same_width_count_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var a: int := 1; var n: int := 32; var r := a << n;"), 1);
}

// --- nullable-lifted shift: same operand-width domain + correct signedness ---

// §6.5/§7.11 (v0.242): a lifted `byte?` shift takes the UNDERLYING `byte`'s
// `0..7` domain, so a runtime count of 9 raises on the both-present branch.
// WAS: `byte?(1) << int(9)` == 2 (9 & 7 == 1).
BEZEL_TEST(codegen_cg_1094, nullable_byte_shl_int_count_9_raises_byte_domain) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var a: byte? := 1; var n: int := 9; var c: byte? := a << n;"), 1);
}

// byte?(128) >> int(1): logical >> → 64 (a signed AShr regressed it to -64 once
// the left payload was narrowed to i8). IN DOMAIN — unchanged by v0.242.
BEZEL_TEST(codegen_cg_1094, nullable_byte_shr_logical) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte? := 128; var n: int := 1; var c: byte? := a >> n; return int(c!);"), 64);
}

// byte?(null) << int: null propagates (no shift performed).
// §7.11 (v0.286, #3298) — REJECTION. This row asserted the lift's
// null-propagation ("`a << n` is null when `a` is"); §7.11 now opens "No
// operator **lifts** to a nullable type" and an absent operand "is diagnosed at
// compile time". `a` is initialised with `null`, so §7.4 narrows nothing.
// The NARROWED spelling still shifts and still obeys §6.5's count domain —
// that is `shift_count_domain_3045.nullable_lifted_uses_underlying_domain`.
BEZEL_TEST(codegen_cg_1094, nullable_byte_shl_null_rejected_3298) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: byte? := null; var n: int := 1; var c: byte? := a << n; "
        "  return (if c = null then 0 else 99); end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawRule = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is not defined on a nullable value type") !=
                std::string::npos &&
            d.message.find("7.11") != std::string::npos)
            sawRule = true;
    BEZEL_EXPECT_TRUE(sawRule);
}

// The non-nullable neighbour keeps #1094's shift lowering covered.
BEZEL_TEST(codegen_cg_1094, byte_shl_nonnullable_control_3298) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte := 5; var n: int := 1; var c: byte := byte(a << n); "
        "return int(c);"), 10);
}
