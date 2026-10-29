// Regression tests for issue #7810 — a call to a built-in `Array<T>` member at
// an argument count the standard library declares no overload for compiled
// silently (`a.Contains(1, 2)`, `a.IndexOf(1, 2, 3)`).
//
// stdlib §4.4 (v0.350) declares on `Array<T>`:
//
//     public method Contains(item: T): bool;
//     public method IndexOf(item: T, startIndex: int := 0): int;   // 1 or 2 arguments
//     public method LastIndexOf(item: T): int;
//
// A built-in member has no MethodSymbol, so nothing counted the arguments; the
// lowerings key on name and arity, so the call matched no lowering (the #3079
// / #2434 warning backstops: a warning and nothing emitted). Spec §26.5 rule 1
// makes the instance member the only candidate by name at any arity, so a
// count no declared overload takes has no candidate: a compile error (§22.6).
//
// The REJECT rows assert the specific diagnostic (member, supplied count,
// declared count). The ACCEPT row reads the data: every declared arity runs
// and prints its value, so an over-broad rejection cannot hide.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\nimport System;\nimport System.Collections.Generic;\n"
           "import System.IO;\n" + decls +
           "class P\n"
           "  public static method Main(): int\n" + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

// The program is rejected, with an Error that names `callee`, the supplied
// count and the declared count.
void expectArityRejected(const std::string& decls, const std::string& body,
                         const std::string& callee, int supplied,
                         const std::string& declared) {
    auto r = runAot(prog(decls, body));
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string want = "the call to `" + callee + "` supplies " +
                             std::to_string(supplied) + " argument(s)";
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(want) != std::string::npos &&
            d.message.find("with " + declared + " argument(s)") !=
                std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

void expectStdout(const std::string& decls, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(decls, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char* kArr = "    var a: Array<int> := [1, 2, 1];\n";

}  // namespace

// ---- #7810: the Array members ---------------------------------------------

BEZEL_TEST(builtin_collection_arity_7810, array_contains_two_args_is_rejected) {
    // per stdlib §4.4 (v0.350) `Contains(item: T)`, spec §26.5 rule 1, §22.6
    expectArityRejected("", std::string(kArr) + "    Console.WriteLine(a.Contains(1, 2));",
                        "Array.Contains", 2, "exactly 1");
}

BEZEL_TEST(builtin_collection_arity_7810, array_indexof_three_args_is_rejected) {
    // per stdlib §4.4 (v0.350) `IndexOf(item: T, startIndex: int := 0)`: 1 or 2
    expectArityRejected("", std::string(kArr) + "    Console.WriteLine(a.IndexOf(1, 2, 3));",
                        "Array.IndexOf", 3, "1 or 2");
}

BEZEL_TEST(builtin_collection_arity_7810, array_indexof_zero_args_is_rejected) {
    // per stdlib §4.4 (v0.350): `item` has no default
    expectArityRejected("", std::string(kArr) + "    Console.WriteLine(a.IndexOf());",
                        "Array.IndexOf", 0, "1 or 2");
}

BEZEL_TEST(builtin_collection_arity_7810, array_contains_statement_position_is_rejected) {
    // per stdlib §4.4 (v0.350): the statement form compiled with only the
    // unused-result warning
    expectArityRejected("", std::string(kArr) + "    a.Contains();",
                        "Array.Contains", 0, "exactly 1");
}

BEZEL_TEST(builtin_collection_arity_7810, array_null_conditional_spelling_is_rejected) {
    // per stdlib §4.4 (v0.350) + spec §7.5: `?.` reaches the same member
    expectArityRejected("", "    var a: Array<int>? := [1, 2];\n"
                            "    Console.WriteLine(a?.Contains(1, 2) ?? false);",
                        "Array.Contains", 2, "exactly 1");
}

// ONE diagnostic: `LastIndexOf` already drew an arity error from the #7671
// arm; the new gate stands down rather than reporting the call twice.
BEZEL_TEST(builtin_collection_arity_7810, array_lastindexof_two_args_draws_one_diagnostic) {
    // per stdlib §4.4 (v0.350) `LastIndexOf(item: T)`
    auto r = runAot(prog("", std::string(kArr) + "    Console.WriteLine(a.LastIndexOf(1, 2));"));
    BEZEL_EXPECT_FALSE(r.ran);
    // the #7671 arm's own wording, once; the new gate's wording, never
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(
                        r, "no member `LastIndexOf` taking 2 argument(s)"), 1);
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(
                        r, "the call to `Array.LastIndexOf`"), 0);
}

// Every DECLARED Array arity still runs, and reads its data.
BEZEL_TEST(builtin_collection_arity_7810, array_declared_arities_run) {
    // per stdlib §4.4 (v0.350): IndexOf 1 or 2 (startIndex), Contains 1,
    // LastIndexOf 1, Insert 2, RemoveAt 1, Remove 1
    expectStdout("",
                 "    var a: Array<int> := [1, 2, 1, 3];\n"
                 "    Console.WriteLine(a.IndexOf(1));\n"
                 "    Console.WriteLine(a.IndexOf(1, 1));\n"
                 "    Console.WriteLine(a.LastIndexOf(1));\n"
                 "    Console.WriteLine(a.Contains(3));\n"
                 "    a.Insert(0, 9);\n"
                 "    a.RemoveAt(1);\n"
                 "    Console.WriteLine(a.Remove(9));\n"
                 "    Console.WriteLine(a.Count);",
                 "0\n2\n2\nTrue\nTrue\n3\n");
}

