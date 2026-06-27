// Tests for codegen fixes: #513 shift-count modulo, #514 NaN reflexive equality,
// #517 char(n) range guard. All derived from the Bezel spec.
//
// #513: §6.5 "The shift count is reduced modulo the operand's bit width."
//   LLVM shl/lshr/ashr with count >= bitwidth is poison; the fix masks the
//   count operand before the shift (& 31 for int, & 63 for long).
//
// #514: §4.3.1 (L308/L314): "NaN = NaN is true" (deliberate, for Set<double>
//   and Dictionary key semantics). IEEE <,<=,>,>= comparisons are unchanged
//   (NaN comparisons remain false per IEEE 754).
//
// #517: §5.2 (L421): "Casting an integer to char requires the integer to fall
//   in U+0000–U+10FFFF excluding the surrogate range; otherwise
//   OverflowException." §5.2 (L2283): "char(0xD800) raises OverflowException."

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ---------------------------------------------------------------------------
// #513 — §6.5: shift count modulo bit-width
// ---------------------------------------------------------------------------

// §6.5 spec example: `1 << 32` is 1 (32 % 32 == 0, so shift by 0).
BEZEL_TEST(codegen_cg_513, shift_left_count_32_wraps_to_0_spec_6_5) {
    // 1 << (32 % 32) == 1 << 0 == 1
    BEZEL_EXPECT_EQ(run("var n := 1; return n << 32;"), 1);
}

// §6.5: `1 << 33` is 2 (33 % 32 == 1, so shift by 1).
BEZEL_TEST(codegen_cg_513, shift_left_count_33_wraps_to_1_spec_6_5) {
    // 1 << (33 % 32) == 1 << 1 == 2
    BEZEL_EXPECT_EQ(run("var n := 1; return n << 33;"), 2);
}

// §6.5: `1 << 0` is 1 (normal case, no wrapping).
BEZEL_TEST(codegen_cg_513, shift_left_count_0_is_identity_spec_6_5) {
    BEZEL_EXPECT_EQ(run("var n := 1; return n << 0;"), 1);
}

// §6.5: normal shift `1 << 3` is 8 — regression guard.
BEZEL_TEST(codegen_cg_513, shift_left_normal_count_3_spec_6_5) {
    BEZEL_EXPECT_EQ(run("var n := 1; return n << 3;"), 8);
}

// §6.5: variable shift count cnt=32 → 1<<(32%32)==1.
BEZEL_TEST(codegen_cg_513, shift_left_variable_count_32_wraps_spec_6_5) {
    BEZEL_EXPECT_EQ(run("var n := 1; var cnt := 32; return n << cnt;"), 1);
}

// §6.5: right-shift count 32 on int wraps to 0, so `8 >> 32` == 8.
BEZEL_TEST(codegen_cg_513, shift_right_count_32_wraps_to_0_spec_6_5) {
    // 8 >> (32 % 32) == 8 >> 0 == 8
    BEZEL_EXPECT_EQ(run("var n := 8; return n >> 32;"), 8);
}

// §6.5: right-shift count 33 → 33 % 32 == 1, so `8 >> 33` == 4.
BEZEL_TEST(codegen_cg_513, shift_right_count_33_wraps_to_1_spec_6_5) {
    // 8 >> (33 % 32) == 8 >> 1 == 4
    BEZEL_EXPECT_EQ(run("var n := 8; return n >> 33;"), 4);
}

// §6.5 / §6.2: signed right-shift fills with sign bit.
// -8 >> 1 == -4 (arithmetic right shift).
BEZEL_TEST(codegen_cg_513, signed_shift_right_fills_sign_bit_spec_6_5) {
    BEZEL_EXPECT_EQ(run("var n := -8; return n >> 1;"), -4);
}

// §6.5: long shift — `1L << 64` wraps: 64 % 64 == 0, result is 1L (returned
// as int, fitting in i32).
BEZEL_TEST(codegen_cg_513, long_shift_left_count_64_wraps_to_0_spec_6_5) {
    // 1L << (64 % 64) == 1L << 0 == 1
    BEZEL_EXPECT_EQ(run("var n := long(1); return int(n << 64);"), 1);
}

