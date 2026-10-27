// Regression tests for #7221 — `StringBuilder.Insert` / `StringBuilder.Remove`
// SILENTLY NO-OP'd an out-of-range request while stdlib §3.1 says they RAISE
// `IndexOutOfRangeException`.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §3.1 (v0.343), the `StringBuilder` Contracts paragraph, verbatim:
//         `Insert` and `Remove` raise `IndexOutOfRangeException` when the
//         index or the `(startIndex, length)` window falls outside `0..Length`;
//         `Replace` raises `ArgumentException` when `oldValue` is empty.
//   * language spec §4.6 (v0.343): a length is a count of CODE POINTS, so the
//     bound `0..Length` is INCLUSIVE at both ends — `Insert(Length, …)`
//     appends and stays admitted, `Insert(Length+1, …)` and `Insert(-1, …)`
//     are outside the bound and raise.
//   * language spec §31.4 (v0.343): "IndexOutOfRangeException — array or
//     string index out of bounds."
//   * language spec §5.1 (v0.343): a `byte` widens to the `int` of the same
//     value (the widening is LOSSLESS), so the bound is tested on the §5.1
//     value — `byte` 200 is the `int` 200, never -56.
//   * language spec §22.5 (v0.343): a named argument binds the PARAMETER it
//     NAMES.
//
// THE DEFECT (MEASURED on the base tree 94d0b7fd).
//
//   | call                | stdout | rc |
//   |---------------------|--------|----|
//   | sb.Insert(99, "X") | `ab`   | 0  |  <- the text is UNCHANGED
//   | sb.Remove(1, 99)   | `abc`  | 0  |  <- the text is UNCHANGED
//
// ⇒ the compiler ACCEPTED the calls, the runtime helper NO-OP'd them, and the
// exit code said the request SUCCEEDED: a request §3.1 rejects was reported as
// a success. The runtime helper's own comment says the range check belongs to
// the codegen layer ("the codegen-side bounds check raises
// IndexOutOfRangeException per the §3.1 v0.195 contract") — the check was
// missing.
//
// THE BOUND DOES NOT MOVE. `0..Length` is INCLUSIVE at both ends: the
// in-range calls are unchanged (rows D, E, F, J, K), and only the calls
// outside the bound raise.
//
// ORACLE CHOICE. The observable is the RAISE, so the rows pin the exception
// TYPE through a `catch` that returns a distinct exit code per catch clause —
// never merely "the program did not print the clamped text". The
// memory-safety rows are on `runAotChecked` (ASan + LSan armed) so the new
// throw edge is observed by the sanitizer: the builder's parts must still be
// DISPOSED when the guard unwinds (CG21), or the raise leaks them.
//
// A StringBuilder is an owned type, so the programs bind it with `using var`
// (§28.3).

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A program that calls `call` on a builder holding "ab" inside a try/catch
// that returns `code` when an exception of `catchType` is raised and 0
// otherwise. The two are distinguishable by the exit code, so the row asserts
// the EXCEPTION rather than merely "the program did not crash".
std::string raising(const std::string& call, const char* catchType, int code) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    using var sb := new StringBuilder();\n"
           "    sb.Append(\"ab\");\n"
           "    try\n"
           "      " + call + ";\n"
           "    catch " + catchType + " => return " + std::to_string(code) +
           ";\n"
           "    end;\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same shape, but the OWNED builder is declared INSIDE the try — so the
// guard's throw edge UNWINDS it. This is the shape the memory-safety rows
// must use: the raise happens on a path that previously returned normally,
// and the builder's parts must still be released on that edge (CG21), or the
// raise leaks them.
std::string raisingWithOwnedBuilder(const std::string& call) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      using var sb := new StringBuilder();\n"
           "      sb.Append(\"ab\");\n"
           "      " + call + ";\n"
           "    catch IndexOutOfRangeException => return 42;\n"
           "    end;\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The IN-RANGE control: three SEPARATE builders, so each insertion is made
