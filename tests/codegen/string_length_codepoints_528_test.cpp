// #528 — string.Length must count CODE POINTS, not UTF-8 bytes (§4.6).
//
// The bug: `processStringCodePoints` in LlvmBackend_String.cpp pushed one
// entry per SOURCE BYTE in the non-backslash path.  A multi-byte UTF-8
// sequence therefore produced multiple entries (one per byte), making the
// stored `length` field equal to the UTF-8 byte count rather than the
// Unicode code-point count.
//
// Fix (same file): in the non-backslash path, decode a full UTF-8 scalar
// (1-4 bytes per the lead-byte pattern) and push ONE code-point entry per
// scalar.  Escape sequences already pushed the correct scalar, so those tests
// are controls that must continue to pass.
//
// Tests derive from §4.6 ("string.Length is the number of code points"),
// §18.8 (escape forms produce a single code point), and issue #528.
//
// Coverage matrix:
//   * Literal Length:      "Café" == 4, "naïve" == 5, "😀" == 1 (4-byte),
//                          "日本" == 2, "abc" == 3, "" == 0
//   * Escape controls:     "é" (é) == 1, "\n" == 1  (must still pass)
//   * Indexing:            "Café"[3] == 233  (é = U+00E9; verifies array contents)
//   * Runtime-built:       ("Ca" + "fé").Length == 4  (concat path)
//   * Interpolation:       $"x{s}".Length where s="fé" → 3  (interpolation path)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Helper: wrap a method body.
int runBody(const std::string& body) {
    return run(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
}

}  // namespace

// ── §4.6 / #528: literal Length counts code points, not bytes ───────────────

// §4.6 / #528: "Café" — 'C','a','f','é' — é is U+00E9 (2 UTF-8 bytes).
// Before fix: 5 (byte count). After fix: 4 (code-point count).
BEZEL_TEST(codegen_string_length_528, cafe_length_is_4) {
    BEZEL_EXPECT_EQ(runBody("var s := \"Caf\xc3\xa9\"; return s.Length;"), 4);
}

// §4.6 / #528: "naïve" — 'n','a','ï','v','e' — ï is U+00EF (2 UTF-8 bytes).
// Before fix: 6 (byte count). After fix: 5 (code-point count).
BEZEL_TEST(codegen_string_length_528, naive_length_is_5) {
    BEZEL_EXPECT_EQ(runBody("var s := \"na\xc3\xafve\"; return s.Length;"), 5);
}

// §4.6 / #528: "😀" — U+1F600 — 4 UTF-8 bytes (F0 9F 98 80).
// Before fix: 4 (byte count). After fix: 1.
BEZEL_TEST(codegen_string_length_528, emoji_length_is_1) {
    BEZEL_EXPECT_EQ(runBody("var s := \"\xf0\x9f\x98\x80\"; return s.Length;"), 1);
}

// §4.6 / #528: "日本" — U+65E5 U+672C — each is 3 UTF-8 bytes.
// Before fix: 6 (byte count). After fix: 2.
BEZEL_TEST(codegen_string_length_528, japanese_length_is_2) {
    BEZEL_EXPECT_EQ(runBody("var s := \"\xe6\x97\xa5\xe6\x9c\xac\"; return s.Length;"), 2);
}

// ── §4.6: control — ASCII strings must still work ───────────────────────────

// §4.6 / #528 control: pure ASCII "abc" must still be 3.
BEZEL_TEST(codegen_string_length_528, ascii_control_3) {
    BEZEL_EXPECT_EQ(runBody("var s := \"abc\"; return s.Length;"), 3);
}

// §4.6 / #528 control: empty string must still be 0.
BEZEL_TEST(codegen_string_length_528, empty_control_0) {
    BEZEL_EXPECT_EQ(runBody("var s := \"\"; return s.Length;"), 0);
}

// ── §4.6 / §18.8: escape-form controls (must still be 1 per scalar) ─────────

// §18.8 / #528 control: "é" via \uXXXX escape — always produced 1 entry
// (escape branch never touched the UTF-8 decode path). Must remain 1.
BEZEL_TEST(codegen_string_length_528, escape_e_acute_length_1) {
    BEZEL_EXPECT_EQ(runBody("var s := \"\\u00E9\"; return s.Length;"), 1);
}

// §18.8 / #528 control: "\n" is 1 escape sequence → 1 code point.
BEZEL_TEST(codegen_string_length_528, escape_newline_length_1) {
    BEZEL_EXPECT_EQ(runBody("var s := \"\\n\"; return s.Length;"), 1);
}

// ── §4.6 / #528: indexing returns the correct code-point value ──────────────

// §4.6 / #528: "Café"[3] must be 233 (U+00E9 = é), not a raw byte value.
// This verifies that the code-point array stores the scalar, not individual
// bytes, and that loadCodePoint returns the right value.
// Note: the exit code is taken mod 256 by the OS; 233 mod 256 = 233, so no
// wrapping occurs — the raw value is directly observable.
BEZEL_TEST(codegen_string_length_528, cafe_index_3_is_e_acute) {
    BEZEL_EXPECT_EQ(runBody("var s := \"Caf\xc3\xa9\"; return s[3];"), 233);
}

