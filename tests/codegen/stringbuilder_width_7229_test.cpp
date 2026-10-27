// Regression tests for #7229 — the StringBuilder threw the string's storage
// WIDTH away, so every code point above U+00FF was mangled and the mutators
// mutated at a BYTE offset while the caller passes a CODE-POINT index.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   spec §4.6 (v0.344), verbatim:
//     "The user-visible model is 'a sequence of code points indexed in O(1).'
//      The internal representation is implementation-defined: an implementation
//      is expected to choose a per-string storage width — typically 1, 2, or 4
//      bytes per code point, selected at construction based on the largest code
//      point in the value — to keep the common case (predominantly-ASCII text)
//      compact while preserving O(1) indexing for every string. The choice of
//      width is invisible at the language level; programs observe only the
//      `char` sequence and its length."
//
//   ⇒⇒ EACH CODE POINT OCCUPIES `width` BYTES. A width-2 string holds each
//   code point in two bytes, so the builder's content — the concatenation of
//   its parts' CODE POINTS — is not the concatenation of the parts' BYTES.
//
//   spec §4.6 (v0.344), verbatim: "`s.Length` returns the number of code points
//   in `s`" and "`s[i]` returns the `char` at code-point position `i`".
//
//   stdlib §3.1 (v0.344), verbatim: "`Insert` and `Remove` raise
//   `IndexOutOfRangeException` when the index or the `(startIndex, length)`
//   window falls outside `0..Length`" — and `Length` is the builder's
//   `Length` property, a count of CODE POINTS. So the mutators' index space
//   is the code-point index space, the same one `sb.Length` reports.
//
// THE DEFECT (measured on the base of this branch, a9a512ff).
//
//   * `bz_strbuilder_to_string` (src/runtime/bezel_runtime_strings.cpp)
//     concatenated the parts' RAW BYTES into one byte blob, and the codegen's
//     `emitStringBuilderToString` (src/codegen/LlvmBackend_String.cpp) stored
//     that blob as a string of a HARD-CODED width of 1 and a length equal to
//     the BYTE count. A width-2 part's two-bytes-per-code-point storage was
//     therefore read as one byte per code point: "日" (U+65E5, bytes E5 65)
//     came back as the TWO code points U+00E5 and U+0065 (229 and 101).
//   * `bz_sb_insert` / `bz_sb_remove` / `bz_sb_replace` addressed that same
//     byte blob, so an in-range CODE-POINT index could land in the middle of
//     a code point's storage: `sb.Append("日"); sb.Insert(1, "X")` split the
//     character and produced "åXe" (bytes C3 A5 58 65).
//
// The oracle is on stdoutText byte-for-byte: a mangled string still exits 0,
// so the exit code alone would be vacuous. Every row prints the LENGTH and the
// code points as INTS (so the expectation is pure ASCII and platform-
// independent) and the result of the CONTENT comparison `t = "…"` — the
// strongest form of the assertion, since it pins the content and not merely a
// length.

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

// #7229 / spec §4.6 (v0.344): the builder's content must come back as the code
// points that went in. "日" is U+65E5, which needs a width of 2; the pre-fix
// builder returned TWO code points (229 and 101) and a length of 2.
BEZEL_TEST(codegen_sb_width_spec_4_6, to_string_returns_the_wide_code_point) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  var eq: bool := t = \"日\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n26085\nTrue\n");
}

// #7229 / spec §4.6 + stdlib §3.1 (v0.344): `sb.Length` and
// `sb.ToString().Length` are the same quantity — a count of CODE POINTS — so
// they must agree for multi-byte content. Pre-fix they were 1 and 2.
BEZEL_TEST(codegen_sb_width_spec_4_6, builder_length_agrees_with_content_length) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString().Length.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n1\n");
}

// #7229 / stdlib §3.1 (v0.344): `Insert(index, value)` inserts at the
// CODE-POINT index `index`. Pre-fix the runtime addressed the flattened BYTE
// buffer, so index 1 landed inside U+65E5's two-byte storage and split the
// character: the content was "åXe" and the length 3.
BEZEL_TEST(codegen_sb_width_spec_4_6, insert_lands_on_the_code_point_index) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  sb.Insert(1, \"X\"); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(int(t[1]).ToString()); "
        "  var eq: bool := t = \"日X\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n26085\n88\nTrue\n");
}

// #7229 / spec §4.6 (v0.344): parts of DIFFERENT widths concatenate as CODE
// POINTS. "a" is width 1, "日" is width 2; a byte-level concatenation is not a
// code-point concatenation, so the pre-fix content was 4 byte-long code points
// (97, 0, 229, 101) instead of 3 (97, 26085, 98). This is the row that pins
// the fix as more than "store width 2".
BEZEL_TEST(codegen_sb_width_spec_4_6, mixed_width_parts_concatenate) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"a\"); "
        "  sb.Append(\"日\"); "
        "  sb.Append(\"b\"); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(int(t[1]).ToString()); "
        "  Console.WriteLine(int(t[2]).ToString()); "
        "  var eq: bool := t = \"a日b\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n97\n26085\n98\nTrue\n");
}

