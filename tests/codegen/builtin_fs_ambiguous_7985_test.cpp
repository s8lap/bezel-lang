// Regression tests for issue #7985 — the #7933 arity gate must STAND DOWN on
// an ambiguous bare head (spec §2.3 / §2.2, v0.350; #5140).
//
// §2.3: "a same-named public exported by an explicitly imported module does
// not shadow it but makes the bare name **ambiguous** in that file under
// §2.2's collision rule — every row alike". So when an imported module
// exports a type named `Environment` (an always-available name — §2.3's
// auto-imported-module row lists `Environment` among the members of the
// auto-imported `System`), the bare name `Environment` is ambiguous, and the
// compiler reports THAT. The arity gate added a SECOND error on the same
// access — "the call to `Environment.Exit` supplies 0 argument(s) but stdlib
// §2.4 declares `Exit` with exactly 1 argument(s)" — which is misleading:
// the call's callee is not resolved yet, so its argument count is not the
// question.
//
// `File` and `Directory` are NOT always-available (§2.3: they live in
// `System.IO`, which requires an explicit `import System.IO;`), so a user
// module that exports one of them SHADOWS the standard library's — the bare
// name is not ambiguous, and the arity gate's own question is the right one.
// The control row below pins that.
//
// Oracle: runAotModules. Reject rows assert the specific diagnostic.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::ModuleSource;
using bezel::test::runAotModules;

namespace {

bool hasError(const bezel::test::JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

std::size_t errorCount(const bezel::test::JitResult& r) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++n;
    return n;
}

}  // namespace

BEZEL_TEST(builtin_fs_ambiguous_7985, ambiguous_environment_head_gets_only_the_ambiguity_error) {
    // per §2.3 / §2.2 (v0.350) and #5140: the bare `Environment` is
    // ambiguous, so THAT is the one diagnostic the access draws. The #7933
    // arity gate stands down (§2.3: the ambiguity is already reported).
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", R"BZ(
module Lib;
public static class Environment
  public static method Exit(): int
    return 9;
  end Exit;
end Environment;
)BZ"});
    mods.push_back({"App", R"BZ(
module App;
import System;
import Lib;
class P
  public static method Main(): int
    Console.WriteLine(Environment.Exit());
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_FALSE(r.ran);
    // The ambiguity is reported ...
    BEZEL_EXPECT_TRUE(hasError(r, "ambiguous"));
    // ... and it is the ONLY error: no second, misleading arity error.
    BEZEL_EXPECT_FALSE(hasError(r, "the call to `Environment.Exit` supplies"));
    BEZEL_EXPECT_EQ(errorCount(r), static_cast<std::size_t>(1));
}

BEZEL_TEST(builtin_fs_ambiguous_7985, ambiguous_environment_head_with_arguments_is_not_an_arity_error) {
    // per §2.3 / §2.2 (v0.350): the same head, called at an arity the
    // standard library does NOT declare (stdlib §2.4 declares
    // `Exit(exitCode: int)`) — the gate stands down there too, so the
    // ambiguity is the only diagnostic.
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", R"BZ(
module Lib;
public static class Environment
  public static method Exit(): int
    return 9;
  end Exit;
end Environment;
)BZ"});
    mods.push_back({"App", R"BZ(
module App;
import System;
import Lib;
class P
  public static method Main(): int
    Console.WriteLine(Environment.Exit(7, 8));
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "ambiguous"));
    BEZEL_EXPECT_FALSE(hasError(r, "the call to `Environment.Exit` supplies"));
    BEZEL_EXPECT_EQ(errorCount(r), static_cast<std::size_t>(1));
}

BEZEL_TEST(builtin_fs_ambiguous_7985, user_module_file_shadows_the_stdlib_file) {
    // control: `File` is NOT an always-available name (stdlib §7.3's `File`
    // lives in `System.IO`, which §2.3 requires an explicit import for), so a
    // user module that exports `File` SHADOWS the standard library's rather
    // than making the bare name ambiguous — and the user's own member is
    // called. The file imports no `System.IO`, so there is no collision to
    // be ambiguous about.
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", R"BZ(
module Lib;
public static class File
  public static method Exists(a: int, b: int): int
    return a + b;
  end Exists;
end File;
)BZ"});
    mods.push_back({"App", R"BZ(
module App;
import System;
import Lib;
class P
  public static method Main(): int
    Console.WriteLine(File.Exists(1, 2));
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

BEZEL_TEST(builtin_fs_ambiguous_7985, unambiguous_environment_head_keeps_the_arity_error) {
    // control: with no imported `Environment`, the bare head IS the
    // standard library's, and the #7933 arity gate keeps its own question.
    auto r = bezel::test::runAot(
        "module M;\nimport System;\n"
        "class P\n"
        "  public static method Main(): int\n"
        "    Environment.Exit();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the call to `Environment.Exit` supplies 0 argument(s) but stdlib §2.4 "
        "declares `Exit` with exactly 1 argument(s)"));
    BEZEL_EXPECT_EQ(errorCount(r), static_cast<std::size_t>(1));
}
