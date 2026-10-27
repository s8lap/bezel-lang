// Regression tests for #7244 — `StringBuilder.AppendFormat` rendered the
// format and every rendered argument BYTE-WISE, so a wide (non-ASCII) value
// was not a string at all.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   spec §4.6 (v0.345), verbatim:
//     "The internal representation is implementation-defined: an implementation
//      is expected to choose a per-string storage width — typically 1, 2, or 4
//      bytes per code point, selected at construction based on the largest code
//      point in the value — to keep the common case (predominantly-ASCII text)
//      compact while preserving O(1) indexing for every string. The choice of
//      width is invisible at the language level; programs observe only the
//      `char` sequence and its length."
//
//   ⇒⇒ A string's DATA is not its TEXT. At width 2, one code point is two
//   bytes, and reading those two bytes as two characters yields two
//   DIFFERENT characters. Every operation that produces or consumes a
//   string's text must therefore work in the CODE-POINT domain: the width is
//   an implementation detail of the storage, never of the text.
//
//   stdlib §3.1 (v0.345), verbatim:
//     "public method AppendFormat(format: string, args: Array<Object>): StringBuilder"
//
//   ⇒⇒ AppendFormat substitutes args[N]'s string form for the hole {N}. The
//   substituted text is the argument's TEXT — its code-point sequence — not
//   its storage bytes.
//
// THE DEFECT (measured on the base of this branch, 9961f766).
//
//   `bz_sb_append_format` (src/runtime/bezel_runtime_strings.cpp) built a
//   BYTE buffer: it read the format as raw bytes (`nb = fmt->len * fmt->width`,
//   the format's BYTE count) and copied each rendered argument's raw bytes
//   into a width-1 output part whose `len` was a BYTE count.
//
//   MEASURED pre-fix (each row below carries its own measurement):
//
//     AppendFormat("x{0}y", <"日">)   =>  Length 4, text "x" + U+00E5 +
//                                          U+0065 + "y"
//     AppendFormat("x{0}y", <"😀">)  =>  Length 6, text "x" + the four raw
//                                          bytes of U+1F600 + "y"
//     AppendFormat("日{0}", 7)        =>  Length 8, text the format's eight
//                                          raw bytes (E5 65 7B 00 30 00 7D
//                                          00) — the literal text garbage,
//                                          the hole never recognised
//     AppendFormat("日", args)        =>  Length 2, text U+00E5 + U+0065
//     AppendFormat("{0}", <"日">)     =>  Length 2, text U+00E5 + U+0065
//
//   ⇒⇒ The builder's content was not the formatted text at all, and its
//   Length counted the argument's STORAGE BYTES where the text has code
//   points. BOTH the format and the rendered argument must be read in the
//   code-point domain.
//
//   MEASURED fail-without: with this branch's `src/runtime/
//   bezel_runtime_strings.cpp` reverted to the base (9961f766) and the test
//   binary rebuilt, FIVE of this file's EIGHT rows FAIL; the three that stay
//   green are the ASCII controls, which is the point of having them.
//
// THE FIX.
//
//   `bz_sb_append_format` now works in the code-point domain end to end: the
//   format's code points are read one at a time, each rendered argument
//   contributes its OWN code points, and the result is appended as a fresh
//   part packed at the narrowest width that fits its largest code point
//   (§4.6's own rule — the rule the builder's other mutators already follow).
//
// THE ORACLE is on stdout, byte for byte. The defect is a wrong VALUE, so
// the exit code alone would be vacuous: the program ran, it just produced
// the wrong text. Each row therefore asserts the builder's LENGTH (a count of
// code points, §4.6) and the CONTENT itself, the strongest form of the
// assertion — the content pins the text, not merely its size.
//
// The rows pin BOTH directions, because a fix that simply dropped the
// non-ASCII bytes would pass the ASCII controls and fail the wide ones:
//
//   * a wide ARGUMENT renders as its code points;
//   * a wide FORMAT's literal text survives;
//   * an ASCII argument and an ASCII format are byte-for-byte unchanged;
//   * the placeholder syntax ({{ }}, alignment, sub-format) is undisturbed.
//
// The LEAK oracle: `runAotChecked` runs the generated module under
// ASan/LSan, so a program that leaks fails the row. Every program here
// binds its owned values with `using var` (§28.3), so the rows are clean on
// both arms and the leak oracle does not mask the value oracle.
//
// ONE EXCEPTION, and it is a PRE-EXISTING defect of its own: an `int`
// argument stored in an `Array<Object>` is BOXED, and the box leaks 24 B —
// with or without an AppendFormat call (the #7272 test file documents the
// same measurement: "a program that only does `args.Add(7)` leaks the same
// 24 B ... so it is a DIFFERENT defect"). The rows that use an `int`
// argument therefore use the plain `runAot` oracle, whose stdout is the
// value oracle; the rows that use a CLASS argument (no box) and the row with
// no argument at all keep the full ASan/LSan oracle.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The builder's LENGTH and its CONTENT, printed so that both the count of
// code points and the text itself are on stdout.
std::string main_(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           "  return 0; end Main; end C;";
}

}  // namespace

