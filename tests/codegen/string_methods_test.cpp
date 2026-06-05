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

}  // namespace
