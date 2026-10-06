// #535 — string search/slice methods must operate on CODE POINTS, not bytes (§4.6).
//
// The bug: Contains/IndexOf/LastIndexOf/StartsWith/EndsWith/Substring/Replace/Split
// use memmem/memcmp on raw byte buffers, passing len (code-point count) instead
// of len*width (byte count). For width-2/4 strings (non-ASCII code points stored
// as uint16_t/uint32_t), this reads only the first `len` bytes out of the
// actual `len*width` byte buffer, giving wrong results.
//
// The fix (LlvmBackend_String.cpp + bezel_runtime.cpp/.h): replace all byte-level
// search/slice operations with calls to new runtime helpers (bz_str_indexof,
// bz_str_contains, bz_str_startswith, bz_str_endswith, bz_str_lastindexof,
// bz_str_substring, bz_str_replace, bz_str_split_anychar_cp) that accept full
// bz_string* pointers and compare code-point by code-point.
//
// Tests derive from §4.6 ("a string is a sequence of Unicode scalar values")
// and §9.6 (string methods). Issue #535, follow-on to #528.
//
// Note: Bezel uses `=` (single equals) for equality comparison, not `==`.
//
// Coverage matrix:
//   * IndexOf:      "Hello 日本 World".IndexOf("日本") = 6 (code points, not bytes)
//   * Contains:     "Hello 日本 World".Contains("Hello") = true
//                   "Hello 日本 World".Contains("日本") = true
//   * StartsWith:   "Hello 日本".StartsWith("Hello") = true
//   * EndsWith:     "Hello 日本".EndsWith("日本") = true
//   * LastIndexOf:  "日本日本".LastIndexOf("日本") = 2 (second occurrence)
//   * Substring:    "Hello 日本 World".Substring(6, 2) = "日本"
//                   length of result = 2
//   * Replace:      "Hello 日本 World".Replace("日本", "Bezel") = "Hello Bezel World"
//   * Split:        "Hello 日本 World".Split(' ') gives ["Hello", "日本", "World"]
//   * Emoji (4-byte U+1F600): "a😀b".IndexOf("😀") = 1
//   * ASCII control: "hello".IndexOf("ll") = 2 (unchanged)

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Helper: wrap a method body inside a Main method.
int runBody(const std::string& body) {
    return run(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
}

// "Hello 日本 World" in UTF-8:
//   H(48) e(65) l(6C) l(6C) o(6F) SP(20)
//   日(E6 97 A5) 本(E6 9C AC) SP(20) W(57) o(6F) r(72) l(6C) d(64)
// Code points: H e l l o SP 日 本 SP W o r l d  — 14 code points total.
// 日 starts at code-point index 6.
static const char* kHelloJapanWorld = "Hello \xe6\x97\xa5\xe6\x9c\xac World";

// "日本" in UTF-8 (U+65E5 U+672C):
static const char* kJapan = "\xe6\x97\xa5\xe6\x9c\xac";

// "😀" = U+1F600 in UTF-8 (F0 9F 98 80):
static const char* kEmoji = "\xf0\x9f\x98\x80";

}  // namespace

// ── §9.6 / #535: IndexOf — code-point index, not byte offset ─────────────────

// "Hello 日本 World".IndexOf("日本") must be 6 (code-point offset), not 12
// (byte offset in the UTF-8 representation). Before fix: 12. After fix: 6.
BEZEL_TEST(codegen_string_methods_535, indexof_japanese_in_mixed_is_6) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var n := \"" + kJapan + "\"; "
        "return s.IndexOf(n);"), 6);
}

// "日本日本".IndexOf("日本") must be 0 (first occurrence at code-point 0).
BEZEL_TEST(codegen_string_methods_535, indexof_japanese_repeated_first_is_0) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kJapan + kJapan + "\"; "
        "var n := \"" + kJapan + "\"; "
        "return s.IndexOf(n);"), 0);
}

