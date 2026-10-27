// Regression tests for #7172 — `string.Insert` SILENTLY CLAMPED an
// out-of-range `startIndex` instead of raising `IndexOutOfRangeException`.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §9.6 (v0.343) declares the member with a POSITION parameter:
//         public pure method Insert(startIndex: int, value: string): string;
//     and carries NO contract paragraph for it — §3.1 (v0.343) records that
//     silence for the §9.6 range family.
//   * stdlib §3.1 (v0.343), the `StringBuilder` twin's Contracts paragraph,
//     verbatim:
//         `Insert` and `Remove` raise `IndexOutOfRangeException` when the
//         index or the `(startIndex, length)` window falls outside `0..Length`;
//         `Replace` raises `ArgumentException` when `oldValue` is empty.
//     `String.Insert` and `StringBuilder.Insert` are the same operation over
//     the same index space, so §3.1's sentence decides the §9.6 twin — exactly
//     as it decides `String.Remove` (#7168), whose twin arm raises it.
//   * language spec §4.6 (v0.343): a string's length is a count of CODE
//     POINTS, and the string indexer raises `IndexOutOfRangeException` when
//     out of range.
//   * language spec §31.4 (v0.343): "IndexOutOfRangeException — array or
//     string index out of bounds."
//   * language spec §5.1 (v0.343): a `byte` widens to the `int` of the same
//     value (the widening is LOSSLESS), so the bound is tested on the §5.1
//     value — `byte` 200 is the `int` 200, never -56.
//
// THE DEFECT (MEASURED on the base tree 1465552e).
//
//   var s: string := "ab";
//   Console.WriteLine(s.Insert(999, "z"));
//
// ⇒ the compiler ACCEPTS it (rc 0), the program prints a CLAMPED string, and
// the exit code says the request SUCCEEDED:
//
//   | call                | stdout | rc |
//   |---------------------|--------|----|
//   | "ab".Insert(999,"z")| `abz`  | 0  |
//   | "ab".Insert(-1,"z") | `zab`  | 0  |
//   | "ab".Insert(3,"z")  | `abz`  | 0  |
//   | "ab".Insert(2,"z")  | `abz`  | 0  |  <- the LEGAL boundary
//
// ⇒ the CLAMP must become a GUARD: §3.1's sentence says *raises*, and a clamp
// to `[0, Length]` is exactly what the contract forbids.
//
// THE BOUNDARY DOES NOT MOVE. `0..Length` is INCLUSIVE at both ends:
// `Insert(0, …)` prepends and `Insert(Length, …)` appends, and both are inside
// the bound and must NOT raise. `Insert(-1, …)` and `Insert(Length+1, …)` are
// outside it and must raise.
//
// ORACLE CHOICE. The observable is the RAISE, so the rows pin the exception
// TYPE through a `catch` that returns a distinct exit code per catch clause —
// never merely "the program did not print the clamped string". The
// memory-safety row is on `runAotChecked` (ASan + LSan armed) so that the
// throw edge's release of the producer receiver is observed by the sanitizer;
// the conformance corpus runner arms NO sanitizer, so it cannot observe that
// half ⇒ that row lives HERE, not in the corpus.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A program that evaluates `render` on the receiver "ab" inside a
// try/catch that returns 42 when an IndexOutOfRangeException is raised and 0
// otherwise. The two are distinguishable by the exit code, so the row
// asserts the EXCEPTION rather than merely "the program did not crash".
std::string raising(const std::string& render) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var s: string := \"ab\";\n"
           "    try\n"
           "      var t := " + render + ";\n"
           "      Console.WriteLine(t);\n"
           "      return 0;\n"
           "    catch IndexOutOfRangeException => return 42;\n"
           "    end;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same shape, but catching the BASE type — the guard raises a