// against the same two-code-point content ("ab") and the expected texts are
// the ones the §3.1 member produces.
std::string insertInRange() {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    using var sb1 := new StringBuilder();\n"
           "    sb1.Append(\"ab\");\n"
           "    sb1.Insert(0, \"X\");\n"
           "    Console.WriteLine(\"[\" + sb1.ToString() + \"]\");\n"
           "    using var sb2 := new StringBuilder();\n"
           "    sb2.Append(\"ab\");\n"
           "    sb2.Insert(1, \"X\");\n"
           "    Console.WriteLine(\"[\" + sb2.ToString() + \"]\");\n"
           "    using var sb3 := new StringBuilder();\n"
           "    sb3.Append(\"ab\");\n"
           "    sb3.Insert(2, \"X\");\n"
           "    Console.WriteLine(\"[\" + sb3.ToString() + \"]\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The `Remove` in-range control, and the at-Length EMPTY WINDOW control
// (`Remove(Length, 0)` — the half-open window `[Length, Length)` is empty and
// lies inside `0..Length`, so it is admitted).
std::string removeInRange() {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    using var sb1 := new StringBuilder();\n"
           "    sb1.Append(\"abc\");\n"
           "    sb1.Remove(1, 1);\n"
           "    Console.WriteLine(\"[\" + sb1.ToString() + \"]\");\n"
           "    using var sb2 := new StringBuilder();\n"
           "    sb2.Append(\"abc\");\n"
           "    sb2.Remove(3, 0);\n"
           "    Console.WriteLine(\"[\" + sb2.ToString() + \"]\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The §5.1 row's program: a builder of 300 code points, so a `byte` 200 is
// INSIDE `0..300` and must be a legal index.
const char* kLong300 =
    "    using var sb := new StringBuilder();\n"
    "    var i := 0;\n"
    "    while i < 300 do\n"
    "      sb.Append(\"x\");\n"
    "      i := i + 1;\n"
    "    end;\n";

}  // namespace

// ===========================================================================
// (A) The issue's literal repro — `Insert(index, value)` with an index past
// the end of the content.
//
// Pre-fix: compiles clean, rc 0, the text is UNCHANGED ("ab").
// Post-fix: the guard raises IndexOutOfRangeException (exit 42).
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           insert_out_of_range_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(raising("sb.Insert(99, \"X\")",
                                  "IndexOutOfRangeException", 42));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The same raise, caught by the BASE type (stdlib §2.5) — the guard raises a
// built-in derived from `Exception`, so a base-typed catch must find it.
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           insert_out_of_range_is_catchable_as_exception_stdlib_2_5) {
    JitResult r = runAot(raising("sb.Insert(99, \"X\")", "Exception", 43));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 43);
}

// ===========================================================================
// (B) The other side of the index's domain — a NEGATIVE index.
//
// Pre-fix: the runtime no-op'd it, rc 0 — a request §3.1 rejects reported as
// a success.
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           insert_negative_index_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(raising("sb.Insert(-1, \"X\")",
                                  "IndexOutOfRangeException", 42));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (C) ONE PAST THE END — `Length + 1`.
//
// `0..Length` is inclusive at both ends, so `Length` is legal and
// `Length + 1` is not. This row is the twin of the control row (D): the same
// shape, one step further out.
//
// Pre-fix: rc 0, the text unchanged.
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           insert_one_past_length_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(raising("sb.Insert(3, \"X\")",
                                  "IndexOutOfRangeException", 42));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (D) THE CONTROL — the IN-RANGE calls are unchanged.
//
// Green on BOTH arms (pre-fix and post-fix): the guard must not reject a
// valid index. `Insert(0, …)` prepends, `Insert(1, …)` inserts in the
// middle, and `Insert(2, …)` — the at-Length boundary — APPENDS.
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           insert_in_range_results_are_unchanged_stdlib_3_1) {
    JitResult r = runAot(insertInRange());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[Xab]\n[aXb]\n[abX]\n");
}

// ===========================================================================
// (E) THE NAMED-ARGUMENT SPELLING.
//
// §22.5 (v0.343): a named argument binds the PARAMETER it NAMES, so the
// in-declaration-order named spelling `sb.Insert(index := 99, value := "X")`
// binds the 99 to `index` and must RAISE.
//
// ⚠ The REORDERED spelling (`value := …, index := …`) is still rejected by
// the front end — the #7150-family gate in `src/sema/ExprTypecheck_Calls.cpp`
// reads `ce.args` positionally — so it is not reachable end-to-end and this
// row does not exercise it.
//
// Pre-fix: the named spelling no-op'd exactly as the positional one did
// (rc 0, the text unchanged).
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           insert_named_index_out_of_range_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(raising("sb.Insert(index := 99, value := \"X\")",
                                  "IndexOutOfRangeException", 42));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (F) `Remove` — the WINDOW falls outside `0..Length`.
