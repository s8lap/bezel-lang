// stdlib §3.1 StringBuilder mutators — Insert, Remove, Replace. A StringBuilder
// is a parts-array of bz.string*; each mutator flattens the current content,
// applies the operation, and resets to a single part (bz_sb_insert / bz_sb_remove
// / bz_sb_replace in the runtime). The existing Append / AppendLine / Clear /
// Length / ToString surface is covered by collection/integration tests; these
// add the three §3.1 mutators the compiler previously lacked.
//
// StringBuilder is an owned type, so test programs bind it with `using var`
// (§28.3) to satisfy ownership.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// -------------------------------------------------------------------------
// §3.1 Insert(index: int, value: string): StringBuilder
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_insert_spec_3_1, insert_middle) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"Hello\"); "
        "  sb.Insert(5, \" World\"); "
        "  return (if sb.ToString() = \"Hello World\" then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_sb_insert_spec_3_1, insert_at_start) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"World\"); "
        "  sb.Insert(0, \"Hello \"); "
        "  return (if sb.ToString() = \"Hello World\" then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_sb_insert_spec_3_1, insert_at_end) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"ab\"); "
        "  sb.Insert(2, \"cd\"); "
        "  return (if sb.ToString() = \"abcd\" then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_sb_insert_spec_3_1, insert_out_of_range_raises_index_out_of_range) {
    // fix #7221 (stdlib §3.1, v0.343): an index PAST the content's Length is
    // outside `0..Length`, so §3.1's sentence decides it — "`Insert` and
    // `Remove` raise `IndexOutOfRangeException` when the index or the
    // `(startIndex, length)` window falls outside `0..Length`". The row
    // pinned the opposite (a silent no-op with the text unchanged) before the
    // fix; it now pins the RAISE, observed through the catch arm's exit code.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"ab\"); "
        "  try "
        "    sb.Insert(99, \"X\"); "
        "  catch IndexOutOfRangeException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// -------------------------------------------------------------------------
// §3.1 Remove(startIndex: int, length: int): StringBuilder
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_remove_spec_3_1, remove_middle) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"Hello World\"); "
        "  sb.Remove(5, 6); "
        "  return (if sb.ToString() = \"Hello\" then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_sb_remove_spec_3_1, remove_prefix) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"abcdef\"); "
        "  sb.Remove(0, 3); "
        "  return (if sb.ToString() = \"def\" then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_sb_remove_spec_3_1, remove_all) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"xyz\"); "
        "  sb.Remove(0, 3); "
        "  return sb.ToString().Length; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_sb_remove_spec_3_1, remove_out_of_range_raises_index_out_of_range) {
    // fix #7221 (stdlib §3.1, v0.343): a `(startIndex, length)` window that
    // ends PAST the content's Length falls outside `0..Length`, so §3.1's
    // sentence decides it — "`Insert` and `Remove` raise
    // `IndexOutOfRangeException` when the index or the `(startIndex, length)`
    // window falls outside `0..Length`". The row pinned the opposite (a
    // silent no-op with the text unchanged) before the fix; it now pins the
    // RAISE, observed through the catch arm's exit code.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"abc\"); "
        "  try "
        "    sb.Remove(1, 99); "
        "  catch IndexOutOfRangeException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 42);
}

// -------------------------------------------------------------------------
// §3.1 Replace(oldValue: string, newValue: string): StringBuilder
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_replace_spec_3_1, replace_all_occurrences) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"a-b-c\"); "
        "  sb.Replace(\"-\", \"+\"); "
        "  return (if sb.ToString() = \"a+b+c\" then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_sb_replace_spec_3_1, replace_longer_new_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"a.b.c\"); "
        "  sb.Replace(\".\", \"::\"); "
        "  return (if sb.ToString() = \"a::b::c\" then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_sb_replace_spec_3_1, replace_substring) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"one fish two fish\"); "
        "  sb.Replace(\"fish\", \"cat\"); "
        "  return (if sb.ToString() = \"one cat two cat\" then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_sb_replace_spec_3_1, replace_no_match_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"abc\"); "
        "  sb.Replace(\"z\", \"Q\"); "
        "  return (if sb.ToString() = \"abc\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// -------------------------------------------------------------------------
// §3.1 chaining: mutators return `this`, so calls compose. Append after a
// mutator must operate on the post-mutation content.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_mutators_spec_3_1, mutate_then_append) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"Hello\"); "
        "  sb.Insert(0, \"[\"); "
        "  sb.Append(\"]\"); "
        "  return (if sb.ToString() = \"[Hello]\" then 1 else 0); "
        "end Main; end C;"), 1);
}

}  // namespace
