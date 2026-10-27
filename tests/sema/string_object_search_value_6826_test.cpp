// Spec-derived regression test for issue #6826.
//
// stdlib §9.6 (v0.345) declares the `string` members' search values as a
// `char`-or-`string` overload set (and, for `Split`, `Array<char>` as well):
// at those positions NOTHING ELSE is declared.
//
// §5.1's reference conversions run the OTHER way — "any type to `Object`" is
// implicit, and `Object` to a `string` is a DOWNCAST, which §5.1 never makes
// implicit — so an `Object` argument reaches no declared overload either.
// §22.6 makes the call a compile error with no candidates cited.
//
// BEFORE THE FIX (measured on 33a3912c):
//
//   "abc".Contains(new Object()) -> the program COMPILED and printed "True";
//   var o: Object := "l";
//   "abc".Contains(o)            -> the program COMPILED and printed "False";
//   "abc".LastIndexOf(o)         -> the program COMPILED and printed "-1";
//   "abc".Replace(o, "x")        -> the program COMPILED and printed "abc"
//                                    — the replacement was SILENTLY DROPPED;
//   "a,b,c".Split(o)             -> the program COMPILED with two
//                                    `internal:` warnings and PRINTED
//                                    NOTHING AT ALL.
//
// A call that reaches no declared overload and is nevertheless ADMITTED is
// the silent-acceptance inversion: the program answers a plausible wrong
// value (or says nothing at all) where the standard library makes it a
// compile error.
//
// FAMILY MEMBERS. The same declared shape admits the same defect for every
// other argument type the declared set does not name — `bool` is the sharpest
// one (`"abc".Contains(true)` ABORTED the compiler in the LLVM verifier with
// "Call parameter type does not match function signature!"), and `float`,
// `double` and `decimal` behave like it. Those rows are here too: the fix is
// one rule ("the argument's type must be one the declared overload set
// names"), not a list of types.
//
// The rows below are on the strict AOT oracle (`runAot`): a rejection row
// asserts the SPECIFIC diagnostic, and a control row asserts the RENDERED
// VALUE of every declared form that still works.
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

// The dedicated diagnostic for a search value that reaches no declared
// overload (the wording #6562 established and #6635 generalised).
const char kSearchValue[] =
    "takes its search value at position 0 as a `char` or a `string` "
    "(stdlib §9.6); the argument's type converts to neither (§5.1)";

// The same diagnostic at the SECOND position of `Replace` — the replacement
// value.
const char kReplacementValue[] =
    "takes its replacement value at position 1 as a `char` or a `string` "
    "(stdlib §9.6); the argument's type converts to neither (§5.1)";

// The same diagnostic at `Split`'s separator, whose declared set has three
// members.
const char kSeparator[] =
    "takes its separator at position 0 as a `char`, a `string` or an "
    "`Array<char>` (stdlib §9.6); the argument's type converts to none of "
    "them (§5.1)";

}  // namespace

// ── §9.6 / §5.1: an `Object` argument reaches no declared overload ─────────

BEZEL_TEST(sema_string_object_search_value_6826,
           contains_new_object_search_value_rejected) {
    expectRejected("Console.WriteLine(s.Contains(new Object()));", kSearchValue);
}

BEZEL_TEST(sema_string_object_search_value_6826,
           contains_object_search_value_rejected) {
    expectRejected("var o: Object := \"l\"; Console.WriteLine(s.Contains(o));",
                   kSearchValue);
}

BEZEL_TEST(sema_string_object_search_value_6826,
           lastindexof_object_search_value_rejected) {
    expectRejected(
        "var o: Object := \"l\"; Console.WriteLine(s.LastIndexOf(o));",
        kSearchValue);
}

BEZEL_TEST(sema_string_object_search_value_6826,
           replace_object_old_value_rejected) {
    expectRejected(
        "var o: Object := \"l\"; Console.WriteLine(s.Replace(o, \"x\"));",
        kSearchValue);
}

// The SECOND position of `Replace` — the replacement value — is the same
// declared set: before the fix the replacement was SILENTLY DROPPED and the
// program printed "abc".
BEZEL_TEST(sema_string_object_search_value_6826,
           replace_object_new_value_rejected) {
    expectRejected(
        "var o: Object := \"l\"; Console.WriteLine(s.Replace(\"a\", o));",
        kReplacementValue);
}

BEZEL_TEST(sema_string_object_search_value_6826,
           split_object_separator_rejected) {
    expectRejected(
        "var o: Object := \",\"; Console.WriteLine(s.Split(o).Count);",
        kSeparator);
}

// A §7.4-narrowed `Object?` read is of its NON-NULLABLE type — `Object` —
// and `Object` is not a `char` or a `string` either.
BEZEL_TEST(sema_string_object_search_value_6826,
           contains_narrowed_object_search_value_rejected) {
    expectRejected(
        "var o: Object? := \"l\";\n"
        "    if o is null then return 1; end;\n"
        "    Console.WriteLine(s.Contains(o));",
        kSearchValue);
}

// ── FAMILY MEMBERS: the other types the declared set does not name ─────────

// `bool` ABORTED the compiler before the fix ("Call parameter type does not
// match function signature!" — an `i1` handed to a `ptr` parameter).
BEZEL_TEST(sema_string_object_search_value_6826,
           contains_bool_search_value_rejected) {
    expectRejected("Console.WriteLine(s.Contains(true));", kSearchValue);
}

BEZEL_TEST(sema_string_object_search_value_6826,
           lastindexof_double_search_value_rejected) {
    expectRejected("Console.WriteLine(s.LastIndexOf(1.5));", kSearchValue);
}

// ── CONTROLS: the declared forms still work, with their values pinned ──────

BEZEL_TEST(sema_string_object_search_value_6826, contains_string_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Contains(\"b\"));",
                 "True\n");
}

BEZEL_TEST(sema_string_object_search_value_6826, contains_char_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Contains('b'));",
                 "True\n");
}

BEZEL_TEST(sema_string_object_search_value_6826, replace_string_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Replace(\"a\", \"x\"));",
                 "xbc\n");
}