// "a😀b".IndexOf("😀") — 4-byte emoji code point must be at index 1 (code point).
// The string "a😀b" stored as width=4 has 3 code points: a(0), 😀(1), b(2).
// The byte buffer is 12 bytes (3 * 4 bytes each). Code-point index of 😀 is 1.
BEZEL_TEST(codegen_string_methods_535, indexof_emoji_is_1) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"a") + kEmoji + "b\"; "
        "var n := \"" + kEmoji + "\"; "
        "return s.IndexOf(n);"), 1);
}

// ASCII control: "hello".IndexOf("ll") must still be 2.
BEZEL_TEST(codegen_string_methods_535, indexof_ascii_control_is_2) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"hello\"; return s.IndexOf(\"ll\");"), 2);
}

// IndexOf not-found must still return -1.
// -1 does not round-trip through the 8-bit process exit code (runAotOut).
BEZEL_TEST(codegen_string_methods_535, indexof_not_found_is_minus1) {
    BEZEL_EXPECT_EQ(runAotOut(
        std::string("module M; class C public static method Main(): int "
                     "var s := \"") + kHelloJapanWorld + "\"; "
        "Console.WriteLine(s.IndexOf(\"xyz\")); return 0; "
        "end Main; end C;"), "-1");
}

// ── §9.6 / #535: LastIndexOf — code-point index ──────────────────────────────

// "日本日本".LastIndexOf("日本") must be 2 (last occurrence at code-point 2).
// In the internal width-2 representation (4 code points * 2 bytes = 8 bytes),
// a byte-level scan would return byte offset 4, not code-point index 2.
BEZEL_TEST(codegen_string_methods_535, lastindexof_japanese_repeated_is_2) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kJapan + kJapan + "\"; "
        "var n := \"" + kJapan + "\"; "
        "return s.LastIndexOf(n);"), 2);
}

// "Hello 日本 World".LastIndexOf("日本") == 6.
BEZEL_TEST(codegen_string_methods_535, lastindexof_japanese_in_mixed_is_6) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var n := \"" + kJapan + "\"; "
        "return s.LastIndexOf(n);"), 6);
}

// ASCII control: "abab".LastIndexOf("ab") must still be 2.
BEZEL_TEST(codegen_string_methods_535, lastindexof_ascii_control_is_2) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"abab\"; return s.LastIndexOf(\"ab\");"), 2);
}

// ── §9.6 / #535: Contains — code-point-aware search ─────────────────────────

// "Hello 日本 World".Contains("Hello") must be true.
// Before fix: memmem searched width-2 data (14 * 2 = 28 bytes) for the bytes of
// "Hello" (width-1, 5 bytes). The width-2 representation of 'H','e','l','l','o'
// uses 2 bytes each (0x48 0x00, 0x65 0x00, ...), so "Hello" as raw bytes doesn't
// appear in the buffer → false. After fix: code-point comparison → true.
BEZEL_TEST(codegen_string_methods_535, contains_ascii_in_mixed_is_true) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "return (if s.Contains(\"Hello\") then 1 else 0);"), 1);
}

// "Hello 日本 World".Contains("日本") must be true.
BEZEL_TEST(codegen_string_methods_535, contains_japanese_in_mixed_is_true) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var n := \"" + kJapan + "\"; "
        "return (if s.Contains(n) then 1 else 0);"), 1);
}

// "Hello 日本 World".Contains("xyz") must be false.
BEZEL_TEST(codegen_string_methods_535, contains_absent_is_false) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "return (if s.Contains(\"xyz\") then 1 else 0);"), 0);
}

// ASCII control: "Hello World".Contains("lo W") must still be true.
BEZEL_TEST(codegen_string_methods_535, contains_ascii_control_is_true) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"Hello World\"; "
        "return (if s.Contains(\"lo W\") then 1 else 0);"), 1);
}

// ── §9.6 / #535: StartsWith — code-point-aware prefix check ─────────────────

// "Hello 日本".StartsWith("Hello") must be true.
// Before fix: memcmp(width-2 buffer, "Hello" bytes, 5) — the first bytes of
// the width-2 buffer encode 'H' as two bytes (0x48 0x00), not one → mismatch.
BEZEL_TEST(codegen_string_methods_535, startswith_ascii_in_mixed_is_true) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"Hello ") + kJapan + "\"; "
        "return (if s.StartsWith(\"Hello\") then 1 else 0);"), 1);
}

