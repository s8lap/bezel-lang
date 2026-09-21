// Codegen — fix #4545 (spec §4.6/§18.5/§18.8, v0.306): a non-ASCII LITERAL
// segment of an interpolated string (`$"…literal text…"`) rendered as its low
// storage bytes instead of its code points. A hole substituting the identical
// text, and the same text in a plain (non-interpolated) string literal, were
// both correct — the defect was confined to how `$"…"` lowers its own literal
// segments.
//
// THE CONTRACT. Spec §4.6 (v0.306): a string's internal storage width (1, 2,
// or 4 bytes per code point, chosen at construction from the largest code
// point present) "is invisible at the language level; programs observe only
// the `char` sequence and its length." Spec §18.5 says a `$"…"` literal
// segment is ordinary source text between holes; §18.8 defines the escape
// sequences that same text is decoded through. Nothing distinguishes a
// literal segment's text from a plain string literal's text — they must
// render identically for identical source characters.
//
// THE BUG. `InterpolatedStringExpr` lowering (LlvmBackend_Exprs.cpp, the
// `!part.literal.empty()` arm) built its own global instead of going through
// the shared `lowerStringLiteral` plain-literal path. It ran the segment's
// text through `processStringText`, which (LlvmBackend_String.cpp:375-381)
// decodes UTF-8 to code points via `processStringCodePoints` and then
// TRUNCATES EVERY CODE POINT TO ITS LOW BYTE (`(char)(cp & 0xFF)`) before
// handing back a `std::string` — a byte-mode-only helper meant for contexts
// that already know their text is ASCII/Latin-1. The literal-part arm then
// stamped the result at a HARD-CODED width of 1 regardless of what it
// actually held. This is the #4499/#4487 "code-point count used in place of
// a byte/width decision" defect class, on the one path those fixes did not
// touch (the issue's own framing).
//
// Measured on HEAD `6c05b4cf` (`scripts/agent_check.sh --probe --asan`), the
// issue's own repro:
//
//     var s := "日本";
//     Console.WriteLine($"lit=日本 hole={s}");
//
//     -> "lit=å, hole=日本"
//
// U+65E5 (日) truncates to its low byte 0xE5 = U+00E5 (å, Latin-1); U+672C
// (本) truncates to its low byte 0x2C = U+002C (,). The hole (`{s}`, which
// goes through the ordinary string-value path, not this one) renders 日本
// correctly, and so does the SAME two characters as a plain "日本" literal —
// confirming the defect is specific to the `$"…"` literal-segment path.
//
// THE FIX routes the literal segment through `lowerStringLiteral` (the exact
// helper plain `"…"`/`@"…"` literals already use, LlvmBackend_String.cpp:384)
// instead of hand-rolling a width-1 global: `processStringCodePoints` decodes
// the segment to real code points, `codePointsWidth` picks the narrowest
// width that fits the SEGMENT's widest code point (not a per-character
// choice — §4.6 is a per-STRING storage decision), and `makeCodePointArray`
// encodes the data at that width. A hole and a literal segment holding
// identical text now agree because they share one construction path.
//
// A byte-length-only fix (thread `content.size()` through unchanged) would
// have been unsound on its own: a width-2 "日" (U+65E5) and a width-1 "åe"
// share the bytes `E5 65`. Width must be carried by the value's own
// construction (as `lowerStringLiteral` does), not inferred from a byte
// count — the same lesson #4499 recorded for the dictionary string-key ABI.
//
// Every row below prints to stdout and asserts with `runAotOut` rather than
// returning a value through `Main`'s exit code: `runAot`'s exitCode is
// WEXITSTATUS-masked to 8 bits, and several of the numeric pins here
// (U+65E5 = 26085, U+1F389 = 127881) do not fit.
//
// FAIL-WITHOUT proven mechanically: with LlvmBackend_Exprs.cpp reverted to
// its pre-fix content (copied aside to /var/tmp/bezel-scratch, `git checkout
// --` the file, rebuilt bezel_tests_codegen) every non-ASCII-literal row
// below FAILED (rendering the truncated-low-byte text this comment
// describes); with the fix restored (verified byte-identical to the
// committed version), all rows PASS. The all-ASCII / hole-only rows answer
// identically on both sides and pin that the fix changes nothing else.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotOut — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// `body` is the statement sequence inside `Main`; it must Console.WriteLine
// its own observable(s) and end without an explicit early return.
std::string wrap(const std::string& body) {
    return "module M; public class C "
           "  public static method Main(): int "
           + body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// The issue's own repro: a width-2 (CJK) literal segment alongside a hole
// substituting the identical text. Pre-fix the literal half alone corrupted;
// the hole half was always correct, which is the whole reason the defect was
// confined to the literal-segment path and not the interpolation feature at
// large.
// ---------------------------------------------------------------------------
BEZEL_TEST(interp_literal_codepoint_4545, cjk_literal_alongside_hole_spec_4_6_18_5) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "    var s := \"日本\"; "
            "    Console.WriteLine($\"lit=日本 hole={s}\"); ")),
        "lit=日本 hole=日本");
}

