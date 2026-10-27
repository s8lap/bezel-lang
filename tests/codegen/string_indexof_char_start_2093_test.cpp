// Spec-derived tests for fix #2093 (proposal 0062 item 2) — the additive
// `string.IndexOf(value: char, startIndex: int)` overload.
// Authority: stdlib v0.215 §9.6 (String section),
// `IndexOf(value: char, startIndex: int)` (stdlib §9.6; proposal 0062 item 2).
// The stdlib pre-v0.215 ships only the 1-arg
// `IndexOf(char)` form; the 2-arg char form is the additive overload from
// proposal 0062 item 2, parallel to the pre-existing
// `IndexOf(value: string, startIndex: int)` form (stdlib §9.6, #649).
//
// Contract pinned here (mirroring the documented IndexOf(string, startIndex)
// contract):
//   * `startIndex` is a code-point index (`§4.6` adaptive-width strings).
//   * `0 <= startIndex <= Length` (IndexOutOfRangeException otherwise).
//   * Returns the first code-point index >= startIndex where the code point
//     equals `value`, or `-1` if none.
//   * Found at exactly `Length` is valid and returns `Length` (the same range
//     semantics as the pre-existing string overload).
//
// Bezel syntax: inequality `<>` (§17.3); `IndexOutOfRangeException` is the
// stdlib-defined fault for the same bounds class as `Substring` (#747), and an
// uncaught raise exits non-zero — so raises are pinned in-source with `try ...
// catch ... end`.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── basic correctness: first-occurrence forward from a positive startIndex ───

BEZEL_TEST(string_indexof_char_start_2093, found_after_start) {
    // per stdlib §4.6 v0.215 / proposal 0062 item 2: scanning from index 1 of
    // "abacabad" for 'a' must find the 'a' at index 2, not at index 0.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"abacabad\"; return s.IndexOf('a', 1); "
        "end Main; end C;"), 2);
}

BEZEL_TEST(string_indexof_char_start_2093, found_at_exact_start) {
    // per stdlib §4.6 v0.215: when the needle sits exactly at startIndex, the
    // overload must return startIndex (not search forward past it).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"banana\"; return s.IndexOf('n', 2); "
        "end Main; end C;"), 2);
}

BEZEL_TEST(string_indexof_char_start_2093, not_found_after_start) {
    // per stdlib §4.6 v0.215: no match at or after startIndex → -1.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var s := \"abc\"; "
        "  Console.WriteLine(s.IndexOf('x', 0)); "
        "  Console.WriteLine(s.IndexOf('a', 1)); "
        "  return 0; "
        "end Main; end C;"), "-1\n-1");
}

BEZEL_TEST(string_indexof_char_start_2093, start_zero_matches_one_arg) {
    // per stdlib §4.6 v0.215: scanning with startIndex = 0 must match the
    // existing 1-arg `IndexOf(char)` form exactly. (Pins equivalence with the
    // documented single-arg overload — the new 2-arg form must NOT alter
    // semantics at startIndex = 0.)
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return s.IndexOf('o', 0); "
        "end Main; end C;"), 4);
}

BEZEL_TEST(string_indexof_char_start_2093, multibyte_start_keeps_codepoint_index) {
    // per stdlib §4.6 v0.215 / spec §4.6: `startIndex` is a CODE-POINT index,
    // not a byte offset. The string "aébéc" is 5 code points long
    // (indices 0..4): a(0), é(1), b(2), é(3), c(4). The 2-arg `IndexOf` from
    // code-point index 2 must skip the 'a' (cp 0) and first 'é' (cp 1) and
    // land on the second 'é' at code-point index 3 — i.e. cp 2 ('b') is
    // examined, then cp 3 ('é') matches. A byte-offset implementation would
    // return 4 instead (the byte-index for the second é within UTF-8 storage).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"a\\u00e9b\\u00e9c\"; "
        "  return s.IndexOf('\\u00e9', 2); "
        "end Main; end C;"), 3);
}

// ── bounds contract: out-of-range startIndex → IndexOutOfRangeException ───────

BEZEL_TEST(string_indexof_char_start_2093, negative_start_raises) {
    // per stdlib §4.6 v0.215: startIndex < 0 raises IndexOutOfRangeException
    // (same bounds class as `Substring` / `string[startIndex]`).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    var s := \"abc\"; "
        "    _ := s.IndexOf('a', -1); "
        "    return 0; "
        "  catch IndexOutOfRangeException => return 7; "
        "  end; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(string_indexof_char_start_2093, start_beyond_length_raises) {
    // per stdlib §4.6 v0.215: startIndex > Length raises
    // IndexOutOfRangeException. (startIndex == Length is the empty-tail case
    // and is valid; startIndex == Length + 1 is out of range — distinct from
    // the 1-arg form which has no bounds to check.)
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    var s := \"abc\"; "
        "    _ := s.IndexOf('a', 4); "
        "    return 0; "
        "  catch IndexOutOfRangeException => return 9; "
        "  end; "
        "end Main; end C;"), 9);
}

BEZEL_TEST(string_indexof_char_start_2093, start_equal_length_returns_minus_one) {
    // per stdlib §4.6 v0.215: startIndex == Length is in-range; the scan
    // immediately exhausts and returns -1. (Pin: this is the same
    // empty-tail semantics as `Substring(start).Length == 0`.)
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var s := \"abc\"; "
        "  Console.WriteLine(s.IndexOf('a', 3)); "
        "  return 0; "
        "end Main; end C;"), "-1");
}

// ── 1-arg form is unchanged (regression guard for the dispatcher) ────────────

BEZEL_TEST(string_indexof_char_start_2093, one_arg_form_unchanged) {
    // Regression guard: the new 2-arg codegen path must NOT short-circuit the
    // existing 1-arg `IndexOf(char)` form. Found case.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return \"Hello World\".IndexOf('o'); "
        "end Main; end C;"), 4);
}

BEZEL_TEST(string_indexof_char_start_2093, one_arg_not_found_unchanged) {
    // Regression guard: 1-arg form returns -1 when not found.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(\"abc\".IndexOf('z')); return 0; "
        "end Main; end C;"), "-1");
}
