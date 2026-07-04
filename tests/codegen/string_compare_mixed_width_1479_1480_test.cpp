// CG27b-ext: string comparison operators across mixed storage widths (§4.6 §9.6).
// Fixes #1479 (emitStringEq wrong for mixed-width operands) and
// #1480 (emitStringOrdCmp wrong for mixed-width operands).
//
// §4.6: the internal storage width (1/2/4 bytes per code point) is an
// implementation detail; programs observe only the char sequence and its
// length. Two strings must compare equal iff their code-point sequences are
// identical, regardless of how each is stored. Ordinal comparison (<, <=, >,
// >=) must be by code-point value, not by raw bytes.
//
// The motivating case: `s[range]` preserves the source string's width (§4.6).
// Slicing "helloĀ" (Ā=U+0100 forces width=2) gives a width=2 substring; the
// literal "hello" is width=1. The old memcmp-based fast path read byteSz=10
// for the width=2 side but only 5 valid bytes existed in the width=1 side,
// so byte 1 of each code point differed (0x00 vs next ASCII char), producing
// a false inequality. The fix dispatches to a per-code-point loop when widths
// differ.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// ─── Equality / inequality — #1479 ──────────────────────────────────────────

// §4.6 #1479: width=2 slice of "helloĀ" [0..4] == width=1 literal "hello"
// (identical code-point sequences: h,e,l,l,o).
BEZEL_TEST(codegen_cg27b_mixed_width, eq_w2_slice_vs_w1_lit_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"helloĀ\"; var sliced := src[0..4]; var lit := \"hello\"; "
        "  return (1 if sliced = lit else 0); "
        "end Main; end C;"), 1);
}

// §4.6 #1479: inequality must also be false for the same equal pair.
BEZEL_TEST(codegen_cg27b_mixed_width, neq_w2_slice_vs_w1_lit_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"helloĀ\"; var sliced := src[0..4]; var lit := \"hello\"; "
        "  return (1 if sliced <> lit else 0); "
        "end Main; end C;"), 0);
}

// §4.6: same test symmetrically — width=1 literal on left, width=2 slice on right.
BEZEL_TEST(codegen_cg27b_mixed_width, eq_w1_lit_vs_w2_slice_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"helloĀ\"; var sliced := src[0..4]; var lit := \"hello\"; "
        "  return (1 if lit = sliced else 0); "
        "end Main; end C;"), 1);
}

// §4.6: non-equal pair across widths must compare unequal.
// "abc" (width=1) vs slice of "abcĀ"[0..3] = "abc" (width=2) → equal (true).
BEZEL_TEST(codegen_cg27b_mixed_width, eq_w2_slice_abc_vs_w1_abc_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"abcĀ\"; var sliced := src[0..2]; var lit := \"abc\"; "
        "  return (1 if sliced = lit else 0); "
        "end Main; end C;"), 1);
}

// §4.6: different content across widths must compare unequal.
// "abd" (width=1) vs slice of "abcĀ"[0..2]="abc" (width=2) → not equal.
BEZEL_TEST(codegen_cg27b_mixed_width, eq_w2_slice_abc_vs_w1_abd_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"abcĀ\"; var sliced := src[0..2]; var lit := \"abd\"; "
        "  return (1 if sliced = lit else 0); "
        "end Main; end C;"), 0);
}

// §4.6: width=1 vs width=4 (U+10000 forces width=4).
// "abc" (w=1) == slice of "abc𐀀"[0..2] = "abc" (w=4).
BEZEL_TEST(codegen_cg27b_mixed_width, eq_w4_slice_vs_w1_lit_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"abc\U00010000\"; var sliced := src[0..2]; var lit := \"abc\"; "
        "  return (1 if sliced = lit else 0); "
        "end Main; end C;"), 1);
}

// §4.6: width=2 vs width=4.
// "Ābc" (w=2) == slice of "Ābc𐀀"[0..2] = "Ābc" (w=4).
BEZEL_TEST(codegen_cg27b_mixed_width, eq_w4_slice_vs_w2_lit_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"Ābc\U00010000\"; var sliced := src[0..2]; var lit := \"Ābc\"; "
        "  return (1 if sliced = lit else 0); "
        "end Main; end C;"), 1);
}

