// #527 — char literal escape value wrong in codegen (§18.3 / §18.8).
//
// The bug: codegen re-parses the raw literal text in parseCharLiteral
// instead of using the lexer-decoded code point.  \uXXXX, \UXXXXXXXX,
// \xH..H, \a, and non-ASCII UTF-8 char literals all produced the wrong
// value before the fix.
//
// Tests verify:
//   - §18.8 \uXXXX (4-hex BMP)  — 'A' = U+0041 = 65
//   - §18.8 \UXXXXXXXX (8-hex full range) — emoji / non-BMP
//   - §18.8 \xH..H (1-4 hex, BMP)  — \x41 = 65, \xE9 = 233
//   - §18.8 \a (alert/bell = 7)
//   - raw non-ASCII UTF-8 in a char literal (é = 233, ĩ = 297, CJK)
//   - controls: plain ASCII ('A'=65), simple escapes (\n=10, \t=9, \x41=65)
//   - non-ASCII comparison: 'ĩ' > 'Ĩ' (297 > 296)
//   - char range over a small non-ASCII range yields the right count

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

} // namespace

// ── §18.8 \uXXXX : four-hex BMP escape ─────────────────────────────────────

// §18.8 example: 'A' is U+0041 = 65 ('A').
// Before the fix parseCharLiteral returned (unsigned char)'u' = 117.
BEZEL_TEST(codegen_char_escape_527, spec_18_8_u4hex_capital_A) {
    BEZEL_EXPECT_EQ(run("return int('\\u0041');"), 65);
}

// §18.8: 'é' = U+00E9 = 233 (é, Latin small letter e with acute).
BEZEL_TEST(codegen_char_escape_527, spec_18_8_u4hex_e_acute) {
    BEZEL_EXPECT_EQ(run("return int('\\u00E9');"), 233);
}

// §18.8: 'ĩ' = U+0129 = 297 (ĩ, Latin small letter i with tilde). 297 does not
// fit an 8-bit exit code, so observe it on stdout (full program built
// directly; run()'s body-only wrapper can't route through runAotOut).
BEZEL_TEST(codegen_char_escape_527, spec_18_8_u4hex_i_tilde) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "Console.WriteLine(int('\\u0129')); return 0; "
        " end Main; end C;"), "297");
}

// §18.8: '中' = U+4E2D = 20013 (中, CJK Unified Ideograph).
// Return value is the low byte via exit-code, so check divisibility instead.
BEZEL_TEST(codegen_char_escape_527, spec_18_8_u4hex_cjk_zhong) {
    // int('\\u4E2D') = 20013; 20013 mod 256 = 109. Exit codes are 0-255.
    BEZEL_EXPECT_EQ(run("return int('\\u4E2D') mod 256;"), 20013 % 256);
}

// ── §18.8 \UXXXXXXXX : eight-hex full-range escape ──────────────────────────

// §18.8 example from spec: '\U0001F600' = U+1F600 = 128512 (😀).
// 128512 mod 256 = 0.
BEZEL_TEST(codegen_char_escape_527, spec_18_8_U8hex_emoji_face) {
    // Verify non-BMP code point is decoded correctly: 128512 mod 256 = 0
    // and 128512 mod 257 = 128512 - 500*257 = 128512-128500 = 12.
    BEZEL_EXPECT_EQ(run("return int('\\U0001F600') mod 257;"), 128512 % 257);
}

// §18.8: '\U00010041' = U+10041 = 65601. 65601 mod 256 = 65.
BEZEL_TEST(codegen_char_escape_527, spec_18_8_U8hex_non_bmp) {
    BEZEL_EXPECT_EQ(run("return int('\\U00010041') mod 256;"), 65601 % 256);
}

// ── §18.8 \xH..H : hex escape, 1–4 digits, BMP only ────────────────────────

// §18.8 example: '\x41' = 0x41 = 65 ('A').
// Before the fix parseCharLiteral returned (unsigned char)'x' = 120.
BEZEL_TEST(codegen_char_escape_527, spec_18_8_xhex_single_byte_65) {
    BEZEL_EXPECT_EQ(run("return int('\\x41');"), 65);
}