// "日本 Hello".StartsWith("日本") must be true.
BEZEL_TEST(codegen_string_methods_535, startswith_japanese_prefix_is_true) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kJapan + " Hello\"; "
        "var p := \"" + kJapan + "\"; "
        "return (if s.StartsWith(p) then 1 else 0);"), 1);
}

// "日本 Hello".StartsWith("Hello") must be false.
BEZEL_TEST(codegen_string_methods_535, startswith_wrong_prefix_is_false) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kJapan + " Hello\"; "
        "return (if s.StartsWith(\"Hello\") then 1 else 0);"), 0);
}

// ASCII control: "Hello World".StartsWith("Hello") must still be true.
BEZEL_TEST(codegen_string_methods_535, startswith_ascii_control_is_true) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"Hello World\"; "
        "return (if s.StartsWith(\"Hello\") then 1 else 0);"), 1);
}

// ── §9.6 / #535: EndsWith — code-point-aware suffix check ───────────────────

// "Hello 日本".EndsWith("日本") must be true.
// Before fix: endsWith computes byte offset = selfLen - sufLen = 8 - 2 = 6,
// then memcmp(self.data+6, suffix.data, 2). In width-2 storage, the real byte
// offset of the last two code points is (8-2)*2 = 12, not 6 → reads wrong bytes.
BEZEL_TEST(codegen_string_methods_535, endswith_japanese_suffix_is_true) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"Hello ") + kJapan + "\"; "
        "var suf := \"" + kJapan + "\"; "
        "return (if s.EndsWith(suf) then 1 else 0);"), 1);
}

// "日本 Hello".EndsWith("Hello") must be true.
BEZEL_TEST(codegen_string_methods_535, endswith_ascii_suffix_in_mixed_is_true) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kJapan + " Hello\"; "
        "return (if s.EndsWith(\"Hello\") then 1 else 0);"), 1);
}

// "Hello 日本".EndsWith("Hello") must be false.
BEZEL_TEST(codegen_string_methods_535, endswith_wrong_suffix_is_false) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"Hello ") + kJapan + "\"; "
        "return (if s.EndsWith(\"Hello\") then 1 else 0);"), 0);
}

// ASCII control: "Hello World".EndsWith("World") must still be true.
BEZEL_TEST(codegen_string_methods_535, endswith_ascii_control_is_true) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"Hello World\"; "
        "return (if s.EndsWith(\"World\") then 1 else 0);"), 1);
}

// ── §9.6 / #535: Substring — code-point-based start/length ──────────────────

// "Hello 日本 World".Substring(6, 2).Length must be 2.
// Code points: H(0) e(1) l(2) l(3) o(4) SP(5) 日(6) 本(7) SP(8) W(9) o(10) r(11) l(12) d(13)
BEZEL_TEST(codegen_string_methods_535, substring_japanese_length_is_2) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var sub := s.Substring(6, 2); "
        "return sub.Length;"), 2);
}

// "Hello 日本 World".Substring(6, 2) must equal "日本".
// Verify by structural equality (Bezel uses single `=` for value equality).
BEZEL_TEST(codegen_string_methods_535, substring_japanese_equals_japan) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var sub := s.Substring(6, 2); "
        "var n := \"" + kJapan + "\"; "
        "return (if sub = n then 1 else 0);"), 1);
}

// "Hello 日本 World".Substring(0, 5) must equal "Hello".
BEZEL_TEST(codegen_string_methods_535, substring_ascii_prefix_from_mixed) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var sub := s.Substring(0, 5); "
        "return (if sub = \"Hello\" then 1 else 0);"), 1);
}

// "Hello 日本 World".Substring(9) must equal "World" (from code point 9 onward).
BEZEL_TEST(codegen_string_methods_535, substring_tail_from_mixed) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var sub := s.Substring(9); "
        "return (if sub = \"World\" then 1 else 0);"), 1);
}

