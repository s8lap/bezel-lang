// Codegen — fix #4420 (stdlib §9.5/§9.6, v0.306): char classification /
// simple-case-mapping used to lower directly onto <ctype.h> (isdigit,
// isalpha, isalnum, isupper, islower, ispunct, isspace, iscntrl, toupper,
// tolower), which C11 §7.4p1 makes undefined for any argument not
// representable as `unsigned char` (0-255) or `EOF`. Bezel's char is a
// 32-bit Unicode scalar up to U+10FFFF (spec §4.5), so any code point above
// U+00FF was an out-of-bounds libc table read — "an out-of-bounds read
// returning a plausible-looking bool, not merely a wrong one" (#4420). The
// same defect existed independently in string.IsNullOrWhiteSpace's
// per-code-point loop.
//
// Root cause verified on HEAD before the fix (manually, via
// scripts/agent_check.sh --probe --asan): char.IsDigit/IsLetter/IsUpper/
// IsLower/IsPunctuation/IsWhiteSpace/IsControl(char(0x65E5)) — U+65E5, "日",
// a CJK letter, general category Lo — ALL read back `true` (isdigit,
// isupper, islower and ispunct simultaneously "true" for the SAME code
// point is impossible for any real classification — direct evidence of
// garbage libc-table reads), while IsLetterOrDigit read back `false`
// (inconsistent with IsLetter=true — further evidence of garbage, not a
// real classification). Fixed, every one of those now deterministically
// reports `false` (the domain-safe, decidable default — see
// bezel_runtime_strings.cpp's bz_char_is_* comments for why: these six
// categories require full Unicode Character Database data the runtime does
// not embed, so above ASCII they report the safe default rather than
// guessing from an incomplete slice of a category table). IsControl (Cc)
// and IsWhiteSpace (the Unicode White_Space property) are each a small,
// closed, version-stable code-point list, so — unlike the other six — they
// are Unicode-EXACT across the full range, not just ASCII; those two get
// their own precision tests below, not just a "no longer UB" one.
//
// tests_below_intentionally_avoid two PRE-EXISTING, UNRELATED disposal gaps
// discovered while probing this fix (reported separately, not fixed here
// per issue scope): (1) the STATIC call spelling `char.ToString(cp)` (as
// opposed to the instance spelling `c.ToString()`) leaks its result on both
// pre-fix and post-fix code; (2) passing an owned (non-literal) `string`
// local into `string.IsNullOrWhiteSpace(s)` also leaks on both pre-fix and
// post-fix code. Neither is new here (reproduced identically before and
// after this change) and neither is a char-classification defect, so the
// tests below use ONLY the instance ToString spelling and ONLY string
// LITERAL arguments to IsNullOrWhiteSpace, which are unaffected.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// --- ASCII rows: prove the fix did not change the ASCII-domain answers ---
// (stdlib §9.5, v0.306: within ASCII, libc's "C"-locale classification IS
// the Unicode general-category answer, so these libc-backed rows are
// unchanged by the fix — routed through bz_char_is_* now instead of raw
// ctype, but computing the identical result.)
BEZEL_TEST(char_classification_unicode_4420, ascii_predicates_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: bool := char.IsDigit('5'); "            // true
        "  var l: bool := char.IsLetter('a'); "            // true
        "  var ld: bool := char.IsLetterOrDigit('9'); "    // true
        "  var u: bool := char.IsUpper('Z'); "              // true
        "  var lo: bool := char.IsLower('z'); "             // true
        "  var p: bool := char.IsPunctuation('.'); "        // true
        "  var ws: bool := char.IsWhiteSpace(' '); "        // true
        "  var ct: bool := char.IsControl('\\n'); "         // true
        "  return (if d then 1 else 0) + (if l then 2 else 0) + "
        "         (if ld then 4 else 0) + (if u then 8 else 0) + "
        "         (if lo then 16 else 0) + (if p then 32 else 0) + "
        "         (if ws then 64 else 0) + (if ct then 128 else 0); "
        "end Main; end C;"), 255);
}

BEZEL_TEST(char_classification_unicode_4420, ascii_negatives_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: bool := char.IsDigit('a'); "              // false
        "  var l: bool := char.IsLetter('5'); "              // false
        "  var u: bool := char.IsUpper('z'); "                // false
        "  var lo: bool := char.IsLower('Z'); "               // false
        "  var p: bool := char.IsPunctuation('a'); "          // false
        "  var ws: bool := char.IsWhiteSpace('a'); "          // false
        "  var ct: bool := char.IsControl('a'); "             // false
        "  return (if d then 1 else 0) + (if l then 1 else 0) + "
        "         (if u then 1 else 0) + (if lo then 1 else 0) + "
        "         (if p then 1 else 0) + (if ws then 1 else 0) + "
        "         (if ct then 1 else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(char_classification_unicode_4420, ascii_toupper_tolower_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var up: char := char.ToUpper('a'); "
        "  var lo: char := char.ToLower('A'); "
        "  var upNoop: char := char.ToUpper('!'); "  // no case mapping: unchanged
        "  return (if up = 'A' then 1 else 0) + (if lo = 'a' then 2 else 0) + "
        "         (if upNoop = '!' then 4 else 0); "
        "end Main; end C;"), 7);
}