// stdlib §2.5-derived built-in, so a base-typed catch must find it.
std::string raisingAsBase(const std::string& render) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var s: string := \"ab\";\n"
           "    try\n"
           "      var t := " + render + ";\n"
           "      Console.WriteLine(t);\n"
           "      return 0;\n"
           "    catch Exception => return 43;\n"
           "    end;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same shape, but the RECEIVER is a +1 string PRODUCER (`("a" + "b")`)
// rather than a literal.
//
// ⚠ This is the shape the MEMORY-SAFETY row must use: the guard RAISES, so
// the throw edge must release the caller-owned producer receiver's +1
// reference (§28.9 — the #6979/#7159 idiom). With a literal receiver there
// is no producer to release and the sanitizer would have nothing to observe.
std::string raisingOnProducer(const std::string& render) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      var t := " + render + ";\n"
           "      Console.WriteLine(t);\n"
           "      return 0;\n"
           "    catch IndexOutOfRangeException => return 42;\n"
           "    end;\n"
           "  end Main;\n"
           "end C;\n";
}

// A program that prints the results of the IN-RANGE calls — the CONTROL for
// the guard: the valid shapes must not change.
std::string inRange() {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var s: string := \"ab\";\n"
           "    Console.WriteLine(\"[\" + s.Insert(0, \"z\") + \"]\");\n"
           "    Console.WriteLine(\"[\" + s.Insert(1, \"z\") + \"]\");\n"
           "    Console.WriteLine(\"[\" + s.Insert(2, \"z\") + \"]\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The receiver used by the §5.1 row: 252 code points, so that a `byte` 200 is
// a LEGAL index (inside 0..252) and the insertion must land at 200 — the
// value #7102's widening fix established.
const char* kLong252 =
    "    var s: string := \"c\" + \"x\".Repeat(250) + \"c\";\n";

}  // namespace