// §6.5: long shift `2L << 65` → 65 % 64 == 1, result 4L (as int).
BEZEL_TEST(codegen_cg_513, long_shift_left_count_65_wraps_to_1_spec_6_5) {
    BEZEL_EXPECT_EQ(run("var n := long(2); return int(n << 65);"), 4);
}

// ---------------------------------------------------------------------------
// #514 — §4.3.1: NaN reflexive equality
// ---------------------------------------------------------------------------

// §4.3.1 (L308): "NaN = NaN is true" — reflexive equality.
BEZEL_TEST(codegen_cg_514, nan_equals_nan_is_true_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (1 if nan = nan else 0);"), 1);
}

// §4.3.1: `nan <> nan` is false (negation of `=` which is true).
BEZEL_TEST(codegen_cg_514, nan_not_equal_nan_is_false_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (1 if nan <> nan else 0);"), 0);
}

// §4.3.1: `1.0 = nan` is false (only same-NaN is reflexively true).
BEZEL_TEST(codegen_cg_514, value_equals_nan_is_false_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (1 if 1.0 = nan else 0);"), 0);
}

// §4.3.1: `nan = 1.0` is false.
BEZEL_TEST(codegen_cg_514, nan_equals_value_is_false_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (1 if nan = 1.0 else 0);"), 0);
}

// §4.3.1: `1.0 = 1.0` is still true (non-NaN equality unaffected).
BEZEL_TEST(codegen_cg_514, normal_equality_unaffected_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("return (1 if 1.0 = 1.0 else 0);"), 1);
}

// §4.3.1: `1.0 <> 2.0` is true (normal inequality unaffected).
BEZEL_TEST(codegen_cg_514, normal_inequality_unaffected_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("return (1 if 1.0 <> 2.0 else 0);"), 1);
}

// §4.3.1: CONTROL — relational `nan < nan` remains false (IEEE).
BEZEL_TEST(codegen_cg_514, nan_less_than_nan_is_false_ieee_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (1 if nan < nan else 0);"), 0);
}

// §4.3.1: CONTROL — relational `nan <= nan` remains false (IEEE).
BEZEL_TEST(codegen_cg_514, nan_leq_nan_is_false_ieee_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (1 if nan <= nan else 0);"), 0);
}

// §4.3.1: CONTROL — relational `nan > nan` remains false (IEEE).
BEZEL_TEST(codegen_cg_514, nan_greater_than_nan_is_false_ieee_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := 0.0 / 0.0; return (1 if nan > nan else 0);"), 0);
}

// §4.3.1: float variant — `nan = nan` is true for float (f32) too.
BEZEL_TEST(codegen_cg_514, float_nan_equals_nan_is_true_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := float(0.0 / 0.0); return (1 if nan = nan else 0);"), 1);
}

// §4.3.1: float variant — `nan <> nan` is false for float (f32).
BEZEL_TEST(codegen_cg_514, float_nan_not_equal_nan_is_false_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run("var nan := float(0.0 / 0.0); return (1 if nan <> nan else 0);"), 0);
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
BEZEL_TEST(codegen_cg_517, char_max_codepoint_ok_spec_5_2) {
    BEZEL_EXPECT_EQ(run("var c := char(0x10FFFF); return int(c);"), 0x10FFFF);
}

// §5.2: CONTROL — char(0) is the null character U+0000, valid.
BEZEL_TEST(codegen_cg_517, char_zero_ok_spec_5_2) {
    BEZEL_EXPECT_EQ(run("var c := char(0); return int(c);"), 0);
}

// §5.2: char(0xD7FF) — one below the surrogate range — is valid.
BEZEL_TEST(codegen_cg_517, char_just_below_surrogate_ok_spec_5_2) {
    BEZEL_EXPECT_EQ(run("var c := char(0xD7FF); return int(c);"), 0xD7FF);
}

// §5.2: char(0xE000) — one above the surrogate range — is valid.
BEZEL_TEST(codegen_cg_517, char_just_above_surrogate_ok_spec_5_2) {
    BEZEL_EXPECT_EQ(run("var c := char(0xE000); return int(c);"), 0xE000);
}

