// Codegen — fix #4483 (stdlib §9.6, v0.306): `string.Equals(other,
// ignoreCase: true)` lowered to an inlined "case-fold each side into a fresh
// buffer, then compare the buffers" loop that carried TWO independent defects
// at one site (LlvmBackend_String.cpp, the `eq.ic` arm).
//
// Defect 1 — the UB. The loop called <ctype.h>'s `tolower()` on the result of
// `loadCodePoint`, the WIDTH-AWARE decoder (§4.6), so any code point above
// U+00FF was passed outside tolower's domain — undefined per C11 §7.4p1. This
// was the last instance of the class #4420 removed from char.* and
// string.IsNullOrWhiteSpace. It is NOT directly observable: ASan cannot see it
// (glibc's ctype tables are uninstrumented libc), and glibc's out-of-line
// `tolower` happens to range-check and hand the argument back — measured on
// this box before the fix: tolower(0x161) = 0x161, tolower(0x65E5) = 0x65E5.
// Benign HERE, on THIS libc, still undefined; nothing below asserts on it
// directly, because nothing honestly can.
//
// Defect 2 — the truncation, which is what made the wrong ANSWER observable.
// The folded code point was `Trunc`'d to i8 and stored at a hardcoded stride
// of 1 byte, while the source index advanced one CODE POINT at a time, and the
// result was labelled width=1. So every code point above U+00FF was silently
// replaced by its low byte, and strings that differ compared EQUAL. Measured
// on HEAD (65f92259) before the fix, by probe:
//
//     "š".Equals("a", true)   -> true     (U+0161's low byte is 0x61 = 'a')
//     "š".Equals("ɡ", true)   -> true     (U+0161 and U+0261 share low byte 0x61)
//     "á".Equals("ǡ", true)   -> true     (U+00E1 and U+01E1 share low byte 0xE1)
//     "😀".Equals("Ā", true)  -> true     (U+1F600 and U+0100 share low byte 0x00)
//
// All four must be false. They are the rows that FAIL pre-fix; the ASCII and
// non-ASCII control rows answer identically on both sides and are here to pin
// that the fix changed nothing it should not have.
//
// The fix routes the member through the runtime helper
// `bz_str_equals_ignorecase` (bezel_runtime_strings.cpp), which folds each
// operand per code point via `bz_char_to_lower` — the SAME shared helper
// #4420 introduced, so the runtime holds one case policy rather than a second
// copy here (#2590) — and decides the comparison position by position, so no
// buffer is materialised at all.
//
// Spec basis: stdlib §9.6 (v0.306), "All comparison and equality methods in
// this version are ordinal (code-point-BASED), including the `ignoreCase`
// overloads — case-insensitive comparison compares each code point's simple
// Unicode case-folded form, with no locale or culture-specific tailoring", and
// §4.6 (a string is a sequence of Unicode scalar values, stored at a
// per-string width of 1/2/4 bytes that is invisible to the language).
//
// Above-ASCII folding: within ASCII, `bz_char_to_lower` IS the simple case
// fold §9.6 asks for; above ASCII it is identity, which is #4420's
// domain-safe answer rather than a widened category table. Two code points
// above ASCII therefore compare equal iff they are the SAME code point — which
// is exactly what the four truncation rows assert. The row
// `above_ascii_fold_is_identity_4481_boundary` records that boundary
// deliberately: full simple case FOLDING above ASCII needs Unicode Character
// Database data this runtime does not embed, and #4481 owns that (no-UB,
// wrong-value-only) range. When #4481 lands, THAT row is the one that changes.
//
// Incidental, reported not asserted: deleting the buffer also removed two
// undisposed allocations per operand. Measured on a single
// `"Hello".Equals("HELLO", true)` program: 76 bytes in 4 allocations pre-fix,
// none post-fix; the `ignoreCase := false` arm leaked nothing on either side.
// No row below asserts a leak count — the disposal gaps in this area belong to
// #4482, and coupling a value oracle to a leak count would make this test fail
// for a reason it does not diagnose.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
// All returns below are small flag sums, well under runAot's 8-bit
// WEXITSTATUS mask.
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// The rows that FAIL pre-fix: distinct strings the one-byte truncation
// collapsed onto the same folded buffer. Each pair differs only ABOVE U+00FF
// (or across the 0xFF boundary) but shares a low byte, so pre-fix each
// returned `true`; the correct ordinal answer (§9.6) is `false`.
// ---------------------------------------------------------------------------

// U+0161 (š) truncates to 0x61, which is ASCII 'a'. The two strings share no
// code point, so they are not equal under any case fold.
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, truncation_wide_vs_ascii_low_byte) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if \"š\".Equals(\"a\", true) then 1 else 0); "
        "end Main; end C;"), 0);
}

// U+0161 (š) and U+0261 (ɡ) share low byte 0x61 — both above U+00FF, so this
// row isolates the truncation from any ASCII interaction.
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, truncation_two_wide_sharing_low_byte) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if \"š\".Equals(\"ɡ\", true) then 1 else 0); "
        "end Main; end C;"), 0);
}

