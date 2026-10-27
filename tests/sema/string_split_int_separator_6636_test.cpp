// Spec-derived regression test for issue #6636.
//
// stdlib §9.6 (v0.345) declares `Split` with three overloads —
// `Split(separator: char)`, `Split(separator: string)` and
// `Split(separators: Array<char>)` — and NO `int` form. §5.1 converts no
// integer type to a `char` or to a `string` (its table has no such row, and
// its constant-narrowing clause narrows an integer to an INTEGER target
// only), so an `int` separator reaches NO declared overload: §22.6 makes the
// call a compile error with no candidates cited.
//
// BEFORE THE FIX (measured on 33a3912c):
//
//   "a,b,c".Split(5) -> the program COMPILED, with two `internal:` warnings
//                        ("codegen cannot dispatch member 'Split' on receiver
//                        of type '': permissive acceptance …" and "the
//                        argument of this `Console.WriteLine` failed to lower
//                        to a value"), and PRINTED NOTHING AT ALL — not even
//                        the line terminator. The call was admitted, its
//                        result was a zero-init fallback slot, and the
//                        program said nothing where it owed a count.
//
// The rows below are on the strict AOT oracle (`runAot`): a rejection row
// asserts the SPECIFIC diagnostic, and a control row asserts the RENDERED
// VALUE of every declared `Split` form that still works.
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
           "    var s := \"a,b,c\";\n"
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

// The dedicated diagnostic for a separator that reaches no declared
// overload. `Split`'s declared set has THREE members, so the reason is
// "none of them" rather than #6635's "neither".
const char kSeparator[] =
    "takes its separator at position 0 as a `char`, a `string` or an "
    "`Array<char>` (stdlib §9.6); the argument's type converts to none of "
    "them (§5.1)";

}  // namespace

// ── §9.6 / §5.1: an integer separator reaches no declared overload ─────────

BEZEL_TEST(sema_string_split_int_separator_6636, split_int_separator_rejected) {
    expectRejected("Console.WriteLine(s.Split(5).Count);", kSeparator);
}

// A COMPILE-TIME CONSTANT is not an escape: §5.1's constant-narrowing
// clause applies to an INTEGER TARGET, and `char`/`string` are not integer
// targets.
BEZEL_TEST(sema_string_split_int_separator_6636,
           split_int_constant_separator_rejected) {
    expectRejected("Console.WriteLine(s.Split(5).Count);", kSeparator);
}

// §5.1's table has no `long` -> `char` row either.
BEZEL_TEST(sema_string_split_int_separator_6636, split_long_separator_rejected) {
    expectRejected("var n: long := 5; Console.WriteLine(s.Split(n).Count);",
                   kSeparator);
}

// ── CONTROLS: the declared forms still work, with their values pinned ──────

BEZEL_TEST(sema_string_split_int_separator_6636, split_string_separator_control) {
    expectStdout("var s := \"a,b,c\"; Console.WriteLine(s.Split(\",\").Count);",
                 "3\n");
}

BEZEL_TEST(sema_string_split_int_separator_6636, split_char_separator_control) {
    expectStdout("var s := \"a,b,c\"; Console.WriteLine(s.Split(',').Count);",
                 "3\n");
}
