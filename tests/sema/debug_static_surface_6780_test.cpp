// Spec-derived regression test for issue #6780.
//
// stdlib §8.2 (v0.345) declares the whole surface of the `Debug` class — it
// is a pure-static utility class with exactly two members:
//
//     public static method Assert(condition: bool, message: string := "");
//     public static method WriteLine(message: string);
//
// `Debug` has NO user declaration, so that listing IS its surface, and
// §22.6 makes a call that matches no declared form a compile error with no
// candidates cited. Before the fix the `Debug` head was in NO built-in
// parameter table at all, so nothing checked a `Debug` call against §8.2.
//
// MEASURED BEFORE THE FIX (on 33a3912c), one-line programs:
//
//   var sb := new StringBuilder(); sb.Append("hello");
//   Debug.WriteLine(sb);   -> the program COMPILED and wrote an EMPTY LINE —
//                              the builder was not a `string` and nothing
//                              said so;
//   Debug.WriteLine(5);    -> "Call parameter type does not match function
//                              signature!" + "IR verification failed" — an
//                              `i32` handed to the runtime helper's `ptr`
//                              parameter: the compiler ABORTED;
//   Debug.WriteLine();     -> the program COMPILED and wrote an EMPTY LINE;
//   Debug.WriteLine("a","b") -> the program COMPILED and wrote an EMPTY LINE
//                              — the surplus argument was DROPPED;
//   Debug.WriteLine(null); -> the program COMPILED and wrote an EMPTY LINE;
//   Debug.Assert("x");     -> "Both operands to ICmp instruction are not of
//                              the same type!" + "IR verification failed" —
//                              the compiler ABORTED;
//   Debug.Assert(true, 5); -> the program COMPILED and did nothing.
//
// The rows below are on the strict AOT oracle (`runAot`): a rejection row
// asserts the SPECIFIC diagnostic, and a control row asserts the RENDERED
// VALUE of the declared forms that still work.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program every row runs: `Debug` lives in `System.Diagnostics`, which
// is not auto-imported (stdlib §2.3).
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
    auto r = runAot("module M;\n"
                    "import System.Diagnostics;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    " + body + "\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The §22.6 message of the ONE built-in admission gate, naming the declared
// form the argument misses.
const char kWriteLineType[] =
    "no matching overload of `Debug.WriteLine` — stdlib §8.2 declares "
    "`WriteLine(message: string)`, so the `message` argument must be a "
    "`string`";

const char kWriteLineArity[] =
    "no matching overload of `Debug.WriteLine` — the call supplies ";

const char kAssertConditionType[] =
    "no matching overload of `Debug.Assert` — stdlib §8.2 declares "
    "`Assert(condition: bool)`, so the `condition` argument must be a `bool`";

const char kAssertMessageType[] =
    "no matching overload of `Debug.Assert` — stdlib §8.2 declares "
    "`Assert(condition: bool, message: string)`, so the `message` argument "
    "must be a `string`";

// §7: `message: string` is non-nullable, so the `null` literal reaches it
// only as a compile error — the §7 gate owns that one, and the gate stands
// down rather than adding a second diagnostic to the same call.
const char kNullable[] =
    "the argument to `Debug.WriteLine` is nullable but the target type is "
    "non-nullable (§7)";

}  // namespace

// ── §8.2 / §22.6: the declared forms of `Debug` ───────────────────────────

// The issue's own repro: a `StringBuilder` is not a `string` (§5.1 has no
// `StringBuilder` -> `string` conversion), and before the fix the call
// compiled and wrote an EMPTY LINE.
BEZEL_TEST(sema_debug_static_surface_6780, writeline_stringbuilder_rejected) {
    expectRejected("var sb := new StringBuilder();\n"
                   "    sb.Append(\"hello\");\n"
                   "    Debug.WriteLine(sb);",
                   kWriteLineType);
}

// An `int` is not a `string` either — before the fix this ABORTED the
// compiler in the LLVM verifier.
BEZEL_TEST(sema_debug_static_surface_6780, writeline_int_rejected) {
    expectRejected("Debug.WriteLine(5);", kWriteLineType);
}

// §22.5/§22.6: `WriteLine` takes ONE argument. Before the fix the surplus
// argument was DROPPED and an empty line was written.
BEZEL_TEST(sema_debug_static_surface_6780, writeline_zero_args_rejected) {
    expectRejected("Debug.WriteLine();", kWriteLineArity);
}

BEZEL_TEST(sema_debug_static_surface_6780, writeline_two_args_rejected) {
    expectRejected("Debug.WriteLine(\"a\", \"b\");", kWriteLineArity);
}

// §7: `message: string` is non-nullable.
BEZEL_TEST(sema_debug_static_surface_6780, writeline_null_rejected) {
    expectRejected("Debug.WriteLine(null);", kNullable);
}

// `Assert`'s condition is a `bool` — before the fix a `string` condition
// ABORTED the compiler.
BEZEL_TEST(sema_debug_static_surface_6780, assert_string_condition_rejected) {
    expectRejected("Debug.Assert(\"x\");", kAssertConditionType);
}

// `Assert`'s message is a `string` — before the fix an `int` message was
// silently accepted and nothing happened.
BEZEL_TEST(sema_debug_static_surface_6780, assert_int_message_rejected) {
    expectRejected("Debug.Assert(true, 5);", kAssertMessageType);
}

// ── CONTROLS: the declared forms still work, with their values pinned ──────

BEZEL_TEST(sema_debug_static_surface_6780, writeline_string_control) {
    // `Debug.WriteLine` writes to the standard ERROR stream (the runtime's
    // own choice — stdlib §8.2 declares the member, not the stream), so the
    // program's STANDARD OUTPUT is empty and the row pins that the call is
    // compiled and runs.
    expectStdout("Debug.WriteLine(\"hello\");", "");
}

BEZEL_TEST(sema_debug_static_surface_6780, assert_true_control) {
    expectStdout("Debug.Assert(true);", "");
}

BEZEL_TEST(sema_debug_static_surface_6780, assert_true_message_control) {
    expectStdout("Debug.Assert(true, \"all good\");", "");
}