// U+00E1 (á, width 1) and U+01E1 (ǡ, width 2) share low byte 0xE1 — the
// truncation straddles the storage-width boundary, so the operands' widths
// differ as well (§4.6).
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, truncation_across_width_boundary) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if \"á\".Equals(\"ǡ\", true) then 1 else 0); "
        "end Main; end C;"), 0);
}

// U+1F600 (😀, width 4) and U+0100 (Ā, width 2) share low byte 0x00 — the
// widest storage width, and the folded byte is NUL, which a length-blind
// compare would also have read as two empty strings.
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, truncation_astral_vs_bmp_low_byte) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if \"\U0001f600\".Equals(\"Ā\", true) then 1 else 0); "
        "end Main; end C;"), 0);
}

// All four truncation collisions in one program, so a partial fix that repairs
// one width but not another is visible as a specific value rather than a bare
// pass/fail. Each correctly-unequal pair contributes its bit; 15 = all correct,
// 0 = the pre-fix behaviour (every pair wrongly equal).
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, truncation_rows_together) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: bool := \"š\".Equals(\"a\", true); "
        "  var b: bool := \"š\".Equals(\"ɡ\", true); "
        "  var c: bool := \"á\".Equals(\"ǡ\", true); "
        "  var d: bool := \"\U0001f600\".Equals(\"Ā\", true); "
        "  return (if not a then 1 else 0) + (if not b then 2 else 0) + "
        "         (if not c then 4 else 0) + (if not d then 8 else 0); "
        "end Main; end C;"), 15);
}

// ---------------------------------------------------------------------------
// Non-ASCII equality: the member must still answer correctly for operands the
// truncation happened to handle by accident (both sides truncating alike).
// ---------------------------------------------------------------------------

// Equal non-ASCII strings compare equal. (Pre-fix this also answered `true`,
// because both operands truncated identically — which is precisely why the
// defect stayed invisible: the obvious probe passes.)
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, non_ascii_equal_strings_are_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if \"日本\".Equals(\"日本\", true) then 1 else 0); "
        "end Main; end C;"), 1);
}

// Differing non-ASCII strings compare unequal.
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, non_ascii_differing_strings_are_unequal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if \"日本\".Equals(\"日x\", true) then 1 else 0); "
        "end Main; end C;"), 0);
}

// A width-2 string whose ASCII tail differs only in case: the fold must still
// apply to the ASCII code points when the string's storage width is 2 (§4.6 —
// storage width is invisible to the language, so the answer may not depend on
// it).
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, ascii_folds_inside_a_wide_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if \"日Abc\".Equals(\"日ABC\", true) then 1 else 0); "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// ASCII control rows — byte-for-byte unchanged by the fix. Within ASCII the
// shared bz_char_to_lower IS the simple case fold §9.6 specifies, so every
// answer here is the pre-fix answer.
// ---------------------------------------------------------------------------

// A case-differing ASCII pair is equal under ignoreCase, and the SAME pair is
// unequal with ignoreCase := false (§9.6: the false arm is plain ordinal
// equality, and the fix must not have collapsed the two arms).
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, ascii_case_differing_pair) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var folded: bool := \"Hello\".Equals(\"HELLO\", true); "
        "  var ordinal: bool := \"Hello\".Equals(\"HELLO\", false); "
        "  return (if folded then 1 else 0) + (if ordinal then 2 else 0); "
        "end Main; end C;"), 1);
}

// ASCII strings that differ in a LETTER, not merely in case, stay unequal —
// folding must not collapse 'c' and 'd'.
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, ascii_differing_letters_are_unequal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if \"ABC\".Equals(\"abd\", true) then 1 else 0); "
        "end Main; end C;"), 0);
}

// Length and emptiness: unequal code-point counts are unequal whatever folds,
// and two empty strings are equal (§9.6 — lengths are code-point counts).
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, length_mismatch_and_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var shorter: bool := \"abc\".Equals(\"ab\", true); "
        "  var empties: bool := \"\".Equals(\"\", true); "
        "  return (if shorter then 1 else 0) + (if empties then 2 else 0); "
        "end Main; end C;"), 2);
}

// #1244 / §9.6: `other` is `string?`, and a null `other` against a non-null
// receiver is `false` — not a SIGSEGV. The fix replaced what that guard was
// guarding (a lowering that dereferenced both operands), so the guard's
// contract is re-pinned here.
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, null_other_is_false_not_a_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: string? := null; "
        "  return (if \"abc\".Equals(o, true) then 1 else 0); "
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// The documented boundary (#4481), recorded so the choice is not re-litigated
// silently. Above ASCII the shared fold is IDENTITY — #4420's domain-safe
// answer, deliberately not a widened category table — so U+03A3 (Σ) and
// U+03C3 (σ) are a case pair Unicode knows about and this runtime does not,
// and they compare UNEQUAL. This row answers identically pre- and post-fix; it
// is contract documentation, not a regression witness. It is also the row that
// must be UPDATED (to `1`) when #4481 lands real Unicode Character Database
// case folding — not deleted, and not worked around.
// ---------------------------------------------------------------------------
BEZEL_TEST(string_equals_ignorecase_codepoint_4483, above_ascii_fold_is_identity_4481_boundary) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (if \"Σ\".Equals(\"σ\", true) then 1 else 0); "
        "end Main; end C;"), 0);
}
