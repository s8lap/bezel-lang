// Regression tests for issue #8022 -- an ALIAS-QUALIFIED name that resolves to
// a type the aliased module does NOT home must be DIAGNOSED, exactly as the
// module-qualified and the type-position spellings are (spec §2.2, §2.7,
// v0.355).
//
// THE DEFECT, MEASURED on the pre-fix code (staging @ 8845ad4e).
//   `import System.IO as sio;` + `sio.NotAType.Exists("x");` COMPILED, ran,
//   and the statement emitted NOTHING (no diagnostic at any severity): the
//   expression arm of `ExprTC::resolveMemberAccess` fell through ("Alias known
//   but member is not a public type -- fall through") while its two siblings --
//   the module-qualified arm ("module `M` has no public type `X`") and the
//   alias TYPE-position resolver -- both speak. §2.2: "The alias is an
//   additional *qualifier*, not a rename", so the three spellings must not
//   differ in whether they report.
//
// Oracle: runAot. The rejected rows assert the specific diagnostic text AND
// that the program does not run (a silent drop would have run and printed
// "after"); the controls assert exact stdout.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotModules;

namespace {

std::string prog(const std::string& imports, const std::string& body) {
    return "module M;\n" + imports +
           "class P\n"
           "  public static method Main(): int\n" + body + "\n"
           "    Console.WriteLine(\"after\");\n"
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

BEZEL_TEST(alias_unhomed_member_8022, alias_unknown_type_call_head_is_rejected) {
    // per §2.2: `sio.NotAType` names no type of `System.IO`.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    sio.NotAType.Exists(\"x\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System.IO` has no public type `NotAType`"));
}

BEZEL_TEST(alias_unhomed_member_8022, alias_builtin_class_of_another_module_is_rejected) {
    // per §2.2 + stdlib §7.3: `File` is homed by `System.IO`, not `System`.
    auto r = runAot(prog("import System as st;\n",
        "    Console.WriteLine(st.File.Exists(\"x\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System` has no public type `File`"));
}

BEZEL_TEST(alias_unhomed_member_8022, module_qualified_unknown_type_is_rejected_same_text) {
    // control: the module-qualified spelling's text, which the alias reuses.
    auto r = runAot(prog("import System.IO;\n",
        "    System.IO.NotAType.Exists(\"x\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System.IO` has no public type `NotAType`"));
}

BEZEL_TEST(alias_unhomed_member_8022, alias_type_position_unknown_type_is_rejected) {
    // control: the alias TYPE-position form already diagnoses (§2.2).
    auto r = runAot(prog("import System.IO as sio;\n",
        "    var x: sio.NotAType := null;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System.IO` has no public type `NotAType`"));
}

BEZEL_TEST(alias_unhomed_member_8022, alias_homed_builtin_class_still_resolves) {
    // control (#7996): `sio.Path` homed by System.IO still resolves.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    Console.WriteLine(sio.Path.Combine(\"a\", \"b\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "a/b\nafter\n");
}

BEZEL_TEST(alias_unhomed_member_8022, alias_user_type_still_wins) {
    // per §2.7 rule 1: an alias of a module that exports a user type named
    // `File` selects THAT type, not a diagnostic.
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", R"BZ(
module Lib;
public static class File
  public static method Exists(p: string): int
    return 7;
  end Exists;
end File;
)BZ"});
    mods.push_back({"App", R"BZ(
module App;
import Lib as l;
class P
  public static method Main(): int
    Console.WriteLine(l.File.Exists("x"));
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}
