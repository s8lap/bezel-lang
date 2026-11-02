// Regression tests for issue #7983 — the #7933 arity gate must SEE a
// MODULE-QUALIFIED head (stdlib §7.3, §7.4, §22.6, v0.350).
//
// The gate keyed on a BARE identifier target, so a class written
// module-qualified — `System.IO.File.Exists(..)` — or through an import
// alias — `sio.File.Exists(..)` — BYPASSED it. The alias spelling compiled
// with only the #3079 "failed to lower ... emits nothing" warning, and an
// over-arity call was admitted where §22.6 makes it a compile error.
//
// WHAT THE LANGUAGE HAS, MEASURED. Neither qualified spelling resolves to
// the built-in `File` in this compiler: the module-qualified one reports
// "module `System` has no public type `IO`" and the aliased one degrades to
// the #3079 warning. That is a PRE-EXISTING gap in the module-path
// resolution, filed separately; it is NOT what this fix changes. What the
// fix changes is that the arity question is ASKED of the qualified head too,
// so the over-arity call is a compile error with the §22.6 diagnostic, and
// the DECLARED arity is not.
//
// Oracle: runAot. Reject rows assert the specific diagnostic; the control
// rows assert that the gate stands down at the declared arity.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& imports, const std::string& body) {
    return "module M;\nimport System;\nimport System.Collections.Generic;\n"
           "import System.IO;\n" + imports +
           "class P\n"
           "  public static method Main(): int\n" + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

bool hasError(const bezel::test::JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

BEZEL_TEST(builtin_fs_qualified_7983, aliased_file_exists_two_arguments_is_rejected) {
    // per stdlib §7.3 `Exists(path: string): bool` + §22.6: the aliased
    // spelling is a call at an arity no declared form takes, so it is a
    // compile error — not a call that compiles with a warning.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    Console.WriteLine(sio.File.Exists(\"a\", \"b\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the call to `File.Exists` supplies 2 argument(s) but stdlib §7.3 "
        "declares `Exists` with exactly 1 argument(s)"));
}

BEZEL_TEST(builtin_fs_qualified_7983, fully_qualified_file_exists_two_arguments_is_rejected) {
    // per stdlib §7.3 + §22.6: the module-qualified spelling is asked the
    // same arity question.
    auto r = runAot(prog("",
        "    Console.WriteLine(System.IO.File.Exists(\"a\", \"b\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the call to `File.Exists` supplies 2 argument(s) but stdlib §7.3 "
        "declares `Exists` with exactly 1 argument(s)"));
}

BEZEL_TEST(builtin_fs_qualified_7983, aliased_directory_delete_three_arguments_is_rejected) {
    // per stdlib §7.4 `Delete(path: string, recursive: bool := false)` +
    // §22.6: the aliased spelling of a member with an optional parameter.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    sio.Directory.Delete(\"a\", true, 1);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the call to `Directory.Delete` supplies 3 argument(s) but stdlib §7.4 "
        "declares `Delete` with 1 or 2 argument(s)"));
}

BEZEL_TEST(builtin_fs_qualified_7983, aliased_declared_arity_is_admitted) {
    // control (the admitting twin of the first row): the SAME shape at the
    // arity the standard library declares is admitted — the gate asks the
    // arity question and the answer is "a declared form takes it".
    auto r = runAot(prog("import System.IO as sio;\n",
        "    Console.WriteLine(sio.File.Exists(\"/nonexistent/x\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasError(r, "the call to `File.Exists` supplies"));
}

BEZEL_TEST(builtin_fs_qualified_7983, fully_qualified_declared_arity_draws_no_arity_error) {
    // control: the module-qualified spelling at the DECLARED arity draws no
    // arity error. (The qualified spelling does not resolve to the built-in
    // `File` in this compiler — a pre-existing gap in the module-path
    // resolution, filed separately — so this spelling reports THAT and
    // nothing else; the arity question is not asked of it.)
    auto r = runAot(prog("",
        "    Console.WriteLine(System.IO.File.Exists(\"/nonexistent/x\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(hasError(r, "the call to `File.Exists` supplies"));
    BEZEL_EXPECT_TRUE(hasError(r, "module `System` has no public type `IO`"));
}

BEZEL_TEST(builtin_fs_qualified_7983, bare_head_arity_is_unchanged) {
    // control: the bare spelling keeps the #7933 gate's own question, and a
    // bare call at the declared arity is admitted.
    auto r = runAot(prog("", "    Console.WriteLine(File.Exists(\"a\", \"b\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the call to `File.Exists` supplies 2 argument(s) but stdlib §7.3 "
        "declares `Exists` with exactly 1 argument(s)"));
}
