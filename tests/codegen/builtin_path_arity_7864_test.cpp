// Regression tests for issue #7864 — `Path.Combine` with 1 or 3 arguments
// compiled, with only a "failed to lower ... emits nothing" warning.
//
// stdlib §7.4 (v0.350) declares:
//
//     public static class Path
//       public static method Combine(a: string, b: string): string;
//       public static method GetFileName(path: string): string;
//       public static method GetExtension(path: string): string;
//       ...
//
// A `Path` call at a count no declared form takes reached no lowering. It is a
// compile error with no candidates (§22.6). The REJECT rows assert the
// specific diagnostic; the ACCEPT row prints the value of every declared form.
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

}  // namespace

BEZEL_TEST(builtin_path_arity_7864, combine_one_argument_is_rejected) {
    // per stdlib §7.4 (v0.350) `Combine(a: string, b: string): string`
    expectArityRejected("", "    Console.WriteLine(Path.Combine(\"x\"));",
                        "Path.Combine", 1, "exactly 2");
}

BEZEL_TEST(builtin_path_arity_7864, combine_three_arguments_is_rejected) {
    // per stdlib §7.4 (v0.350)
    expectArityRejected("", "    Console.WriteLine(Path.Combine(\"x\", \"y\", \"z\"));",
                        "Path.Combine", 3, "exactly 2");
}

BEZEL_TEST(builtin_path_arity_7864, getfilename_two_arguments_is_rejected) {
    // per stdlib §7.4 (v0.350) `GetFileName(path: string): string`
    expectArityRejected("", "    Console.WriteLine(Path.GetFileName(\"a/b\", \"c\"));",
                        "Path.GetFileName", 2, "exactly 1");
}

BEZEL_TEST(builtin_path_arity_7864, getextension_no_arguments_is_rejected) {
    // per stdlib §7.4 (v0.350) `GetExtension(path: string): string`
    expectArityRejected("", "    Console.WriteLine(Path.GetExtension());",
                        "Path.GetExtension", 0, "exactly 1");
}

BEZEL_TEST(builtin_path_arity_7864, declared_arities_run) {
    // per stdlib §7.4 (v0.350): Combine takes 2, the others take 1
    expectStdout("",
                 "    Console.WriteLine(Path.Combine(\"a\", \"b\"));\n"
                 "    Console.WriteLine(Path.GetFileName(\"a/b.txt\"));\n"
                 "    Console.WriteLine(Path.GetExtension(\"a/b.txt\"));\n"
                 "    Console.WriteLine(Path.GetFileNameWithoutExtension(\"a/b.txt\"));\n"
                 "    Console.WriteLine(Path.GetDirectoryName(\"a/b.txt\"));",
                 "a/b\nb.txt\n.txt\nb\na\n");
}
