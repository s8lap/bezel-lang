// Regression tests for #7245 — `String.IsAscii` keyed on the STORAGE WIDTH
// instead of on the CONTENT.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   stdlib §9.6 (v0.344), verbatim:
//     "public pure readonly property IsAscii: bool;  // true iff every code
//      point < 128 (O(1))"
//
//   ⇒⇒ THE PREDICATE IS DEFINED BY THE CONTENT: it is true iff EVERY code
//   point of the value is < 128. It says nothing about how the value is
//   stored.
//
//   spec §4.6 (v0.344), verbatim:
//     "The internal representation is implementation-defined: an implementation
//      is expected to choose a per-string storage width — typically 1, 2, or 4
//      bytes per code point, selected at construction based on the largest code
//      point in the value — to keep the common case (predominantly-ASCII text)
//      compact while preserving O(1) indexing for every string. The choice of
//      width is invisible at the language level; programs observe only the
//      `char` sequence and its length."
//
//   ⇒⇒ THE WIDTH IS INVISIBLE AT THE LANGUAGE LEVEL. A legal implementation
//   may store a value at a width wider than its content needs (the range
//   slice preserves the SOURCE's width, so a slice of a wide string is stored
//   wide even when its content is one ASCII code point). A predicate that
//   answers from the width therefore reports something the program cannot
//   observe from the value — and the stdlib defines the property by content.
//
// THE DEFECT (measured on the base of this branch, 9961f766).
//
//   `bz_string_is_ascii` (src/runtime/bezel_runtime_strings.cpp:33) answered
//   from the WIDTH:
//
//       if (s->width != 1) return 0;                // ≥2-byte code points ⇒ not ASCII
//
//   so a value stored at width 2 or 4 answered FALSE even when every one of
//   its code points was ASCII. The issue's own repro is a slice of a wide
//   string:
//
//       ("a" + "日")[0..<1].IsAscii     // => False, must be True
//
//   ("a" + "日") is two code points — 'a' (97) and U+65E5 — and it is stored
//   at width 2 because of the U+65E5. The range slice [0..<1] keeps ONE code
//   point ('a', 97 — pure ASCII) and PRESERVES the source's width (§4.6 lets
//   the slice keep it), so the slice is a width-2 string whose content is
//   ASCII — and the width-based predicate answered False.
//
//   The fix answers from the CONTENT: the runtime reads each code point in
//   the code-point domain (via the width-aware `bz_str_cp` helper) and
//   reports a code point ≥ 128, so every legal width gives the same,
//   content-determined answer.
//
// THE ORACLE is on stdout, byte-for-byte. `IsAscii` is a BOOL, so a wrong
// answer is a silent one: the program still exits 0, so the exit code alone
// would be VACUOUS. Each row prints the length, the code point as an INT (so
// the expectation is pure ASCII and platform-independent), the `IsAscii`
// predicate, and — where the row is about a slice — the CONTENT COMPARISON
// `t = "…"`, the strongest form of the assertion, since it pins the content
// and not merely a length.
//
// The rows below pin BOTH directions of the predicate, because a fix that
// simply inverted the shortcut would pass the issue's repro and break the
// other cases:
//
//   * a width-2 / width-4 value whose content is ASCII  ⇒ TRUE (the defect);
//   * a width-2 value whose content has a code point ≥ 128 ⇒ FALSE;
//   * a width-1 value whose content has a byte in [128,255] ⇒ FALSE (the
//     Latin-1 case: the width says nothing about the code points);
//   * the empty value ⇒ TRUE.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string main_(const std::string& body) {
    return std::string("module M; class C public static method Main(): int ") +
           body + "  return 0; end Main; end C;";
}

}  // namespace