// fix #5150 (§3.5, v0.309): respelled `s[0] mod 256` as `int(s[0]) mod 256` --
// `mod` lists integer types and `decimal` only, so a `char` operand is a compile
// error (the old spelling is pinned as a reject in
// binary_mod_char_char_rejected_3109_narrow).
// §4.6 / #528: "日本"[0] == U+65E5 == 26085.
// Exit codes are 0-255, so check mod 256: 26085 mod 256 = 101.
BEZEL_TEST(codegen_string_length_528, japanese_index_0_mod256) {
    BEZEL_EXPECT_EQ(
        runBody("var s := \"\xe6\x97\xa5\xe6\x9c\xac\"; return int(s[0]) mod 256;"),
        26085 % 256);
}

// §4.6 / #528: "😀"[0] == U+1F600 == 128512. mod 256 = 128.
BEZEL_TEST(codegen_string_length_528, emoji_index_0_mod256) {
    BEZEL_EXPECT_EQ(
        runBody("var s := \"\xf0\x9f\x98\x80\"; return int(s[0]) mod 256;"),
        128512 % 256);
}

// ── §4.6 / #528: runtime-built strings — concatenation ──────────────────────

// §4.6 / #528: ("Ca" + "fé").Length — the concat path (emitStringConcat)
// reads the stored `length` field from each bz_string and adds them.
// "Ca".Length == 2 + "fé".Length == 2 → total 4.
// If emitStringConcat itself reads a wrong len it would also show here.
BEZEL_TEST(codegen_string_length_528, concat_cafe_length_is_4) {
    BEZEL_EXPECT_EQ(runBody(
        "var a := \"Ca\"; var b := \"f\xc3\xa9\"; "
        "var c := a + b; return c.Length;"), 4);
}

// §4.6 / #528: concat "日" + "本" → length 2.
BEZEL_TEST(codegen_string_length_528, concat_japanese_length_is_2) {
    BEZEL_EXPECT_EQ(runBody(
        "var a := \"\xe6\x97\xa5\"; var b := \"\xe6\x9c\xac\"; "
        "var c := a + b; return c.Length;"), 2);
}

// ── §18.5 / §4.6 / #528: runtime-built strings — interpolation ──────────────

// §18.5 / #528: $"x{s}" where s == "fé" — the interpolation path converts
// s to string via emitPrimitiveToString (identity for string) then concatenates
// "x" + s.  Result must be length 3 ("x","f","é" — 3 code points).
BEZEL_TEST(codegen_string_length_528, interp_nons_ascii_length) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"f\xc3\xa9\"; "
        "var r := $\"x{s}\"; return r.Length;"), 3);
}

// §18.5 / #528: $"Caf{s}" where s == "é" (raw UTF-8) → length 4.
BEZEL_TEST(codegen_string_length_528, interp_prefix_nonascii_length) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"\xc3\xa9\"; "
        "var r := $\"Caf{s}\"; return r.Length;"), 4);
}

// ── §9.6 / #1295: PadLeft/PadRight preserve wide code points (> U+00FF) ──────
// The prior inline lowering assumed 1 byte per code point (memcpy'd the code-
// point COUNT as a byte count, hard-stored width=1), corrupting any cp > U+00FF.
// The result is compared at the code-point level (`int(p[i]) = <cp>`) because an
// 8-bit exit code cannot distinguish the corruption (20013 & 255 == 45, the very
// low byte the bug produced).

// §9.6 (#1295): "中" (U+4E2D=20013).PadLeft(4) → "   中"; the tail code point is
// preserved (20013), the pad is a space (32), Length is 4.
BEZEL_TEST(codegen_string_length_528, padleft_wide_codepoint_1295) {
    BEZEL_EXPECT_EQ(runBody(
        "var p := \"\xe4\xb8\xad\".PadLeft(4); "
        "return (if (p.Length = 4) and (int(p[3]) = 20013) "
        "and (int(p[0]) = 32) then 1 else 0);"), 1);
}

// §9.6 (#1295): "中".PadRight(3) → "中  "; the head code point is preserved.
BEZEL_TEST(codegen_string_length_528, padright_wide_codepoint_1295) {
    BEZEL_EXPECT_EQ(runBody(
        "var p := \"\xe4\xb8\xad\".PadRight(3); "
        "return (if (p.Length = 3) and (int(p[0]) = 20013) "
        "and (int(p[2]) = 32) then 1 else 0);"), 1);
}

// §9.6 (#1295): a WIDE pad char widens the result — "x".PadLeft(3, '★'
// U+2605=9733) keeps the pad code point AND the ASCII 'x' (widened).
BEZEL_TEST(codegen_string_length_528, pad_widens_for_wide_padchar_1295) {
    BEZEL_EXPECT_EQ(runBody(
        "var p := \"x\".PadLeft(3, '\xe2\x98\x85'); "
        "return (if (p.Length = 3) and (int(p[0]) = 9733) "
        "and (int(p[2]) = 120) then 1 else 0);"), 1);
}

// §9.6 (#1295) control: pure-ASCII PadLeft/PadRight are unchanged.
BEZEL_TEST(codegen_string_length_528, pad_ascii_unchanged_1295) {
    BEZEL_EXPECT_EQ(runBody(
        "var a := \"ab\".PadLeft(5); "
        "var b := \"ab\".PadRight(4, '*'); "
        "return (if (a = \"   ab\") and (b = \"ab**\") then 1 else 0);"), 1);
}