// --- The #4420 regression proper: above U+00FF, no more UB ---
// Pre-fix (verified via scripts/agent_check.sh --probe --asan on HEAD before
// this commit, reverting only this fix): every row below read back `true`
// via a raw, out-of-bounds isdigit/isalpha/isupper/islower/ispunct(0x65E5)
// call. Post-fix each is exactly `false` — the deterministic, decidable
// default for categories that need full Unicode Character Database data the
// runtime does not embed (stdlib §9.5, v0.306; #4420: "do not close this by
// widening a category table").
BEZEL_TEST(char_classification_unicode_4420, above_ascii_predicates_are_false_not_ub) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var hi: char := char(26085); "  // U+65E5 "日" (CJK letter, Lo)
        "  var d: bool := char.IsDigit(hi); "
        "  var l: bool := char.IsLetter(hi); "
        "  var ld: bool := char.IsLetterOrDigit(hi); "
        "  var u: bool := char.IsUpper(hi); "
        "  var lo: bool := char.IsLower(hi); "
        "  var p: bool := char.IsPunctuation(hi); "
        "  return (if d then 1 else 0) + (if l then 1 else 0) + "
        "         (if ld then 1 else 0) + (if u then 1 else 0) + "
        "         (if lo then 1 else 0) + (if p then 1 else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(char_classification_unicode_4420, above_ascii_astral_predicates_are_false_not_ub) {
    // U+1F600 GRINNING FACE — well above the BMP; the pre-fix libc call's
    // out-of-bounds offset here is over 500 KB past the ctype table.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var hi: char := char(128512); "
        "  var l: bool := char.IsLetter(hi); "
        "  var d: bool := char.IsDigit(hi); "
        "  var ws: bool := char.IsWhiteSpace(hi); "
        "  return (if l then 1 else 0) + (if d then 1 else 0) + (if ws then 1 else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(char_classification_unicode_4420, above_ascii_toupper_tolower_unchanged_not_ub) {
    // §9.5 (v0.306): ToUpper/ToLower "return the argument unchanged where it
    // has [no case mapping]" — applied here to the undecidable-above-ASCII
    // range rather than guessing.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var hi: char := char(26085); "
        "  var up: char := char.ToUpper(hi); "
        "  var lo: char := char.ToLower(hi); "
        "  return (if up = hi then 1 else 0) + (if lo = hi then 2 else 0); "
        "end Main; end C;"), 3);
}

// --- IsControl (Cc): a closed, version-stable set, exact across the FULL
// char domain, not just ASCII (unlike the six predicates above). The C1
// control block U+0080-U+009F is Cc; U+00A0 (the very next code point,
// NO-BREAK SPACE) is Zs and must NOT be classified control. ---
BEZEL_TEST(char_classification_unicode_4420, iscontrol_c1_block_exact) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var nel: bool := char.IsControl(char(133)); "     // U+0085 NEL -> true
        "  var last: bool := char.IsControl(char(159)); "    // U+009F -> true
        "  var nbsp: bool := char.IsControl(char(160)); "    // U+00A0 -> false (Zs)
        "  return (if nel then 1 else 0) + (if last then 2 else 0) + "
        "         (if not nbsp then 4 else 0); "
        "end Main; end C;"), 7);
}

// --- IsWhiteSpace (Unicode White_Space property): also a closed,
// version-stable set, exact across the full char domain. ---
BEZEL_TEST(char_classification_unicode_4420, iswhitespace_property_exact) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var nbsp: bool := char.IsWhiteSpace(char(160)); "    // U+00A0 -> true
        "  var ideo: bool := char.IsWhiteSpace(char(12288)); "  // U+3000 -> true
        "  var letter: bool := char.IsWhiteSpace(char(26085)); " // U+65E5 -> false
        "  return (if nbsp then 1 else 0) + (if ideo then 2 else 0) + "
        "         (if not letter then 4 else 0); "
        "end Main; end C;"), 7);
}

// --- string.IsNullOrWhiteSpace's independent site (stdlib §9.6, v0.306):
// the second location the issue names (LlvmBackend_Calls2.cpp's per-code-
// point loop) used the SAME raw isspace(cp) call. Routed through the same
// bz_char_is_white_space helper, so a wide (width>1) string's high code
// points are now Unicode-White_Space-exact instead of UB. Uses only string
// LITERAL arguments (see file header: a non-literal owned `string` local
// passed to this method has a pre-existing, unrelated disposal leak that
// this fix neither introduces nor is scoped to repair). ---
BEZEL_TEST(char_classification_unicode_4420, string_isnullorwhitespace_unicode_exact) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var nbspOnly: bool := string.IsNullOrWhiteSpace(\"\xC2\xA0\"); "      // U+00A0 -> true
        "  var letterOnly: bool := string.IsNullOrWhiteSpace(\"\xE6\x97\xA5\"); " // U+65E5 "日" -> false
        "  var asciiSpaces: bool := string.IsNullOrWhiteSpace(\"   \"); "         // unchanged
        "  return (if nbspOnly then 1 else 0) + (if not letterOnly then 2 else 0) + "
        "         (if asciiSpaces then 4 else 0); "
        "end Main; end C;"), 7);
}