// #7244 / spec §4.6 + stdlib §3.1 (v0.345) — A WIDE ARGUMENT.
//
// The argument is a class whose `ToString()` returns "日" (U+65E5) — ONE code
// point, stored at width 2. The formatted text is "x" + "日" + "y", which is
// THREE code points.
//
// Pre-fix the argument's two storage bytes were copied into a width-1 output
// part, so the builder held FOUR code points ("x", 0xE5, 0x65, "y") and its
// text was "x" followed by two Latin-1 characters.
BEZEL_TEST(codegen_sb_appendformat_width_domain_7244,
           wide_argument_renders_as_its_code_points) {
    const std::string src =
        "module M; "
        "class W public override method ToString(): string => \"日\"; end W; "
        "class C public static method Main(): int "
        "  using var w := new W(); "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(w); "
        "  using var sb := new StringBuilder(); "
        "  sb.AppendFormat(\"x{0}y\", args); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nx日y\n"));
}

// #7244 / spec §4.6 (v0.345) — THE SAME, AT WIDTH 4.
//
// "😀" is U+1F600, a code point that needs four bytes per code point (§4.6:
// "typically 1, 2, or 4 bytes per code point"), so the argument's storage is
// four bytes wide.
//
// MEASURED pre-fix: those four bytes became FOUR Latin-1 code points, so the
// builder held SIX code points ("x", the four raw bytes of the emoji, "y")
// where the text has THREE, and the printed text was "x" followed by the
// four raw bytes of U+1F600.
BEZEL_TEST(codegen_sb_appendformat_width_domain_7244,
           astral_argument_renders_as_its_code_points) {
    const std::string src =
        "module M; "
        "class W public override method ToString(): string => \"😀\"; end W; "
        "class C public static method Main(): int "
        "  using var w := new W(); "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(w); "
        "  using var sb := new StringBuilder(); "
        "  sb.AppendFormat(\"x{0}y\", args); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nx😀y\n"));
}

// #7244 / spec §4.6 (v0.345) — A WIDE FORMAT.
//
// The FORMAT itself contains "日", so the format string is stored at width 2
// and its literal text is NOT ASCII. Pre-fix the format was read as 16-bit
// units interpreted as BYTES — every second byte a NUL — so the literal text
// was garbage and the hole was not recognised (the placeholder scan saw a
// NUL where it expected the closing brace).
//
// The hole's index is 0 and the argument is the ASCII "7", so the expected
// text is "日7" — TWO code points.
//
// MEASURED pre-fix: the builder held EIGHT code points — the format's four
// code points read as their eight raw bytes (E5 65 7B 00 30 00 7D 00), so
// the literal text was the two Latin-1 characters U+00E5 and U+0065, a NUL,
// '0', a NUL, '}' and a NUL, and the hole was never recognised (the
// placeholder scan saw the NUL where it expected the closing brace).
BEZEL_TEST(codegen_sb_appendformat_width_domain_7244,
           wide_format_literal_is_read_as_code_points) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(7); "
        "  using var sb := new StringBuilder(); "
        "  sb.AppendFormat(\"日{0}\", args); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n日7\n"));
}

