// Regression tests for #7244 — `StringBuilder.AppendFormat` SEGFAULTED when
// one of its `args` was a `string`.
//
// THE DEFECT.
//
//   `args` is an `Array<Object>`, and an `Object` slot holds a value in one of
//   TWO representations (spec §8.6 / §12.7, v0.350): a class instance / a
//   boxed value / a built-in exception carries a VTABLE at word 0, and a
//   `string` carries its DATA pointer at word 0. `bz_sb_append_format`
//   (src/runtime/bezel_runtime_strings.cpp) read word 0 as a vtable
//   unconditionally and called slot 3 (ToString) through it, so a `string`
//   argument made the generated program JUMP INTO THE STRING'S OWN BYTES — a
//   wild call through a data pointer.
//
//   MEASURED (the issue's own repro, the tip `bd1f8788`):
//
//     $ ./p06_af_str
//     Segmentation fault          (exit 139)
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   stdlib §3.1 (v0.350), verbatim:
//     "public method AppendFormat(format: string, args: Array<Object>): StringBuilder;"
//
//   and its Contracts clause (stdlib §3.1, v0.350), verbatim:
//     "`AppendFormat`'s format string contains literal text and holes `{n}` —
//      `n` a zero-based index into `args` — each hole optionally carrying a
//      format spec after a colon (`{0:F2}`) …"
//
//   ⇒⇒ AppendFormat substitutes THE STRING FORM of `args[N]` — the very text
//   `args[N].ToString()` produces. And spec §8.5 (v0.350) makes that
//   operation one: "a hole without a format spec formats via the value's
//   ordinary `ToString()`".
//
//   spec §9.6 (v0.350): a `string`'s `ToString()` is the IDENTITY — its own
//   text. So a `string` argument's string form is the string itself, and the
//   two routes MUST agree: `args[0].ToString()` and
//   `AppendFormat("{0}", args)` render the same text. (MEASURED on the tip,
//   BEFORE the fix: `args[0].ToString()` printed "hello" and its length 5,
//   while `AppendFormat("x{0}y", args)` SEGFAULTED — the two routes of one
//   operation disagreed, one of them fatally.)
//
//   spec §4.6 (v0.350), on the WIDTH domain (the OTHER half of #7244, already
//   repaired on the tip by e44ba7aa and pinned by
//   tests/codegen/sb_appendformat_width_domain_7244_test.cpp):
//     "an implementation is expected to choose a per-string storage width —
//      typically 1, 2, or 4 bytes per code point, selected at construction
//      based on the largest code point in the value … The choice of width is
//      invisible at the language level; programs observe only the `char`
//      sequence and its length."
//
//   ⇒⇒ A string's DATA is not its TEXT. At width 2, one code point is two
//   bytes, and reading those two bytes as two characters yields two DIFFERENT
//   characters. The rows below therefore pin the formatted text in the
//   CODE-POINT domain: a WIDE string argument contributes ONE code point,
//   not its two storage bytes.
//
// THE ORACLE is on stdout, byte for byte. The defect is a CRASH, so "the
// program ran" is the first oracle; but "the program ran" alone would accept a
// fix that renders the WRONG text, so every row also asserts the printed
// text (and the builder's LENGTH, a count of code points, §4.6).
//
// The LEAK oracle: `runAotChecked` compiles the generated module with
// AddressSanitizer and runs it under LSan, so a program that leaks fails the
// row. Every program here binds its owned values with `using var` (§28.3) and
// is clean on both arms, so the leak oracle does not mask the value oracle.
//
// ONE ROW USES THE PLAIN ORACLE, and says why: the issue's own repro binds
// its `Array<Object>` to a plain `var` local built from an ARRAY LITERAL, and
// that array leaks its 208-byte allocation when it is passed as AppendFormat's
// `args` (MEASURED under ASan/LSan: "SUMMARY: AddressSanitizer: 208 byte(s)
// leaked in 2 allocation(s)" — the array's header + backing store). The leak
// is NOT #7244's: it reproduces with a CLASS argument too (the path this fix
// does not touch), it does NOT reproduce with `using var arr` or with
// `new Array<Object>()`, and it is a disposal defect of its own — reported to
// the coordinator. The row therefore uses `runAot` (the plain AOT oracle),
// whose exit code and stdout are the value oracle; every other row uses the
// ASan/LSan oracle.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