// ---------------------------------------------------------------------------
// #1094 — §6.5: a shift's count is masked by the LEFT operand's bit width, and
// the result keeps the left operand's (narrow) type. Before the fix, a narrow
// left operand with a WIDER count (`byte << int`) widened the left to the
// count's width, masked the count by the WRONG width (&31 instead of &7), and
// truncated back — `byte(1) << int(8)` gave 0 instead of 1. (`byte << byte` and
// `int << int`, both same width, already worked.)
// ---------------------------------------------------------------------------

// byte(1) << int(8): 8 & 7 == 0 → 1 << 0 == 1 (was 0).
BEZEL_TEST(codegen_cg_1094, byte_shl_int_count_8_masks_by_byte_width) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte := 1; var n: int := 8; var c: byte := a << n; return int(c);"), 1);
}

// byte(1) << int(9): 9 & 7 == 1 → 1 << 1 == 2 (was 0).
BEZEL_TEST(codegen_cg_1094, byte_shl_int_count_9_masks_by_byte_width) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte := 1; var n: int := 9; var c: byte := a << n; return int(c);"), 2);
}

// byte(200) << int(1): 400 wraps at byte width → 144.
BEZEL_TEST(codegen_cg_1094, byte_shl_wraps_at_byte_width) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte := 200; var n: int := 1; var c: byte := a << n; return int(c);"), 144);
}

// short(1) << int(17): 17 & 15 == 1 → 2.
BEZEL_TEST(codegen_cg_1094, short_shl_int_count_masks_by_short_width) {
    BEZEL_EXPECT_EQ(run(
        "var a: short := 1; var n: int := 17; var c: short := a << n; return int(c);"), 2);
}

// ushort(0x8000) >> int(17): 17 & 15 == 1 → logical >> 1 == 0x4000 (16384).
BEZEL_TEST(codegen_cg_1094, ushort_shr_logical_masks_by_ushort_width) {
    BEZEL_EXPECT_EQ(run(
        "var a: ushort := 32768; var n: int := 17; var c: ushort := a >> n; return int(c);"),
        16384);
}

// sbyte(-128) >> int(1): arithmetic (signed) shift → -64.
BEZEL_TEST(codegen_cg_1094, sbyte_shr_arithmetic) {
    BEZEL_EXPECT_EQ(run(
        "var a: sbyte := -128; var n: int := 1; var c: sbyte := a >> n; return int(c);"), -64);
}

// byte(128) >> int(1): logical (unsigned) shift → 64 (not -64).
BEZEL_TEST(codegen_cg_1094, byte_shr_logical) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte := 128; var n: int := 1; var c: byte := a >> n; return int(c);"), 64);
}

// Regression: int(1) << int(32) still masks by 32 (== 0) → 1; int << int unchanged.
BEZEL_TEST(codegen_cg_1094, int_shl_same_width_regression) {
    BEZEL_EXPECT_EQ(run(
        "var a: int := 1; var n: int := 32; return a << n;"), 1);
}

// --- nullable-lifted shift: same operand-width masking + correct signedness ---

// byte?(1) << int(9): 9 & 7 == 1 → 2 (was a compiler crash / wrong value).
BEZEL_TEST(codegen_cg_1094, nullable_byte_shl_int_count_masks_by_byte_width) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte? := 1; var n: int := 9; var c: byte? := a << n; return int(c!);"), 2);
}

// byte?(128) >> int(1): logical >> → 64 (a signed AShr regressed it to -64 once
// the left payload was narrowed to i8).
BEZEL_TEST(codegen_cg_1094, nullable_byte_shr_logical) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte? := 128; var n: int := 1; var c: byte? := a >> n; return int(c!);"), 64);
}

// byte?(null) << int: null propagates (no shift performed).
BEZEL_TEST(codegen_cg_1094, nullable_byte_shl_null_propagates) {
    BEZEL_EXPECT_EQ(run(
        "var a: byte? := null; var n: int := 1; var c: byte? := a << n; "
        "return (0 if c = null else 99);"), 0);
}