// #7229 / stdlib §3.1 (v0.344): `Remove(startIndex, length)` is in the same
// index space as `Insert` — it removes CODE POINTS. Pre-fix it removed BYTES,
// so `Remove(0, 1)` on "日x" cut U+65E5 in half and left the low byte.
BEZEL_TEST(codegen_sb_width_spec_4_6, remove_takes_a_code_point_index) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  sb.Append(\"x\"); "
        "  sb.Remove(0, 1); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  var eq: bool := t = \"x\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n120\nTrue\n");
}

// #7229 / stdlib §3.1 (v0.344): `Replace(oldValue, newValue)` matches a
// CODE-POINT sequence. Pre-fix it matched raw bytes, so the width-2 storage of
// "日" (bytes E5 65) did not match the width-2 storage of the same character
// inside a wider part (bytes E5 00 65 00) and the replacement silently did
// nothing.
BEZEL_TEST(codegen_sb_width_spec_4_6, replace_matches_a_code_point_sequence) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"a日b\"); "
        "  sb.Replace(\"日\", \"Z\"); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(int(t[1]).ToString()); "
        "  Console.WriteLine(int(t[2]).ToString()); "
        "  var eq: bool := t = \"aZb\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n97\n90\n98\nTrue\n");
}

// #7229 / spec §4.6 (v0.344): §4.6 counts CODE POINTS, and a code point
// above the BMP is ONE code point. U+1F600 needs a width of 4 (§4.5: a `char`
// is a 32-bit Unicode scalar value; §4.6: "1, 2, or 4 bytes per code point"),
// so the builder's content is ONE code point of 128512 — not four bytes read as
// four code points.
BEZEL_TEST(codegen_sb_width_spec_4_6, astral_code_point_is_one_code_point) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"😀\"); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  var eq: bool := t = \"😀\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n128512\nTrue\n");
}

// #7229 / spec §4.6 (v0.344) CONTROL: the ordinary (ASCII) case must be
// unchanged — a width-1 builder still reports 3 code points for "abc" and the
// content still compares equal to the literal.
BEZEL_TEST(codegen_sb_width_spec_4_6, ascii_content_is_unchanged) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"abc\"); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(int(t[2]).ToString()); "
        "  var eq: bool := t = \"abc\"; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n97\n99\nTrue\n");
}

// #7229 / spec §4.6 (v0.344): the builder's content and the string literal it
// came from are the same code-point sequence — the STRONGEST form of the
// assertion (it pins the CONTENT, not merely a length). The literal "日" is
// one code point (26085); pre-fix the builder's copy was two (229, 101).
BEZEL_TEST(codegen_sb_width_spec_4_6, content_equals_the_source_literal) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  var lit: string := \"日\"; "
        "  var eq: bool := sb.ToString() = lit; "
        "  Console.WriteLine(eq.ToString()); "
        "  Console.WriteLine((sb.ToString().Length = lit.Length).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\n");
}

// -------------------------------------------------------------------------
// THE WIDTH IS OBSERVABLE — the row that pins the width RULE.
//
// stdlib §9.6 (v0.344) declares, verbatim:
//   `public pure readonly property IsAscii: bool;  // true iff every code
//    point < 128 (O(1))`
// and spec §4.6 (v0.344) exposes the same predicate: "A pure-ASCII string
// (every code point in U+0000–U+007F) … Implementations expose this through
// the runtime predicate `s.IsAscii: bool`".
//
// ⇒ A string's storage width is NOT invisible at the language level: the
// runtime answers IsAscii from the width in O(1), so a string stored wider
// than its value requires can answer IsAscii differently from a `=`-equal
// string stored at its natural width.
//
// The case: a mutator NARROWS the content. "日" (U+65E5) replaced by "z"
// leaves a one-code-point content whose only code point is 122 < 128 ⇒ the
// result IS ASCII. The row asserts the VALUE (IsAscii is true) and that the
// result is `=`-equal to the literal "z" — two `=`-equal strings must not
// disagree on an observable.
//
// A builder that packs at the WIDEST of its parts' widths keeps the width-2
// storage after the replacement and answers FALSE here, while the literal
// "z" answers TRUE.
// -------------------------------------------------------------------------
BEZEL_TEST(codegen_sb_width_spec_4_6, replace_narrows_the_content_so_isascii_is_true) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  sb.Replace(\"日\", \"z\"); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(t.IsAscii.ToString()); "
        "  var lit: string := \"z\"; "
        "  Console.WriteLine(lit.IsAscii.ToString()); "
        "  var eq: bool := t = lit; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n122\nTrue\nTrue\nTrue\n");
}

// #7229 / spec §4.6 + stdlib §9.6 (v0.344): the same rule on the REMOVE
// path. "a" + "日" with the "日" removed leaves the one code point 'a' (97),
// which is ASCII — the content must be stored at the width that fits THAT
// value, not at the width the removed part had.
BEZEL_TEST(codegen_sb_width_spec_4_6, remove_narrows_the_content_so_isascii_is_true) {
    auto r = runAot(main_(
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"a\"); "
        "  sb.Append(\"日\"); "
        "  sb.Remove(1, 1); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(int(t[0]).ToString()); "
        "  Console.WriteLine(t.IsAscii.ToString()); "
        "  var lit: string := \"a\"; "
        "  Console.WriteLine(lit.IsAscii.ToString()); "
        "  var eq: bool := t = lit; "
        "  Console.WriteLine(eq.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n97\nTrue\nTrue\nTrue\n");
}