// #7244 / spec §8.6 / §12.7 / §8.5 (v0.350) — THE ISSUE'S OWN REPRO.
//
// `args` is an `Array<Object>` holding a `string`, and `AppendFormat` used to
// read that string's DATA pointer as a vtable and call slot 3 through it: a
// wild call into the string's own bytes, i.e. a SEGFAULT.
//
// MEASURED on the tip `bd1f8788` BEFORE the fix: exit 139, no output at all.
// AFTER the fix: exit 0, and the builder holds "xhelloy" — SEVEN code points
// ("x" + "hello" + "y"), so the Length and the content's length agree.
//
// This row is the issue's repro VERBATIM (`var arr: Array<Object> := [ … ]`),
// so it uses the plain AOT oracle — see the file comment for the measured
// reason (that array literal leaks 208 B under LSan, a defect of its own).
BEZEL_TEST(codegen_sb_appendformat_string_argument_7244,
           appendformat_with_a_string_argument_does_not_crash) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  var arr: Array<Object> := [ \"hello\" ]; "
        "  sb.AppendFormat(\"x{0}y\", arr); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString().Length.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n7\n"));
}

// #7244 / spec §8.6 / §12.7 / §9.6 (v0.350) — THE PRINTED TEXT.
//
// The crash row above proves the program ran; this row proves it printed the
// RIGHT thing. A `string` argument's string form is the string ITSELF
// (§9.6: a string's ToString is the identity), so the formatted text is
// "x" + "hello" + "y" = SEVEN code points.
//
// The oracle is the ASan/LSan leg (`runAotChecked`): the program must run
// clean under the sanitizer, so the fix cannot have traded the crash for a
// memory error (e.g. releasing a string the array still owns).
BEZEL_TEST(codegen_sb_appendformat_string_argument_7244,
           appendformat_with_a_string_argument_prints_the_formatted_text) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(\"hello\"); "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\nxhelloy\n"));
}

// #7244 / spec §4.6 (v0.350) — THE CODE-POINT DOMAIN, ON A STRING ARGUMENT.
//
// "日" is U+65E5: ONE code point, stored at WIDTH 2 (§4.6: the width is
// chosen per string from the largest code point in the value). The formatted
// text is "x" + "日" + "y" — THREE code points.
//
// The width-domain half of #7244 reads a string's TEXT, never its storage
// bytes. A fix that rendered the argument's two storage bytes would print
// FOUR code points ("x", U+00E5, U+0065, "y") and the text "xåey" — the
// byte-level flattening the issue names. This row is therefore the width
// oracle for the STRING path specifically (the width-domain file's own rows
// reach the width defect through a CLASS argument, which this fix does not
// touch).
//
// MEASURED on the tip BEFORE the fix: exit 139 (the crash), so the row is red
// for the crash AND for the value it pins.
BEZEL_TEST(codegen_sb_appendformat_string_argument_7244,
           appendformat_code_point_domain) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(\"日\"); "
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

// #7244 / spec §4.6 (v0.350) — A MIXED-WIDTH BUILDER.
//
// The builder's parts are STRINGS, each with its OWN per-string width (§4.6),
// so the builder's content is NOT the concatenation of the parts' BYTES: a
// width-1 part's byte and a width-2 part's two bytes are different code
// points, and a byte-level concatenation of parts with DIFFERENT widths is not
// a code-point concatenation at all.
//
// This row builds exactly that: a WIDTH-1 part ("a", appended first) and a
// WIDTH-2 part ("[日]", the formatted part — its largest code point is
// U+65E5, so §4.6's own rule packs it at width 2). The builder's content is
// the concatenation of the parts' CODE POINTS: "a" + "[" + "日" + "]" — FOUR
// code points.
//
// MEASURED on the tip BEFORE the fix: exit 139 (the crash).
BEZEL_TEST(codegen_sb_appendformat_string_argument_7244,
           appendformat_with_a_mixed_width_builder) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(\"日\"); "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"a\"); "
        "  sb.AppendFormat(\"[{0}]\", args); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\na[日]\n"));
}

