// Regression tests for #6562 — a flow-NARROWED `int?` passed to `string.IndexOf`.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §9.6 (v0.339): `IndexOf(value: char, startIndex: int := 0)` and
//     `IndexOf(value: string, startIndex: int := 0)`. `startIndex` is a
//     code-point index in [0, Length]; a start equal to Length is valid and
//     finds nothing, so it returns -1.
//   * spec §7.4 (v0.339): an operand narrowed non-null is a non-nullable
//     operand. After `if n is null then return …; end;` a narrowed `int?` is
//     admissible at the `int` startIndex position, and its VALUE is the payload.
//   * spec §5.1 (v0.339): `short` converts implicitly to `int`.
//
// THE DEFECT. Both two-argument IndexOf arms (the `char` needle and the
// `string` needle) lowered startIndex with a bare `lowerExpr`. A narrowed
// `int?` therefore reached the sign-extension as the tagged-optional
// `{ i8, i32 }` box, and the compiler died ("SExt only operates on integer").
// Sema already admits the narrowed operand at this position and rejects the
// un-narrowed one (the `intPos` gate in checkBuiltinStringCallArgs), so the fix
// is the getIntArg unwrap. That is the same sema-gate-plus-unwrap pair as #2614.
//
// ORACLE CHOICE. The defect is a value-bearing miscompile, so the rows that
// pin stdout are the oracle. The values are chosen so that the PAYLOAD and the
// PRESENCE FLAG give different answers: a present `int?` has flag 1, and
// reading the flag instead of the payload would search from index 1 and find
// 'c' at index 2, not 5.
//
// EXPECTED VALUES, one row per BEZEL_TEST below. The string is "abcabc", where
// 'c' sits at indexes 2 and 5.
//
//   Category A — the DEFECT (each fails without the fix, dying in the compiler):
//     narrowed_int_q_start_index_char_needle        "5\n"
//     narrowed_int_q_start_index_string_needle      "5\n"
//     narrowed_int_q_start_index_at_length          "-1\n"
//     narrowed_short_q_start_index_char_needle      "5\n"
//
//   Category B — CONTROLS (green on both sides; the plain-int path was never
//   broken):
//     plain_int_start_index_char_needle_control     "5\n"
//     plain_int_start_index_string_needle_control   "5\n"
//
//   Category C — the SOUNDNESS twin (fails without the fix, because the program
//   was not rejected before codegen):
//     narrowed_int_q_search_value_rejected_before_codegen  rejected, not run
//
// ORACLE DISCIPLINE. Every value row asserts `ran` and the whole stdout
// byte-for-byte. The reject row asserts `!ran` AND the dedicated diagnostic,
// because a compiler crash also yields `!ran`.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Wraps `body` in a runnable `Main` that returns 0.
std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasErrorContaining(const bezel::test::JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// Category A — the DEFECT: a narrowed `int?` startIndex
// ===========================================================================

// §7.4 narrows `n` to its payload 3. §9.6 then searches "abcabc" from index 3,
// where the next 'c' is at 5. Reading the presence flag (1) would give 2.
BEZEL_TEST(indexof_int_start_index_6562,
           narrowed_int_q_start_index_char_needle) {
    auto r = runAot(prog(
        "    var n: int? := 3;\n"
        "    if n is null then return 1; end;\n"
        "    Console.WriteLine(\"abcabc\".IndexOf('c', n).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// The `string` needle takes the other 2-argument arm, which had the same raw
// lowering.
BEZEL_TEST(indexof_int_start_index_6562,
           narrowed_int_q_start_index_string_needle) {
    auto r = runAot(prog(
        "    var n: int? := 3;\n"
        "    if n is null then return 1; end;\n"
        "    Console.WriteLine(\"abcabc\".IndexOf(\"c\", n).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// §9.6: a start equal to Length is in range and finds nothing. Payload 6 is
// used because the flag (1) would instead return 2.
BEZEL_TEST(indexof_int_start_index_6562,
           narrowed_int_q_start_index_at_length) {
    auto r = runAot(prog(
        "    var n: int? := 6;\n"
        "    if n is null then return 1; end;\n"
        "    Console.WriteLine(\"abcabc\".IndexOf('c', n).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// §5.1: `short` widens implicitly to `int`. The narrowed `short?` payload has
// the narrower width, so it must be sign-extended from i16 to i64 correctly.
BEZEL_TEST(indexof_int_start_index_6562,
           narrowed_short_q_start_index_char_needle) {
    auto r = runAot(prog(
        "    var s: short? := 3;\n"
        "    if s is null then return 1; end;\n"
        "    Console.WriteLine(\"abcabc\".IndexOf('c', s).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// ===========================================================================
// Category B — CONTROLS
// ===========================================================================

// A plain `int` startIndex was never broken. These rows guard the unchanged
// path against a fix that changes the `int` lowering.
BEZEL_TEST(indexof_int_start_index_6562,
           plain_int_start_index_char_needle_control) {
    auto r = runAot(prog(
        "    var p: int := 3;\n"
        "    Console.WriteLine(\"abcabc\".IndexOf('c', p).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

BEZEL_TEST(indexof_int_start_index_6562,
           plain_int_start_index_string_needle_control) {
    auto r = runAot(prog(
        "    var p: int := 3;\n"
        "    Console.WriteLine(\"abcabc\".IndexOf(\"c\", p).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// ===========================================================================
// Category C — the SOUNDNESS twin: the position-0 `int?` is rejected
// ===========================================================================

// The issue's repro. §9.6 has no `int` search-value overload, and §7.4 narrowing
// removes only the null case. The program must be rejected by sema with the
// dedicated message and must never reach codegen. Before the fix it ran into
// malformed IR instead.
BEZEL_TEST(indexof_int_start_index_6562,
           narrowed_int_q_search_value_rejected_before_codegen) {
    auto r = runAot(prog(
        "    var n: int? := 1;\n"
        "    if n is null then return 1; end;\n"
        "    Console.WriteLine(\"abc\".IndexOf(n).ToString());\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "takes its search value at position 0 as a `char` or a `string`"));
}