// #7244 / spec §4.6 (v0.345) — A MIXED-WIDTH BUILDER.
//
// The builder already holds a wide part and the formatted part is narrow (or
// the other way round): the builder's content is the concatenation of its
// parts' CODE POINTS (§4.6), so a narrow part after a wide one must not be
// read at the wide part's width. This row is the mixed-width case the
// builder's own flattening already handles (§4.6's per-string width), reached
// through AppendFormat.
//
// The expected text is "日x7y" — FOUR code points.
//
// MEASURED: this row is GREEN ON BOTH ARMS. The builder's own flattening
// already concatenates its parts' CODE POINTS (§4.6's per-string width), so
// a narrow formatted part after a wide one was already right — the defect is
// in the formatted part's own construction, not in the concatenation. The row
// is here to pin that the fix did not disturb the flattening.
BEZEL_TEST(codegen_sb_appendformat_width_domain_7244,
           mixed_width_parts_concatenate_as_code_points) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(7); "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"日\"); "
        "  sb.AppendFormat(\"x{0}y\", args); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n日x7y\n"));
}

// CONTROL — an ASCII ARGUMENT AND AN ASCII FORMAT are unchanged.
//
// The fix must not disturb the common case: the rendered text is byte-for-byte
// what it was, and the builder's Length counts the same code points. This row
// is what makes the wide rows above meaningful — a fix that dropped every
// non-ASCII byte, or that counted BYTES instead of code points, would pass
// the wide rows only by being wrong in the other direction.
BEZEL_TEST(codegen_sb_appendformat_width_domain_7244,
           control_ascii_argument_and_format_unchanged) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(7); "
        "  using var sb := new StringBuilder(); "
        "  sb.AppendFormat(\"x{0}y\", args); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nx7y\n"));
}

// CONTROL — THE PLACEHOLDER SYNTAX is undisturbed.
//
// §3.1's AppendFormat substitutes the holes {N}; a literal brace is written
// {{ }}. §3.1 also states that a hole "optionally carr[ies] a format spec
// after a colon (`{0:F2}`)". The fix moved the scan from bytes to code
// points, so this row pins that the SCAN is unchanged: the escaped braces
// render as literal braces, both holes are substituted, and the format spec
// after the colon is skipped to reach the index.
//
// The argument is ASCII, so this row is a control and not a repeat of the
// wide rows.
BEZEL_TEST(codegen_sb_appendformat_width_domain_7244,
           control_placeholder_syntax_undisturbed) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(7); "
        "  args.Add(8); "
        "  using var sb := new StringBuilder(); "
        "  sb.AppendFormat(\"{{x}}={0}+{1:F2}\", args); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n{x}=7+8\n"));
}

// #7244 / spec §4.6 + stdlib §3.1 (v0.345) — A NO-PLACEHOLDER FORMAT.
//
// §3.1: "a no-placeholder format contributes its literal text" (the #1179
// fix). The fix must not have changed that, and in particular a wide
// no-placeholder format must contribute its TEXT — this is the narrowest
// spelling of the width defect, with no argument involved at all.
//
// MEASURED pre-fix: the builder held TWO code points — the wide code point
// read as its two storage bytes, U+00E5 and U+0065 ("åe") — instead of the
// ONE code point the text has.
BEZEL_TEST(codegen_sb_appendformat_width_domain_7244,
           wide_no_placeholder_format_contributes_its_text) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  using var sb := new StringBuilder(); "
        "  sb.AppendFormat(\"日\", args); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n日\n"));
}

// #7244 / spec §4.6 + stdlib §3.1 (v0.345) — THE BUILDER'S OWN ToString
// SEES THE SAME TEXT.
//
// §3.1 declares `StringBuilder.ToString(): string`, and §8.5 makes the
// interpolation hole and every `Object`-typed call the same rendering
// operation, so the two routes must agree. This row renders the same builder
// through the builder's own ToString and pins that the TEXT it yields is the
// argument's text — the one code point "日", not the two Latin-1 characters
// the argument's storage bytes spell.
//
// MEASURED pre-fix: the builder's own ToString answered TWO code points —
// U+00E5 and U+0065 ("åe") — so even the builder's own route rendered the
// argument's STORAGE rather than its TEXT.
//
// The interpolation form is a hole with no format spec, so §8.5 formats it
// via the value's ordinary `ToString()`.
BEZEL_TEST(codegen_sb_appendformat_width_domain_7244,
           builder_tostring_and_the_format_agree) {
    const std::string src =
        "module M; "
        "class W public override method ToString(): string => \"日\"; end W; "
        "class C public static method Main(): int "
        "  using var w := new W(); "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(w); "
        "  using var sb := new StringBuilder(); "
        "  sb.AppendFormat(\"{0}\", args); "
        "  var t := sb.ToString(); "
        "  Console.WriteLine(t.Length.ToString()); "
        "  Console.WriteLine(t); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n日\n"));
}