// The same text as a NUMERIC pin (indexing the rendered result, not relying
// on the test file's own encoding matching stdout's). U+65E5 = 26085. §4.6:
// `s[i]` returns the code point at position i. Pre-fix this indexed the
// truncated-low-byte string, giving 0xE5 = 229.
BEZEL_TEST(interp_literal_codepoint_4545, cjk_literal_code_point_numeric_pin_spec_4_6) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "    var res := $\"lit=日本\"; "
            "    Console.WriteLine($\"cp0={int(res[4])} cp1={int(res[5])}\"); ")),
        "cp0=26085 cp1=26412");
}

// ---------------------------------------------------------------------------
// Width 4 (astral plane, e.g. emoji) — the widest storage width, and outside
// even the BMP that a naive 16-bit fix could have special-cased.
// U+1F389 (🎉) = 127881.
// ---------------------------------------------------------------------------
BEZEL_TEST(interp_literal_codepoint_4545, astral_literal_alongside_hole_spec_4_6) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "    var s := \"x\"; "
            "    Console.WriteLine($\"lit=🎉 hole={s}\"); ")),
        "lit=🎉 hole=x");
}

BEZEL_TEST(interp_literal_codepoint_4545, astral_literal_code_point_numeric_pin_spec_4_6) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "    var res := $\"🎉x\"; "
            "    Console.WriteLine($\"cp={int(res[0])}\"); ")),
        "cp=127881");
}

// ---------------------------------------------------------------------------
// A single literal segment mixing ASCII, Latin-1-range and CJK code points.
// §4.6's storage width is a per-STRING decision from the segment's WIDEST
// code point, not a per-character choice — this row would show corruption on
// the ASCII/Latin-1 members too if the fix picked width per character instead
// of once for the whole segment. "aÀ日b": a=U+0061, À=U+00C0, 日=U+65E5,
// b=U+0062.
// ---------------------------------------------------------------------------
BEZEL_TEST(interp_literal_codepoint_4545, mixed_width_single_segment_spec_4_6) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "    Console.WriteLine($\"lit=aÀ日b\"); ")),
        "lit=aÀ日b");
}

// ---------------------------------------------------------------------------
// Verbatim interpolated strings (`$@"…"`, §18.7) share the same literal-part
// arm with `isVerbatim=true` — the fix must not have broken the "no escape
// decoding" contract while fixing the width. `\n` must survive as the two
// literal characters backslash-n, not become a newline; the non-ASCII text
// beside it must still render correctly.
// ---------------------------------------------------------------------------
BEZEL_TEST(interp_literal_codepoint_4545, verbatim_literal_non_ascii_no_escape_decode_spec_18_7) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "    Console.WriteLine($@\"日本\\n end\"); ")),
        "日本\\n end");
}

// ---------------------------------------------------------------------------
// Literal-brace escaping (§18.5 "Literal braces": `{{`/`}}`) beside non-ASCII
// text in the same literal segment — the splitter's brace-unescaping and this
// fix's width-aware construction must compose.
// ---------------------------------------------------------------------------
BEZEL_TEST(interp_literal_codepoint_4545, escaped_braces_beside_non_ascii_spec_18_5) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "    Console.WriteLine($\"braces {{日}} lit\"); ")),
        "braces {日} lit");
}

// ---------------------------------------------------------------------------
// Regression pins: rows that answer identically before and after the fix,
// confirming it changed nothing else.
// ---------------------------------------------------------------------------

// Width-1 ASCII-only literal segments were never affected (processStringText's
// truncation is a no-op below U+0100) — unchanged behaviour.
BEZEL_TEST(interp_literal_codepoint_4545, ascii_only_literal_unaffected_spec_4_6) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "    var n := 42; "
            "    Console.WriteLine($\"n={n} ok\"); ")),
        "n=42 ok");
}

// A plain (non-interpolated) "日本" literal was always correct — the same
// text, same two lines, rendered through both paths must still agree.
BEZEL_TEST(interp_literal_codepoint_4545, plain_literal_still_correct_spec_4_6) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "    Console.WriteLine(\"日本\"); "
            "    Console.WriteLine($\"日本\"); ")),
        "日本\n日本");
}