//
// §3.1: "the `(startIndex, length)` window falls outside `0..Length`". The
// content is "ab" (Length 2), so `(1, 99)` ends three code points past the
// end.
//
// Pre-fix: rc 0, the text unchanged.
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           remove_out_of_range_window_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(raising("sb.Remove(1, 99)",
                                  "IndexOutOfRangeException", 42));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The other half of the window: the START is past the end.
//
// Pre-fix: rc 0, the text unchanged.
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           remove_start_out_of_range_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(raising("sb.Remove(99, 1)",
                                  "IndexOutOfRangeException", 42));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// A NEGATIVE length — the window's length is not a length at all.
//
// Pre-fix: rc 0, the text unchanged.
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           remove_negative_length_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(raising("sb.Remove(0, -1)",
                                  "IndexOutOfRangeException", 42));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (G) THE CONTROL — the in-range `Remove` calls are unchanged.
//
// Green on BOTH arms: `Remove(1, 1)` removes the middle code point, and
// `Remove(3, 0)` — the EMPTY window at `Length` — is inside `0..Length` and
// leaves the content alone.
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           remove_in_range_results_are_unchanged_stdlib_3_1) {
    JitResult r = runAot(removeInRange());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[ac]\n[abc]\n");
}

// ===========================================================================
// (H) MEMORY SAFETY — the new throw edge on the `runAotChecked` oracle
// (ASan + LSan armed).
//
// The guard introduces a NEW throw edge on a path that previously returned
// normally. The builder is an OWNED value (`using var`, §28.3), so the unwind
// must dispose it (CG21) — and the disposal releases the builder's parts. If
// the raise skipped that disposal, LSan would report the parts as leaked.
//
// The builder is declared INSIDE the try, so the throw really does unwind it.
//
// Post-fix: no ASan report, no LSan leak, and the catch arm's exit code is
// 42.
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           insert_out_of_range_is_memory_safe_stdlib_3_1) {
    JitResult r = runAotChecked(
        raisingWithOwnedBuilder("sb.Insert(99, \"X\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // no ASan report, no LSan leak
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The same edge, on the `Remove` window.
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           remove_out_of_range_is_memory_safe_stdlib_3_1) {
    JitResult r = runAotChecked(
        raisingWithOwnedBuilder("sb.Remove(1, 99)"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (I) §5.1 — an UNSIGNED index widens to the `int` of the same value, so the
// bound is tested on the WIDENED value.
//
// `byte` 200 on a 300-code-point builder is inside `0..300` and must be a
// legal index — and the insertion must land at 200. This row is the
// anti-regression for the widening: a guard that tested the sign-extended
// -56 would raise here, and one that tested the un-widened i8 would raise
// too.
//
// It is also the anti-regression for the guard and the mutation AGREEING: the
// guard tests the widened value and the runtime helper is handed the SAME
// widened value. A guard that tested one and the mutation used the other
// would leave a silent no-op behind — the very defect #7221 is about.
//
// Pre-fix: the sign-extended -56 reached the runtime helper, which no-opped
// it — the content stayed 300 code points and `IndexOf("Z")` printed -1.
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           insert_byte_index_is_widened_losslessly_spec_5_1) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        std::string(kLong300) +
        "    var b: byte := 200;\n"
        "    sb.Insert(b, \"Z\");\n"
        "    Console.WriteLine(sb.ToString().IndexOf(\"Z\").ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// ===========================================================================
// (J) A flow-NARROWED `int?` index — sema admits it as the non-nullable
// `int` parameter, and the guard must read the narrowed payload.
//
// Green on BOTH arms: the narrowing is not what this fix changes, and the
// row pins that the §5.1 widening did not break it.
// ===========================================================================
BEZEL_TEST(stringbuilder_insert_remove_range_7221,
           insert_narrowed_nullable_index_is_admitted_stdlib_3_1) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    sb.Append(\"ab\");\n"
        "    var i: int? := 1;\n"
        "    if i <> null then\n"
        "      sb.Insert(i, \"X\");\n"
        "    end;\n"
        "    Console.WriteLine(\"[\" + sb.ToString() + \"]\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[aXb]\n");
}