// ===========================================================================
// (A) The issue's literal repro — `Insert(startIndex)` with a startIndex past
// the end of the receiver.
//
// Pre-fix: compiles clean, rc 0, prints the clamped string `abz`.
// Post-fix: the guard raises IndexOutOfRangeException (exit 42).
// ===========================================================================
BEZEL_TEST(string_insert_range_7172,
           insert_start_index_out_of_range_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(raising("s.Insert(999, \"z\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The same raise, caught by the BASE type (stdlib §2.5) — the guard raises a
// built-in derived from `Exception`, so a base-typed catch must find it.
BEZEL_TEST(string_insert_range_7172,
           insert_start_index_out_of_range_is_catchable_as_exception_stdlib_2_5) {
    JitResult r = runAot(raisingAsBase("s.Insert(999, \"z\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 43);
}

// ===========================================================================
// (B) The other side of the position's domain — a NEGATIVE startIndex.
//
// Pre-fix: the clamp pinned the insertion to the FRONT of the string
// (`zab`), rc 0 — a wrong answer reported as success.
// ===========================================================================
BEZEL_TEST(string_insert_range_7172,
           insert_negative_start_index_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(raising("s.Insert(-1, \"z\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (C) ONE PAST THE END — `Length + 1`.
//
// `0..Length` is inclusive at both ends, so `Length` is legal and
// `Length + 1` is not. This row is the twin of the control row (D): it is
// the same shape, one step further out.
//
// Pre-fix: the clamp pinned the insertion to the END of the string (`abz`),
// rc 0.
// ===========================================================================
BEZEL_TEST(string_insert_range_7172,
           insert_start_index_one_past_length_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(raising("s.Insert(3, \"z\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (D) THE CONTROL — the IN-RANGE calls are unchanged.
//
// Green on BOTH arms (pre-fix and post-fix): the guard must not reject a
// valid index. `Insert(0, …)` prepends, `Insert(1, …)` inserts in the
// middle, and `Insert(2, …)` — the at-Length boundary — APPENDS (the same
// convention `Substring`, `Remove` and the range slice use).
// ===========================================================================
BEZEL_TEST(string_insert_range_7172,
           insert_in_range_results_are_unchanged_stdlib_3_1) {
    JitResult r = runAot(inRange());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "[zab]\n[azb]\n[abz]\n");
}

// ===========================================================================
// (E) THE NAMED-ARGUMENT SPELLING.
//
// §22.5 (v0.343): a named argument binds the PARAMETER it NAMES. The
// #7180 pick that landed on the staging tip made the §9.6 instance arms read
// their parameter's source argument from the published binding, so the
// in-declaration-order named spelling `s.Insert(startIndex := 999,
// value := "z")` binds the 999 to `startIndex` and must RAISE.
//
// ⚠ The REORDERED spelling (`value := …, startIndex := …`) is still rejected
// by the front end — the #7150-family gate in `src/sema/ExprTypecheck_Calls.cpp`
// reads `ce.args` positionally — so it is not reachable end-to-end and this
// row does not exercise it.
//
// Pre-fix: the named spelling clamped exactly as the positional one did
// (`abz`, rc 0).
// ===========================================================================
BEZEL_TEST(string_insert_range_7172,
           insert_named_start_index_out_of_range_raises_index_out_of_range_stdlib_3_1) {
    JitResult r = runAot(
        raising("s.Insert(startIndex := 999, value := \"z\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The named spelling with an IN-RANGE index — the control that the guard
// did not start reading the index positionally.
BEZEL_TEST(string_insert_range_7172,
           insert_named_in_range_is_unchanged_stdlib_3_1) {
    JitResult r = runAot(
        raising("s.Insert(startIndex := 1, value := \"z\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (F) MEMORY SAFETY — the same raise on the `runAotChecked` oracle (ASan +
// LSan armed), on a PRODUCER receiver.
//
// The guard introduces a NEW throw edge on a path that previously returned
// normally, and §28.9 (v0.343) makes the producer receiver's +1 reference
// the CALLER's to release AFTER the call returns — a raise unwinds past that
// release, and §31.4's built-in exceptions are trivial-dispose, so nothing
// else releases it. The guard must release it ON THE THROW EDGE (the
// #6979/#7159 idiom), or the raise LEAKS the receiver.
//
// Post-fix: nothing is read out of bounds, the producer's reference is
// released on the throw edge, so LSan is quiet, and the catch arm's exit
// code is 42.
BEZEL_TEST(string_insert_range_7172,
           insert_start_index_out_of_range_is_memory_safe_stdlib_3_1) {
    JitResult r = runAotChecked(raisingOnProducer("(\"a\" + \"b\").Insert(999, \"z\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // no ASan report, no LSan leak
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// The NEGATIVE index on the same oracle — the other side of the domain.
BEZEL_TEST(string_insert_range_7172,
           insert_negative_start_index_is_memory_safe_stdlib_3_1) {
    JitResult r = runAotChecked(raisingOnProducer("(\"a\" + \"b\").Insert(-1, \"z\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (G) §5.1 — an UNSIGNED `startIndex` widens to the `int` of the same value,
// so the bound is tested on the WIDENED value.
//
// `byte` 200 on a 252-code-point receiver is inside `0..252` and must be
// legal — and the insertion must land at 200 (the value #7102's widening fix
// established). This row is the anti-regression for the widening: a guard
// that tested the sign-extended -56 would raise here, and one that tested
// the un-widened i8 would raise too.
//
// The literal 200 is the CONTROL: it is a signed `int`, so it was never
// broken, and the `byte` row must print what the literal prints.
BEZEL_TEST(string_insert_range_7172,
           insert_byte_start_index_is_widened_losslessly_spec_5_1) {
    auto r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        std::string(kLong252) +
        "    var b: byte := 200;\n"
        "    Console.WriteLine(s.Insert(b, \"Z\").IndexOf(\"Z\").ToString());\n"
        "    Console.WriteLine(s.Insert(200, \"Z\").IndexOf(\"Z\").ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n200\n");
}
