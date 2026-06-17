// CG27f: stdlib §9.6 string methods — StartsWith, EndsWith, Contains,
// IndexOf, Substring. Implemented as compiler intrinsics using libc
// memcmp / memmem / malloc+memcpy under the hood.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// §9.6: StartsWith(prefix): bool
BEZEL_TEST(codegen_cg27f, starts_with_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.StartsWith(\"Hello\") else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg27f, starts_with_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.StartsWith(\"World\") else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_cg27f, starts_with_empty_prefix) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"abc\"; "
        "  return (1 if s.StartsWith(\"\") else 0); "
        "end Main; end C;"), 1);
}

// §9.6: EndsWith(suffix): bool
BEZEL_TEST(codegen_cg27f, ends_with_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.EndsWith(\"World\") else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg27f, ends_with_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.EndsWith(\"Hello\") else 0); "
        "end Main; end C;"), 0);
}

// §9.6: Contains(sub): bool
BEZEL_TEST(codegen_cg27f, contains_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.Contains(\"lo W\") else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg27f, contains_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return (1 if s.Contains(\"xyz\") else 0); "
        "end Main; end C;"), 0);
}

// §9.6: IndexOf(sub): int — first occurrence index, -1 if not found.
BEZEL_TEST(codegen_cg27f, index_of_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; return s.IndexOf(\"World\"); "
        "end Main; end C;"), 6);
}

BEZEL_TEST(codegen_cg27f, index_of_not_found) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; return s.IndexOf(\"xyz\"); "
        "end Main; end C;"), -1);
}

BEZEL_TEST(codegen_cg27f, index_of_start) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"abc\"; return s.IndexOf(\"a\"); "
        "end Main; end C;"), 0);
}

// §9.6: Substring(start): string — from start to end.
BEZEL_TEST(codegen_cg27f, substring_from_offset) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; var t := s.Substring(6); "
        "  return (1 if t = \"World\" else 0); "
        "end Main; end C;"), 1);
}

// §9.6: Substring(start, length): string — slice of given length.
BEZEL_TEST(codegen_cg27f, substring_with_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; var t := s.Substring(0, 5); "
        "  return (1 if t = \"Hello\" else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg27f, substring_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; var t := s.Substring(6); "
        "  return t.Length; "
        "end Main; end C;"), 5);
}

// fix #747 R43 / §9.6 / language spec §4.6: an out-of-range Substring index or
// length raises IndexOutOfRangeException (the runtime previously silently
// clamped). All exceptions are caught so the JIT runner never aborts.

// start past the end, a negative start, and a too-long (start, length) each
// throw — three caught OOB cases leave the counter at 0.
BEZEL_TEST(codegen_cg27f, substring_out_of_range_throws_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; var fails := 0; "
        "  try var a := s.Substring(6); fails := fails + 1; "
        "  catch IndexOutOfRangeException => fails := fails + 0; end; "
        "  try var b := s.Substring(-1); fails := fails + 1; "
        "  catch IndexOutOfRangeException => fails := fails + 0; end; "
        "  try var c := s.Substring(3, 3); fails := fails + 1; "
        "  catch IndexOutOfRangeException => fails := fails + 0; end; "
        "  return fails; "  // 0 — every OOB call threw
        "end Main; end C;"), 0);
}

// Boundary cases that are VALID must NOT throw: start == len (empty tail),
// length 0, and an exact (0, len) slice. 1+2+4+8+16 = 31.
BEZEL_TEST(codegen_cg27f, substring_valid_boundaries_no_throw_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; var ok := 0; "
        "  if s.Substring(5).Length = 0 then ok := ok + 1; end; "
        "  if s.Substring(5, 0).Length = 0 then ok := ok + 2; end; "
        "  if s.Substring(0, 0).Length = 0 then ok := ok + 4; end; "
        "  if s.Substring(2).Length = 3 then ok := ok + 8; end; "
        "  if s.Substring(0, 5).Length = 5 then ok := ok + 16; end; "
        "  return ok; "  // 31
        "end Main; end C;"), 31);
}

// Bounds are in CODE POINTS (#535): a code-point-OOB start throws.
BEZEL_TEST(codegen_cg27f, substring_codepoint_out_of_range_throws_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello \xE6\x97\xA5\xE6\x9C\xAC World\"; "  // 14 code points
        "  try var x := s.Substring(99); return 1; "
        "  catch IndexOutOfRangeException => return 0; end; "  // 0 — threw
        "end Main; end C;"), 0);
}

// -------------------------------------------------------------------------
// §9.6 Split(separator: char): owned Array<string> — split on a single code
// point. The char overload routes through bz_string_split_anychar with a
// one-element separator set; the string-delimiter overload (below) routes
// through bz_string_split.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_string_split_char_spec_9_6, three_parts) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p: Array<string> := \"a,b,c\".Split(','); return p.Count; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(codegen_string_split_char_spec_9_6, part_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p: Array<string> := \"aa,bb,cc\".Split(','); "
        "  return (1 if p[1] = \"bb\" else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_string_split_char_spec_9_6, empty_segment_between_separators) {
    // Two adjacent separators yield an empty segment (no coalescing), matching
    // the string-delimiter Split.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p: Array<string> := \"a,,b\".Split(','); "
        "  return (1 if (p.Count = 3 and p[1] = \"\") else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_string_split_char_spec_9_6, no_separator_whole_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p: Array<string> := \"abc\".Split(','); "
        "  return (1 if (p.Count = 1 and p[0] = \"abc\") else 0); "
        "end Main; end C;"), 1);
}