// #7245 / stdlib §9.6 + spec §4.6 (v0.344) — THE DEFECT'S OWN REPRO.
//
// ("a" + "日") is stored at width 2 (its largest code point is U+65E5). The
// range slice [0..<1] keeps one code point and preserves the source's width,
// so the result is a WIDTH-2 string whose content is the single ASCII code
// point 'a'. stdlib §9.6 defines `IsAscii` by the content ("true iff every
// code point < 128"), so the answer is TRUE.
//
// Pre-fix the runtime answered from the width and reported False.
BEZEL_TEST(codegen_sb_isascii_content_7245, a_slice_of_a_wide_string_is_ascii) {
    auto r = runAot(main_(
        "  var s := (\"a\" + \"日\")[0..<1]; "
        "  Console.WriteLine(s.Length.ToString()); "
        "  Console.WriteLine(int(s[0]).ToString()); "
        "  Console.WriteLine(s.IsAscii.ToString()); "
        "  var eq: bool := s = \"a\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n97\nTrue\nTrue\n");
}

// #7245 / stdlib §9.6 + spec §4.6 (v0.344) — THE SAME DEFECT AT WIDTH 4.
//
// "😀" is U+1F600, a code point that needs four bytes per code point (§4.6:
// "1, 2, or 4 bytes per code point"), so the source is stored at width 4 and
// the slice keeps that width. The slice's content is the single ASCII code
// point 'a', so `IsAscii` is TRUE — the predicate may not answer from the
// width.
//
// Pre-fix the runtime answered from the width and reported False.
BEZEL_TEST(codegen_sb_isascii_content_7245, a_slice_of_an_astral_string_is_ascii) {
    auto r = runAot(main_(
        "  var s := (\"a\" + \"😀\")[0..<1]; "
        "  Console.WriteLine(s.Length.ToString()); "
        "  Console.WriteLine(int(s[0]).ToString()); "
        "  Console.WriteLine(s.IsAscii.ToString()); "
        "  var eq: bool := s = \"a\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n97\nTrue\nTrue\n");
}

// #7245 / stdlib §9.6 (v0.344) — THE OTHER DIRECTION, ON A WIDE VALUE.
//
// A width-2 value whose content carries a code point in U+0080..U+00FF
// (U+00E9 'é' = 233) is NOT ASCII: 233 ≥ 128, so the property is FALSE. The
// row is the mirror of the two above — a fix that made every wide value
// answer TRUE would pass those and fail this one.
//
// The row also pins the content, so it fails for the right reason: the
// content is 'a' followed by 'é', not a mangled pair of bytes.
BEZEL_TEST(codegen_sb_isascii_content_7245, a_wide_value_with_a_latin1_code_point_is_not_ascii) {
    auto r = runAot(main_(
        "  var s := (\"a\" + \"é\" + \"日\")[0..<2]; "
        "  Console.WriteLine(s.Length.ToString()); "
        "  Console.WriteLine(int(s[0]).ToString()); "
        "  Console.WriteLine(int(s[1]).ToString()); "
        "  Console.WriteLine(s.IsAscii.ToString()); "
        "  var eq: bool := s = \"aé\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n97\n233\nFalse\nTrue\n");
}

// #7245 / stdlib §9.6 (v0.344) — THE WIDTH-1 LATIN-1 CASE.
//
// A width-1 value can hold any code point ≤ 255, so a width-1 value is NOT
// automatically ASCII either: 'é' (U+00E9 = 233) is ≥ 128. The answer is
// FALSE. (The pre-fix runtime got this case right by scanning the bytes; the
// row keeps it that way.)
BEZEL_TEST(codegen_sb_isascii_content_7245, a_narrow_value_with_a_latin1_code_point_is_not_ascii) {
    auto r = runAot(main_(
        "  Console.WriteLine(\"H\\u00e9llo\".IsAscii.ToString()); "
        "  Console.WriteLine(\"Hello\".IsAscii.ToString()); "
        "  Console.WriteLine(\"\".IsAscii.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\nTrue\n");
}

// #7245 / stdlib §9.6 (v0.344) — THE WIDE VALUE THAT IS NOT ASCII.
//
// ("a" + "日") holds U+65E5, which is ≥ 128, so the property is FALSE. This
// is the row the pre-fix runtime answered by accident (the width said "not
// ASCII"); the content-based answer must agree with it.
BEZEL_TEST(codegen_sb_isascii_content_7245, a_wide_value_is_not_ascii) {
    auto r = runAot(main_(
        "  var s := \"a\" + \"日\"; "
        "  Console.WriteLine(s.Length.ToString()); "
        "  Console.WriteLine(int(s[1]).ToString()); "
        "  Console.WriteLine(s.IsAscii.ToString()); "
        "  Console.WriteLine((\"😀\").IsAscii.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n26085\nFalse\nFalse\n");
}
