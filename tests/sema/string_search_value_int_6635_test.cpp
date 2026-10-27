// Spec-derived regression test for issue #6635.
//
// stdlib §9.6 (v0.345) declares the `string` members' search values: the
// declared overload sets are `Contains(value: char)` / `Contains(value:
// string)`, `LastIndexOf(value: char)` / `LastIndexOf(value: string)`,
// `Replace(oldChar: char, newChar: char)` / `Replace(oldValue: string,
// newValue: string)` — at those positions NOTHING ELSE is declared.
//
// §5.1 converts no integer type to a `char` or to a `string` (its table has
// no such row, and its constant-narrowing clause narrows an integer to an
// INTEGER target only), so an `int` search value reaches NO declared
// overload: §22.6 makes the call a compile error with no candidates cited.
//
// BEFORE THE FIX (measured on 33a3912c):
//
//   "abc".Contains(5)     -> "Call parameter type does not match function
//                             signature!" + "IR verification failed" — an
//                             `i32` handed to a `ptr` parameter: the
//                             compiler ABORTED in the LLVM verifier;
//   "abc".LastIndexOf(5)  -> the same abort;
//   "abc".Replace(5, "x") -> the same abort;
//   "abc".Replace('a', 5) -> NO abort and NO diagnostic: the replacement was
//                             SILENTLY DROPPED and the program printed "bc"
//                             — a silent wrong answer.
//
// The rows below are on the strict AOT oracle (`runAot`): a rejection row
// asserts the SPECIFIC diagnostic (the dedicated one #6562 established for
// the same shape at `IndexOf`), and a control row asserts the RENDERED
// VALUE of every member that still works.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program every row runs: the call is the only statement, so a rejected
// call is visibly a no-op when it is (wrongly) accepted.
std::string prog(const std::string& stmt) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var s := \"abc\";\n"
           "    " + stmt + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& stmt, const std::string& needle) {
    auto r = runAot(prog(stmt));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A control row: the program must compile, run, and print exactly `out`.
void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    " + body + "\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The dedicated search-value diagnostic (#6562's wording, at the members
// #6635's title names). A generic assignability message would name `string`
// as the target of an overload set that is `char` OR `string`.
const char kSearchValue[] =
    "takes its search value at position 0 as a `char` or a `string` "
    "(stdlib §9.6); the argument's type converts to neither (§5.1)";

// The same diagnostic at the SECOND position of `Replace` — the replacement
// value, whose declared set is the same `char`-or-`string` pair.
const char kReplacementValue[] =
    "takes its replacement value at position 1 as a `char` or a `string` "
    "(stdlib §9.6); the argument's type converts to neither (§5.1)";

}  // namespace

// ── §9.6 / §5.1: an integer search value reaches no declared overload ──────

BEZEL_TEST(sema_string_search_value_int_6635, contains_int_search_value_rejected) {
    expectRejected("Console.WriteLine(s.Contains(5));", kSearchValue);
}

BEZEL_TEST(sema_string_search_value_int_6635,
           lastindexof_int_search_value_rejected) {
    expectRejected("Console.WriteLine(s.LastIndexOf(5));", kSearchValue);
}

BEZEL_TEST(sema_string_search_value_int_6635, replace_int_old_value_rejected) {
    expectRejected("Console.WriteLine(s.Replace(5, \"x\"));", kSearchValue);
}

// The SECOND position of `Replace` is the same declared set, and the same
// defect: the replacement was SILENTLY DROPPED and the program printed "bc".
BEZEL_TEST(sema_string_search_value_int_6635, replace_int_new_value_rejected) {
    expectRejected("Console.WriteLine(s.Replace('a', 5));", kReplacementValue);
}

// §5.1's table has no `long` -> `char` row either, and its constant-narrowing
// clause narrows an integer to an INTEGER target only.
BEZEL_TEST(sema_string_search_value_int_6635, contains_long_search_value_rejected) {
    expectRejected("var n: long := 5; Console.WriteLine(s.Contains(n));",
                   kSearchValue);
}

// A COMPILE-TIME CONSTANT is not an escape: the constant-narrowing clause
// applies to an INTEGER TARGET, and `char`/`string` are not integer targets.
BEZEL_TEST(sema_string_search_value_int_6635,
           contains_int_constant_search_value_rejected) {
    expectRejected("Console.WriteLine(s.Contains(5));", kSearchValue);
}

// ── CONTROLS: the declared forms still work, with their values pinned ──────

BEZEL_TEST(sema_string_search_value_int_6635, contains_string_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Contains(\"b\"));",
                 "True\n");
}

BEZEL_TEST(sema_string_search_value_int_6635, contains_char_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Contains('b'));",
                 "True\n");
}

BEZEL_TEST(sema_string_search_value_int_6635, lastindexof_string_control) {
    expectStdout("var s := \"abcabc\"; Console.WriteLine(s.LastIndexOf(\"b\"));",
                 "4\n");
}

BEZEL_TEST(sema_string_search_value_int_6635, lastindexof_char_control) {
    expectStdout("var s := \"abcabc\"; Console.WriteLine(s.LastIndexOf('b'));",
                 "4\n");
}

BEZEL_TEST(sema_string_search_value_int_6635, replace_string_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Replace(\"a\", \"x\"));",
                 "xbc\n");
}

BEZEL_TEST(sema_string_search_value_int_6635, replace_char_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Replace('a', 'x'));",
                 "xbc\n");
}

// `IndexOf` is #6562's own scope — its dedicated message is unchanged, and
// the control row pins that the fix did not disturb it.
BEZEL_TEST(sema_string_search_value_int_6635, indexof_string_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.IndexOf(\"b\"));",
                 "1\n");
}