// -------------------------------------------------------------------------
// §9.6 Split(separator: string): owned Array<string> — split on a substring
// delimiter. Routes through bz_string_split. Regression: the call previously
// emitted malformed IR (declared bz_string_split with 5 params while calling
// it with 4) — verifiable only with BEZEL_VERIFY_IR, and untested at runtime.
// These exercise the multi-char delimiter end-to-end through the JIT.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_string_split_str_spec_9_6, multichar_delim_three_parts) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p: Array<string> := \"a::b::c\".Split(\"::\"); return p.Count; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(codegen_string_split_str_spec_9_6, multichar_delim_part_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p: Array<string> := \"aa::bb::cc\".Split(\"::\"); "
        "  return (1 if (p[0] = \"aa\" and p[1] = \"bb\" and p[2] = \"cc\") else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_string_split_str_spec_9_6, multichar_delim_no_match_whole_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p: Array<string> := \"abc\".Split(\"::\"); "
        "  return (1 if (p.Count = 1 and p[0] = \"abc\") else 0); "
        "end Main; end C;"), 1);
}

// -------------------------------------------------------------------------
// §9.6 (static) Compare(a, b): int and Compare(a, b, ignoreCase: bool): int —
// ordinal comparison; ignoreCase folds ASCII case. Returns negative/0/positive.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_string_compare_spec_9_6, compare_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return string.Compare(\"abc\", \"abc\"); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_string_compare_spec_9_6, compare_less) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (1 if string.Compare(\"abc\", \"abd\") < 0 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_string_compare_spec_9_6, ignorecase_equal_when_folded) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return string.Compare(\"ABC\", \"abc\", true); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_string_compare_spec_9_6, ordinal_uppercase_less_than_lowercase) {
    // Ordinal (ignoreCase=false): 'A' (65) < 'a' (97), so "ABC" < "abc".
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (1 if string.Compare(\"ABC\", \"abc\", false) < 0 else 0); "
        "end Main; end C;"), 1);
}

// -------------------------------------------------------------------------
// §9.6 (static) Repeat(c: char, count: int): string — a count-long string of c.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_string_repeat_spec_9_6, repeat_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string := string.Repeat('x', 5); return s.Length; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_string_repeat_spec_9_6, repeat_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string := string.Repeat('a', 3); "
        "  return (1 if s = \"aaa\" else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_string_repeat_spec_9_6, repeat_zero_is_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string := string.Repeat('z', 0); return s.Length; "
        "end Main; end C;"), 0);
}

// ── #741 stdlib §9.6: IsAscii, Split(Array<char>), Trim(IEnumerable<char>) ───
// Each emitted no code pre-fix: IsAscii read an uninitialized i1 (always false);
// Split(Array<char>) / Trim(set) fell through, leaving the result null →
// SIGSEGV / SIGABRT / wrong count.

// §9.6: IsAscii is true iff every code point < 128.
BEZEL_TEST(codegen_string_741, is_ascii_true_for_ascii) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (1 if \"Hello\".IsAscii else 0); "
        "end Main; end C;"), 1);
}

// §9.6: a string with a code point ≥ 128 is not ASCII.
BEZEL_TEST(codegen_string_741, is_ascii_false_for_latin1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (1 if \"H\\u00e9llo\".IsAscii else 0); "  // é = U+00E9
        "end Main; end C;"), 0);
}

// §9.6: the empty string is ASCII.
BEZEL_TEST(codegen_string_741, is_ascii_empty_is_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return (1 if \"\".IsAscii else 0); "
        "end Main; end C;"), 1);
}

// §9.6: Split(separators: Array<char>) splits on ANY of the chars.
BEZEL_TEST(codegen_string_741, split_array_of_chars_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var seps := new Array<char>(); "
        "  seps.Add(','); seps.Add(';'); "
        "  using var parts := \"a,b;c\".Split(seps); "
        "  return parts.Count; "  // 3
        "end Main; end C;"), 3);
}

// §9.6: the split segments carry the correct content.
BEZEL_TEST(codegen_string_741, split_array_of_chars_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var seps := new Array<char>(); "
        "  seps.Add(','); seps.Add(';'); "
        "  using var parts := \"a,b;c\".Split(seps); "
        "  if parts[0] <> \"a\" then return 1; end; "
        "  if parts[1] <> \"b\" then return 2; end; "
        "  if parts[2] <> \"c\" then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §9.6: an empty separator set returns the whole string as one segment.
BEZEL_TEST(codegen_string_741, split_empty_set_is_whole_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var seps := new Array<char>(); "
        "  using var parts := \"abc\".Split(seps); "
        "  return parts.Count; "  // 1
        "end Main; end C;"), 1);
}

// §9.6: Trim(chars) strips leading/trailing code points in the set.
BEZEL_TEST(codegen_string_741, trim_char_set) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var chars := new Array<char>(); "
        "  chars.Add('x'); "
        "  var t := \"xxHelloxx\".Trim(chars); "
        "  return t.Length; "  // "Hello" = 5
        "end Main; end C;"), 5);
}

// §9.6: Trim(set) with multiple members strips any of them from both ends.
BEZEL_TEST(codegen_string_741, trim_char_set_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var chars := new Array<char>(); "
        "  chars.Add('_'); chars.Add('-'); "
        "  var t := \"_-mid-_\".Trim(chars); "
        "  return (1 if t = \"mid\" else 0); "
        "end Main; end C;"), 1);
}

}  // namespace
