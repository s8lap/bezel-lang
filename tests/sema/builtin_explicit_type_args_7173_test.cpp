// Spec-derived regression test for issue #7173.
//
// Spec §14.5 "Explicit lists are all-or-nothing" (v0.345): "An explicit
// type-argument list (`obj.Method<int>(args)`) binds the declared type
// parameters in declaration order and must supply exactly as many arguments
// as the method declares type parameters. A shorter or a longer list is a
// compile error; there are no partial lists."
//
// A BUILT-IN member has no MethodSymbol, so the compiler's own §14.5 check
// (the one that reads the callee's declared type-parameter count) never ran
// for it, and the list was simply IGNORED. Every member of stdlib §9.6
// (`string`), §3.1 (`StringBuilder`) and §8.2 (`Debug`) declares NO type
// parameter, so any explicit list at one of them is a compile error.
//
// MEASURED BEFORE THE FIX (on 33a3912c), one-line programs:
//
//   Debug.WriteLine<int>("x");        -> the program COMPILED and ran;
//   Debug.Assert<int>(true);          -> the program COMPILED and ran;
//   "a,b".Split<int>(',');            -> the program COMPILED and ran;
//   "abc".Contains<int>('b');         -> the program COMPILED and ran;
//   string.Join<int>(",", xs);        -> the program COMPILED and ran;
//   new StringBuilder().Append<int>(5) -> the program COMPILED and ran;
//
// The list was accepted and IGNORED — a type argument the member does not
// declare, silently dropped.
//
// The rows below are on the strict AOT oracle (`runAot`): a rejection row
// asserts the SPECIFIC diagnostic, and a control row asserts the RENDERED
// VALUE of the same call without the list.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program every row runs.
std::string prog(const std::string& stmt) {
    return "module M;\n"
           "import System.Diagnostics;\n"
           "class C\n"
           "  public static method Main(): int\n"
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
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The §14.5 rejection of an explicit list on a member that declares no type
// parameter — the `Math` head's own sentence (stdlib §2.2's `Math` reports
// the same shape), so the two built-in heads say the same thing.
const char kNoTypeParams[] =
    "explicit type argument(s), but it declares no type parameters";

}  // namespace

// ── §8.2 `Debug`: neither member declares a type parameter ──────────────────

BEZEL_TEST(sema_builtin_explicit_type_args_7173,
           debug_writeline_one_type_arg_rejected) {
    expectRejected("Debug.WriteLine<int>(\"x\");",
                   "`Debug.WriteLine` is given 1 " + std::string(kNoTypeParams));
}

BEZEL_TEST(sema_builtin_explicit_type_args_7173,
           debug_writeline_two_type_args_rejected) {
    expectRejected("Debug.WriteLine<int, string>(\"x\");",
                   "`Debug.WriteLine` is given 2 " + std::string(kNoTypeParams));
}

BEZEL_TEST(sema_builtin_explicit_type_args_7173,
           debug_assert_one_type_arg_rejected) {
    expectRejected("Debug.Assert<int>(true);",
                   "`Debug.Assert` is given 1 " + std::string(kNoTypeParams));
}

BEZEL_TEST(sema_builtin_explicit_type_args_7173,
           debug_assert_two_type_args_rejected) {
    expectRejected("Debug.Assert<int, string>(true, \"m\");",
                   "`Debug.Assert` is given 2 " + std::string(kNoTypeParams));
}

// ── §9.6 `string`: no member declares a type parameter ──────────────────────

BEZEL_TEST(sema_builtin_explicit_type_args_7173,
           string_split_one_type_arg_rejected) {
    expectRejected("var s := \"a,b\"; Console.WriteLine(s.Split<int>(','));",
                   "`string.Split` is given 1 " + std::string(kNoTypeParams));
}

BEZEL_TEST(sema_builtin_explicit_type_args_7173,
           string_contains_one_type_arg_rejected) {
    expectRejected("var s := \"abc\"; Console.WriteLine(s.Contains<int>('b'));",
                   "`string.Contains` is given 1 " + std::string(kNoTypeParams));
}

BEZEL_TEST(sema_builtin_explicit_type_args_7173,
           string_join_one_type_arg_rejected) {
    expectRejected("var xs: Array<string> := [\"a\", \"b\"];\n"
                   "    Console.WriteLine(string.Join<int>(',', xs));",
                   "`string.Join` is given 1 " + std::string(kNoTypeParams));
}

BEZEL_TEST(sema_builtin_explicit_type_args_7173,
           string_repeat_one_type_arg_rejected) {
    expectRejected("Console.WriteLine(string.Repeat<int>('a', 2));",
                   "`string.Repeat` is given 1 " + std::string(kNoTypeParams));
}

// ── §3.1 `StringBuilder`: no member declares a type parameter ───────────────

BEZEL_TEST(sema_builtin_explicit_type_args_7173,
           stringbuilder_append_one_type_arg_rejected) {
    expectRejected("var sb := new StringBuilder(); sb.Append<int>(5);",
                   "`StringBuilder.Append` is given 1 " + std::string(kNoTypeParams));
}

BEZEL_TEST(sema_builtin_explicit_type_args_7173,
           stringbuilder_appendline_one_type_arg_rejected) {
    expectRejected("var sb := new StringBuilder(); sb.AppendLine<int>(\"x\");",
                   "`StringBuilder.AppendLine` is given 1 " +
                       std::string(kNoTypeParams));
}

// ── CONTROLS: the same calls WITHOUT the list still work ───────────────────

BEZEL_TEST(sema_builtin_explicit_type_args_7173, string_split_control) {
    // "a,b" split at a `char` separator yields TWO parts.
    expectStdout("var s := \"a,b\"; Console.WriteLine(s.Split(',').Count);",
                 "2\n");
}

BEZEL_TEST(sema_builtin_explicit_type_args_7173, string_join_control) {
    expectStdout("var xs: Array<string> := [\"a\", \"b\"];\n"
                 "    Console.WriteLine(string.Join(\",\", xs));",
                 "a,b\n");
}

BEZEL_TEST(sema_builtin_explicit_type_args_7173, stringbuilder_append_control) {
    expectStdout("var sb := new StringBuilder(); sb.Append(\"hi\");\n"
                 "    Console.WriteLine(sb.Length);",
                 "2\n");
}

BEZEL_TEST(sema_builtin_explicit_type_args_7173, debug_writeline_control) {
    // `Debug.WriteLine` writes to the standard ERROR stream, so the
    // program's standard output is empty; the row pins that the call still
    // compiles and runs.
    expectStdout("Debug.WriteLine(\"hello\");", "");
}