// ASCII control: "Hello World".Substring(6, 5) must still equal "World".
BEZEL_TEST(codegen_string_methods_535, substring_ascii_control) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"Hello World\"; "
        "var sub := s.Substring(6, 5); "
        "return (if sub = \"World\" then 1 else 0);"), 1);
}

// ── §9.6 / #535: Replace (string overload) — code-point-aware ───────────────

// "Hello 日本 World".Replace("日本", "Bezel") must give "Hello Bezel World".
BEZEL_TEST(codegen_string_methods_535, replace_japanese_with_ascii) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var n := \"" + kJapan + "\"; "
        "var r := s.Replace(n, \"Bezel\"); "
        "return (if r = \"Hello Bezel World\" then 1 else 0);"), 1);
}

// "Hello World".Replace("World", "日本") must give "Hello 日本".
BEZEL_TEST(codegen_string_methods_535, replace_ascii_with_japanese) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"Hello World\"; "
        "var n := \"") + kJapan + "\"; "
        "var r := s.Replace(\"World\", n); "
        "var expect := \"Hello \" + n; "
        "return (if r = expect then 1 else 0);"), 1);
}

// ASCII control: "abcabc".Replace("bc", "x") must still give "axax".
BEZEL_TEST(codegen_string_methods_535, replace_ascii_control) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"abcabc\"; "
        "var r := s.Replace(\"bc\", \"x\"); "
        "return (if r = \"axax\" then 1 else 0);"), 1);
}

// ── §9.6 / #535: Split (char overload) — code-point-aware ───────────────────

// "Hello 日本 World".Split(' ') must yield 3 segments.
// The space character is code point U+0020 (ASCII); in a width-2 string it is
// stored as two bytes (0x20 0x00 on little-endian). A byte-level scan treats
// the low byte of every code point as a potential separator — for width-2 data
// this finds 0x20 at every even byte where the high byte is zero (e.g. the
// high byte of ASCII code points in the 0x0000..0x00FF range), producing far
// too many splits. A code-point scan checks each code point as a whole 32-bit
// value against U+0020, producing exactly the 3 correct segments.
BEZEL_TEST(codegen_string_methods_535, split_by_space_in_mixed_count_is_3) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var parts := s.Split(' '); "
        "return parts.Count;"), 3);
}

// "Hello 日本 World".Split(' ')[0] must be "Hello" (Length 5).
BEZEL_TEST(codegen_string_methods_535, split_by_space_first_len_is_5) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var parts := s.Split(' '); "
        "return parts[0].Length;"), 5);
}

// "Hello 日本 World".Split(' ')[1] must be "日本" (Length 2).
BEZEL_TEST(codegen_string_methods_535, split_by_space_second_len_is_2) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var parts := s.Split(' '); "
        "return parts[1].Length;"), 2);
}

// "Hello 日本 World".Split(' ')[2] must be "World" (Length 5).
BEZEL_TEST(codegen_string_methods_535, split_by_space_third_len_is_5) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var parts := s.Split(' '); "
        "return parts[2].Length;"), 5);
}

// "Hello 日本 World".Split(' ')[1] must equal "日本" (value equality).
BEZEL_TEST(codegen_string_methods_535, split_by_space_second_is_japan) {
    BEZEL_EXPECT_EQ(runBody(
        std::string("var s := \"") + kHelloJapanWorld + "\"; "
        "var parts := s.Split(' '); "
        "var n := \"" + kJapan + "\"; "
        "return (if parts[1] = n then 1 else 0);"), 1);
}

// ASCII control: "a b c".Split(' ') must still yield 3 parts.
BEZEL_TEST(codegen_string_methods_535, split_ascii_control_count_is_3) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"a b c\"; var parts := s.Split(' '); return parts.Count;"), 3);
}

// ASCII control: "hello".Split('l') must yield 3 parts ("he","","o").
BEZEL_TEST(codegen_string_methods_535, split_ascii_repeated_sep) {
    BEZEL_EXPECT_EQ(runBody(
        "var s := \"hello\"; var parts := s.Split('l'); return parts.Count;"), 3);
}