// §4.6: same-width (w=1 vs w=1) equality still works after the refactor.
BEZEL_TEST(codegen_cg27b_mixed_width, eq_w1_same_width_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"hello\"; var b := \"hello\"; "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 1);
}

// §4.6: same-width not-equal still works.
BEZEL_TEST(codegen_cg27b_mixed_width, eq_w1_same_width_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"hello\"; var b := \"world\"; "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 0);
}

// ─── Ordinal comparison — #1480 ─────────────────────────────────────────────

// §4.6 §9.6 #1480: "ac" (width=2, from "Āac"[1..2]) > "ab" (width=1).
// Code points: 'a'='a', 'c'>'b' → "ac" > "ab", so "ac" < "ab" is FALSE.
BEZEL_TEST(codegen_cg27b_mixed_width, ord_lt_w2_ac_vs_w1_ab_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var w2 := \"Āac\"; var sliced := w2[1..2]; var lit := \"ab\"; "
        "  return (1 if sliced < lit else 0); "
        "end Main; end C;"), 0);
}

// §4.6 §9.6 #1480: "ac" > "ab" must be TRUE (symmetric check).
BEZEL_TEST(codegen_cg27b_mixed_width, ord_gt_w2_ac_vs_w1_ab_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var w2 := \"Āac\"; var sliced := w2[1..2]; var lit := \"ab\"; "
        "  return (1 if sliced > lit else 0); "
        "end Main; end C;"), 1);
}

// §4.6: "aa" < "ab" (width=2 slice vs width=1 literal) must be TRUE.
// Code points: 'a'='a', 'a'<'b' → "aa" < "ab".
BEZEL_TEST(codegen_cg27b_mixed_width, ord_lt_w2_aa_vs_w1_ab_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var w2 := \"Āaa\"; var sliced := w2[1..2]; var lit := \"ab\"; "
        "  return (1 if sliced < lit else 0); "
        "end Main; end C;"), 1);
}

// §4.6: equal strings across widths → <= is TRUE, >= is TRUE.
BEZEL_TEST(codegen_cg27b_mixed_width, ord_le_w2_abc_vs_w1_abc_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"abcĀ\"; var sliced := src[0..2]; var lit := \"abc\"; "
        "  return (1 if sliced <= lit else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg27b_mixed_width, ord_ge_w2_abc_vs_w1_abc_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"abcĀ\"; var sliced := src[0..2]; var lit := \"abc\"; "
        "  return (1 if sliced >= lit else 0); "
        "end Main; end C;"), 1);
}

// §4.6: shorter string < longer with same prefix, mixed widths.
// "ab" (w=2, slice of "Āab") < "abc" (w=1) because lengths differ on tie.
BEZEL_TEST(codegen_cg27b_mixed_width, ord_lt_w2_shorter_prefix) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"Āab\"; var sliced := src[1..2]; var lit := \"abc\"; "
        "  return (1 if sliced < lit else 0); "
        "end Main; end C;"), 1);
}

// §4.6: width=1 vs width=4 ordinal — 'z'(0x7A) < U+10000(0x10000).
// Slice "z𐀀" (w=4)[0..0] = "z" (w=4) < "€𐀀"… actually simpler:
// "a" (w=1) < "𐀀" (w=4, U+10000 > 'a'=0x61).
BEZEL_TEST(codegen_cg27b_mixed_width, ord_lt_w1_a_vs_w4_supplementary_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src := \"\U00010000\"; var sliced := src[0..0]; var lit := \"z\"; "
        "  return (1 if lit < sliced else 0); "
        "end Main; end C;"), 1);
}

// §4.6: same-width (w=1) ordinal < still works after the refactor.
BEZEL_TEST(codegen_cg27b_mixed_width, ord_lt_w1_same_width_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"abc\"; var b := \"abd\"; "
        "  return (1 if a < b else 0); "
        "end Main; end C;"), 1);
}

// §4.6: same-width (w=2) ordinal works too.
BEZEL_TEST(codegen_cg27b_mixed_width, ord_lt_w2_same_width_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"abcĀ\"; var b := \"abdĀ\"; "
        "  return (1 if a < b else 0); "
        "end Main; end C;"), 1);
}

}  // namespace
