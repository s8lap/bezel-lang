// Fix #500: String.Split() result type not set in sema — unannotated local
// (`var parts := s.Split(',')`) was untyped, so .Count/.Get(i)/for-in
// all silently returned 0.
//
// ROOT CAUSE UNDER TEST:
//   sema had `if (ma.member == "Split") return;` without calling
//   ctx_.setExprType(), so the call node carried no type.  An unannotated
//   `var parts := s.Split(',')` inferred the sentinel (untyped) TypeRef,
//   meaning downstream member access resolved to nothing → 0.
//
// The fix: set the result type to Array<string> via makeConcreteCollectionResult,
// mirroring the ToArray()/ToSet() fix for LINQ materializers.
//
// Spec reference: stdlib §4.6 (Array), §9.6 (String.Split overloads).
// All three overloads (Split(char), Split(string), Split(Array<char>))
// return owned Array<string>.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// §9.6: Split(char) — unannotated local, .Count (the primary bug repro)
// Before fix: returns 0.  After fix: returns 3.
// ===========================================================================

BEZEL_TEST(split_result_type_500, char_count_unannotated_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"a,b,c\"; "
        "  using var parts := s.Split(','); "
        "  return parts.Count; "
        "end Main; end C;"), 3);
}

// ===========================================================================
// §9.6: Split(char) — unannotated local, .Get(1) == "b"
// ===========================================================================

BEZEL_TEST(split_result_type_500, char_get_unannotated_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"a,b,c\"; "
        "  using var parts := s.Split(','); "
        "  return (if parts.Get(1) = \"b\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §9.6: Split(char) — unannotated local, indexer parts[1] == "b"
// ===========================================================================

BEZEL_TEST(split_result_type_500, char_indexer_unannotated_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"aa,bb,cc\"; "
        "  using var parts := s.Split(','); "
        "  return (if parts[1] = \"bb\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §9.6: Split(char) — unannotated local, for-in iteration
// Sum string lengths: "a" + "b" + "c" = 1+1+1 = 3.
// ===========================================================================

BEZEL_TEST(split_result_type_500, char_for_in_unannotated_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"a,b,c\"; "
        "  using var parts := s.Split(','); "
        "  var total := 0; "
        "  for var p in parts do total := total + p.Length; end; "
        "  return total; "
        "end Main; end C;"), 3);
}

// ===========================================================================
// §9.6: Split(string) overload — unannotated local, .Count
// "a::b::c".Split("::") should yield 3 parts.
// ===========================================================================

BEZEL_TEST(split_result_type_500, string_delim_count_unannotated_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"a::b::c\"; "
        "  using var parts := s.Split(\"::\"); "
        "  return parts.Count; "
        "end Main; end C;"), 3);
}

// ===========================================================================
// §9.6: Split(string) overload — unannotated local, .Get(0) == "a"
// ===========================================================================

BEZEL_TEST(split_result_type_500, string_delim_get_unannotated_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"aa::bb::cc\"; "
        "  using var parts := s.Split(\"::\"); "
        "  return (if parts.Get(0) = \"aa\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §9.6: Split(char) — empty source string → 1 part (the empty string itself)
// ===========================================================================

BEZEL_TEST(split_result_type_500, char_empty_source_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"\"; "
        "  using var parts := s.Split(','); "
        "  return parts.Count; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §9.6: Split(char) — no separator present → 1 part (whole string)
// ===========================================================================

BEZEL_TEST(split_result_type_500, char_no_separator_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"abc\"; "
        "  using var parts := s.Split(','); "
        "  return (if (parts.Count = 1 and parts.Get(0) = \"abc\") then 1 else 0); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// CONTROL: annotated `using var parts: Array<string> := ...` still works
// (regression guard — the fix must not break the pre-existing annotated path)
// ===========================================================================

BEZEL_TEST(split_result_type_500, control_annotated_char_still_works_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"a,b,c\"; "
        "  var parts: Array<string> := s.Split(','); "
        "  return parts.Count; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(split_result_type_500, control_annotated_string_delim_still_works_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p: Array<string> := \"a::b::c\".Split(\"::\"); return p.Count; "
        "end Main; end C;"), 3);
}
