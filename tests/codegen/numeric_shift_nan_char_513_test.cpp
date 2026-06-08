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