// §18.8: '\xE9' = 0xE9 = 233 (é as a code point, same as é).
BEZEL_TEST(codegen_char_escape_527, spec_18_8_xhex_two_digit_233) {
    BEZEL_EXPECT_EQ(run("return int('\\xE9');"), 233);
}

// §18.8: '\x4E2D' = 0x4E2D = 20013 (same code point as 中 above).
BEZEL_TEST(codegen_char_escape_527, spec_18_8_xhex_four_digit_20013) {
    BEZEL_EXPECT_EQ(run("return int('\\x4E2D') mod 256;"), 20013 % 256);
}

// ── §18.8 \a (alert/bell = U+0007) ─────────────────────────────────────────

// §18.8 table: \a = Alert (bell) = U+0007 = 7.
// Before the fix \a fell to 'default' and returned (unsigned char)'a' = 97.
BEZEL_TEST(codegen_char_escape_527, spec_18_8_named_escape_alert) {
    BEZEL_EXPECT_EQ(run("return int('\\a');"), 7);
}

// ── Raw non-ASCII UTF-8 char literals ───────────────────────────────────────

// §18.3: a raw UTF-8 code point is a valid char literal.
// 'é' is U+00E9 = 233 encoded as the 2-byte UTF-8 sequence \xC3\xA9.
// Before the fix parseCharLiteral returned only the first byte (0xC3 = 195).
BEZEL_TEST(codegen_char_escape_527, spec_18_3_raw_utf8_e_acute_233) {
    // Source file is UTF-8; "é" embeds bytes \xC3\xA9 directly.
    BEZEL_EXPECT_EQ(run("return int('é');"), 233);
}

// §18.3: 'ĩ' is U+0129 = 297 (2-byte UTF-8: \xC4\xA9). 297 does not fit an
// 8-bit exit code, so observe it on stdout.
BEZEL_TEST(codegen_char_escape_527, spec_18_3_raw_utf8_i_tilde_297) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "Console.WriteLine(int('ĩ')); return 0; "
        " end Main; end C;"), "297");
}

// ── Control cases: existing escapes must still be correct ───────────────────

// §18.3: plain ASCII 'A' = 65. Must remain correct after the fix.
BEZEL_TEST(codegen_char_escape_527, control_plain_ascii_A_is_65) {
    BEZEL_EXPECT_EQ(run("return int('A');"), 65);
}

// §18.8: '\n' = newline = 10. Must remain correct.
BEZEL_TEST(codegen_char_escape_527, control_newline_escape_is_10) {
    BEZEL_EXPECT_EQ(run("return int('\\n');"), 10);
}

// §18.8: '\t' = horizontal tab = 9. Must remain correct.
BEZEL_TEST(codegen_char_escape_527, control_tab_escape_is_9) {
    BEZEL_EXPECT_EQ(run("return int('\\t');"), 9);
}

// §18.8 \x used for a simple ASCII value matches plain literal.
// '\x41' == 'A' both must equal 65.
BEZEL_TEST(codegen_char_escape_527, control_xhex_equals_plain_ascii) {
    BEZEL_EXPECT_EQ(run("return (1 if '\\x41' = 'A' else 0);"), 1);
}

// ── Non-ASCII comparison: §4.5 chars compare by code-point value ─────────────

// §4.5: "Character values compare by code-point value."
// 'ĩ' (U+0129=297) > 'Ĩ' (U+0128=296) must be true.
// Before the fix both likely decoded to the wrong first byte (0xC4) → equal.
BEZEL_TEST(codegen_char_escape_527, spec_4_5_nonascii_comparison_i_tilde_gt_I_tilde) {
    // Source file is UTF-8; the chars embed the correct byte sequences.
    BEZEL_EXPECT_EQ(run("return (1 if 'ĩ' > 'Ĩ' else 0);"), 1);
}

// ── Char range count over a small non-ASCII range ────────────────────────────

// §17.5 / §4.5: for var c in 'è'..'ê' counts 3 iterations
// (é=233, ê=234 — closed range). Accumulate into int.
BEZEL_TEST(codegen_char_escape_527, spec_17_5_nonascii_range_count) {
    BEZEL_EXPECT_EQ(
        run("var n := 0; "
            "for var c in '\\u00E8'..'\\u00EA' do n := n + 1; end; "
            "return n;"),
        3);
}