// CONTROL — THE BOXED-OBJECT PATH IS UNCHANGED.
//
// The vtable-bearing arm is what the helper was written for: a class
// instance's word 0 IS a vtable, and slot 3 is its ToString. The fix adds a
// branch BEFORE it; it must not disturb it. The class renders "W", so the
// formatted text is "x" + "W" + "y" — THREE code points.
//
// This row is the positive control for the whole fix: without it, a fix that
// answered "string" for EVERY argument (e.g. one that never dispatched the
// vtable at all) would pass the rows above.
//
// MEASURED on the tip BEFORE the fix: exit 0 and this same output — the
// control is GREEN on both arms, as a control must be.
BEZEL_TEST(codegen_sb_appendformat_string_argument_7244,
           appendformat_with_an_object_argument_is_unchanged) {
    const std::string src =
        "module M; "
        "class W public override method ToString(): string => \"W\"; end W; "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nxWy\n"));
}

// CONTROL — NO ARGUMENTS AT ALL.
//
// The degenerate form: an EMPTY `Array<Object>` and a format with no hole.
// §3.1: "a no-placeholder format contributes its literal text" (the #1179
// fix). The text is "plain" — FIVE code points.
//
// MEASURED on the tip BEFORE the fix: exit 0 and this same output — GREEN on
// both arms, as a control must be.
//
// NOT PINNED HERE: a hole whose index is OUTSIDE `args`. The runtime renders
// it as the empty string, while stdlib §3.1's Contracts clause says such a
// hole "raises FormatException". That gap is a SEPARATE, PRE-EXISTING defect
// (it is the same for a class argument, and it is not #7244's shape), so this
// file does not pin the current behaviour of it — a row that froze it would
// contradict the stdlib.
BEZEL_TEST(codegen_sb_appendformat_string_argument_7244,
           appendformat_with_no_arguments_is_unchanged) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  using var sb := new StringBuilder(); "
        "  sb.AppendFormat(\"plain\", args); "
        "  Console.WriteLine(sb.Length.ToString()); "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\nplain\n"));
}

// #7244 / stdlib §3.1 + spec §8.5 (v0.350) — THE TWO ROUTES AGREE.
//
// stdlib §3.1 says AppendFormat substitutes "the string form of args[N]", and
// spec §8.5 makes that the value's ordinary `ToString()`. So the formatted
// text and `args[N].ToString()` are the SAME operation and MUST produce the
// same text.
//
// This row prints both: `args[0].ToString()` first ("hi"), then the builder's
// content ("[hi]"). Before the fix the first route printed "hi" and the
// second SEGFAULTED.
//
// The row is what makes the fix's SHAPE checkable rather than merely
// crash-free: it pins that AppendFormat's argument rendering is the
// `Object.ToString()` rendering, not a second, drifting spelling of it.
BEZEL_TEST(codegen_sb_appendformat_string_argument_7244,
           appendformat_matches_the_arguments_own_tostring) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  using var args: Array<Object> := new Array<Object>(); "
        "  args.Add(\"hi\"); "
        "  Console.WriteLine(args[0].ToString()); "
        "  using var sb := new StringBuilder(); "
        "  sb.AppendFormat(\"[{0}]\", args); "
        "  Console.WriteLine(sb.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("hi\n[hi]\n"));
}
