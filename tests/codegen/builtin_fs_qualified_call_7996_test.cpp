// Regression tests for issue #7996 — a MODULE-QUALIFIED or ALIAS-QUALIFIED
// static call to `File` / `Directory` / `Path` / `Environment` must resolve
// to the built-in static class, and behave EXACTLY like the bare spelling
// (spec §2.2, §2.7, stdlib §2.4 / §7.3 / §7.4, v0.350).
//
// THE DEFECT, MEASURED on the pre-fix code.
//   `import System.IO;` + `System.IO.File.Exists("a")` was rejected with
//     "module `System.IO` has no public type `File` (§2.2 / §13.2)"
//   `import System.IO as sio;` + `sio.File.Exists("a")` COMPILED and emitted
//     NOTHING — the #3079/#2434 "failed to lower" degradation — a silent drop,
//     the worst outcome.
//   `import System;` + `System.Environment.Get("X")` behaved the same way.
//
// WHY. stdlib §7.3 declares `public static class File` in the `System.IO`
// module and §7.4 declares `Directory` and `Path` there; §2.4 declares
// `Environment` in `System`. §2.7 rule 2: "match the longest dotted prefix of
// the chain against an imported module's full name (or the current module's).
// The remainder then resolves against that module's public declarations" — so
// `System.IO.File` names `System.IO`'s `File`. §2.2's alias rule: "The alias
// is an additional *qualifier*, not a rename" — so `sio.File` names the SAME
// `File`. The resolver looked the remainder up among the module's EXPORTED
// TypeSymbols only, and a built-in static class is not an exported type, so
// both spellings missed it.
//
// WHAT THE FIX DOES. The qualified head is admitted and then REWRITTEN to the
// bare spelling, so every consumer downstream — the §22.6 arity gate, the
// §22.5 named-argument binder, the §7 nullability gate, the closed-surface
// reject, and codegen's static-call ladder — sees the bare head and answers
// identically. That is why the rows below assert the WHOLE behaviour, not
// merely that the program compiles.
//
// Oracle: runAot. The admitted rows assert the program's exact stdout; the
// rejected rows assert the specific diagnostic.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotModules;

namespace {

// A program in module `M` with the given imports and body.
std::string prog(const std::string& imports, const std::string& body) {
    return "module M;\n" + imports +
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

std::size_t errorCount(const bezel::test::JitResult& r) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++n;
    return n;
}

// A module whose public `Environment` makes the BARE name `Environment`
// ambiguous in the importing file (§2.3: "a same-named public exported by an
// explicitly imported module does not shadow it but makes the bare name
// **ambiguous** in that file under §2.2's collision rule").
const char* kAmbiguousEnvironmentLib = R"BZ(
module Lib;
public static class Environment
  public static method Get(name: string): string
    return "USER";
  end Get;
end Environment;
)BZ";

}  // namespace

// --- the module-qualified spelling (§2.7 rule 2) ---------------------------

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_file_exists_is_admitted) {
    // per stdlib §7.3 `Exists(path: string): bool` + §2.7 rule 2: the
    // module-qualified head names `System.IO`'s `File`, so the call is the
    // built-in's and ANSWERS — it is not a rejected name and not a dropped
    // statement. The path does not exist, so the answer is `False`.
    auto r = runAot(prog("import System.IO;\n",
        "    Console.WriteLine(System.IO.File.Exists(\"/nonexistent/x7996\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_path_combine_is_admitted) {
    // per stdlib §7.4 `Combine(a, b): string` + §2.7 rule 2: the same
    // qualified head, a member that RETURNS a value.
    auto r = runAot(prog("import System.IO;\n",
        "    Console.WriteLine(System.IO.Path.Combine(\"a\", \"b\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "a/b\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_directory_exists_is_admitted) {
    // per stdlib §7.4 `Directory.Exists(path: string): bool` + §2.7 rule 2.
    auto r = runAot(prog("import System.IO;\n",
        "    Console.WriteLine(System.IO.Directory.Exists(\"/nonexistent/x7996\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_environment_is_admitted) {
    // per stdlib §2.4 `Set(name, value)` / `Get(name): string?` + §2.7 rule 2:
    // `Environment` is a `System` module's public declaration, so
    // `System.Environment.Set(..)` names it.
    auto r = runAot(prog("import System;\n",
        "    System.Environment.Set(\"BEZEL_7996\", \"42\");\n"
        "    var v := System.Environment.Get(\"BEZEL_7996\");\n"
        "    Console.WriteLine(v);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// --- the alias-qualified spelling (§2.2) -----------------------------------

BEZEL_TEST(builtin_fs_qualified_call_7996, alias_qualified_file_exists_is_admitted) {
    // per §2.2 ("the alias is an additional qualifier, not a rename") + stdlib
    // §7.3: `sio.File` names `System.IO`'s `File`. Pre-fix this compiled and
    // emitted NOTHING — the row asserts the ANSWER, so it is red on the
    // pre-fix code.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    Console.WriteLine(sio.File.Exists(\"/nonexistent/x7996\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, alias_qualified_path_combine_is_admitted) {
    // per §2.2 + stdlib §7.4.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    Console.WriteLine(sio.Path.Combine(\"a\", \"b\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "a/b\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, alias_qualified_directory_exists_is_admitted) {
    // per §2.2 + stdlib §7.4.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    Console.WriteLine(sio.Directory.Exists(\"/nonexistent/x7996\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, alias_qualified_environment_is_admitted) {
    // per §2.2 + stdlib §2.4: the alias names the `System` module, whose
    // public declaration `Environment` the alias reaches.
    auto r = runAot(prog("import System as sys;\n",
        "    sys.Environment.Set(\"BEZEL_7996\", \"42\");\n"
        "    var v := sys.Environment.Get(\"BEZEL_7996\");\n"
        "    Console.WriteLine(v);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// --- the qualified spelling behaves LIKE the bare one ----------------------

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_unknown_member_is_rejected) {
    // per stdlib §7.3's documented surface + §2.7 (a qualified name denotes
    // the SAME entity as the bare one): `File` declares no `NotAMember`, so
    // the qualified spelling is refused by the SAME closed-surface reject the
    // bare spelling draws — NOT silently dropped, and NOT reported as "no
    // public type `File`" (the head DOES resolve).
    auto r = runAot(prog("import System.IO;\n",
        "    System.IO.File.NotAMember(\"a\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "no member `NotAMember` on `File`"));
    BEZEL_EXPECT_FALSE(hasError(r, "has no public type"));
}

BEZEL_TEST(builtin_fs_qualified_call_7996, alias_qualified_unknown_member_is_rejected) {
    // the same, through the alias. Pre-fix this compiled and emitted nothing.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    sio.File.NotAMember(\"a\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "no member `NotAMember` on `File`"));
    BEZEL_EXPECT_FALSE(hasError(r, "has no public type"));
}

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_named_argument_is_honoured) {
    // per §22.5 ("naming an argument changes which parameter it binds to")
    // + stdlib §7.3 (`Exists(path: string)` — `path` is the DECLARED name):
    // the qualified spelling binds the name to the parameter exactly as the
    // bare spelling does.
    auto r = runAot(prog("import System.IO;\n",
        "    Console.WriteLine(System.IO.File.Exists(path := \"/nonexistent/x7996\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, alias_qualified_named_argument_is_honoured) {
    // the same, through the alias.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    Console.WriteLine(sio.File.Exists(path := \"/nonexistent/x7996\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_nullable_argument_is_refused) {
    // per §7 (a non-nullable parameter refuses a nullable argument) + stdlib
    // §7.3 (`path: string` is not nullable): the qualified spelling draws the
    // SAME §7 diagnostic the bare spelling draws.
    auto r = runAot(prog("import System.IO;\n",
        "    var b := System.IO.File.Exists(path := null);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `File.Exists` is nullable but the target type is "
        "non-nullable"));
}

BEZEL_TEST(builtin_fs_qualified_call_7996, alias_qualified_nullable_argument_is_refused) {
    // the same, through the alias.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    var b := sio.File.Exists(path := null);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `File.Exists` is nullable but the target type is "
        "non-nullable"));
}

// --- the controls: the bare spelling and the arity gate are unchanged ------

BEZEL_TEST(builtin_fs_qualified_call_7996, bare_head_is_unchanged) {
    // control: the bare spelling is untouched by this fix — same member, same
    // answer.
    auto r = runAot(prog("import System.IO;\n",
        "    Console.WriteLine(File.Exists(\"/nonexistent/x7996\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_bad_arity_is_rejected) {
    // per §22.6 + stdlib §7.3 (`Exists(path: string)`): the qualified
    // spelling is asked the arity question the bare spelling is asked.
    auto r = runAot(prog("import System.IO;\n",
        "    System.IO.File.Exists(\"a\", \"b\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the call to `File.Exists` supplies 2 argument(s) but stdlib §7.3 "
        "declares `Exists` with exactly 1 argument(s)"));
}

BEZEL_TEST(builtin_fs_qualified_call_7996, alias_qualified_bad_arity_is_rejected) {
    // the same, through the alias.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    sio.File.Exists(\"a\", \"b\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the call to `File.Exists` supplies 2 argument(s) but stdlib §7.3 "
        "declares `Exists` with exactly 1 argument(s)"));
}

// --- the qualified spelling names the MODULE's declaration -----------------

BEZEL_TEST(builtin_fs_qualified_call_7996, qualified_head_names_the_stdlib_class_not_the_local_one) {
    // per §2.2 ("A declaration in the *current module* shadows any same-named
    // import for bare-name purposes (the import stays reachable by
    // qualifier)") + §2.7 rule 2 (the remainder resolves against the
    // QUALIFIED module's public declarations): the current module declares
    // its own `File`, so the BARE name is the local one — and the qualified
    // name still reaches `System.IO`'s `File`, the standard library's.
    auto r = runAot(prog(
        "import System.IO;\n"
        "public static class File\n"
        "  public static method Exists(path: string): bool\n"
        "    return true;\n"
        "  end Exists;\n"
        "end File;\n",
        "    Console.WriteLine(System.IO.File.Exists(\"/nonexistent/x7996\"));\n"
        "    Console.WriteLine(File.Exists(\"/nonexistent/x7996\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    // The standard library's `File.Exists` answers `False` (the path does not
    // exist); the local `File.Exists` answers `True`.
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\n");
}

// --- the §22.6 arity gate on a QUALIFIED head (the fix-forward of #7996) ---
//
// THE DEFECT, MEASURED on the rejected commit `4ebbbaaf`. `Environment` is an
// ALWAYS-AVAILABLE name (§2.3's auto-imported `System` row), so a file whose
// import exports a public `Environment` makes the BARE name ambiguous. The
// #7996 head rewrite turns the qualified head `System.Environment` (or
// `sys.Environment`) into a bare identifier, and the §22.6 arity gate then
// took its BARE branch — the branch that carries the #7985 ambiguity
// stand-down — and stood down. The result was the worst outcome: a program
// the BASE REJECTED with the §22.6 arity error became a COMPILED program
// whose statement emitted NOTHING (the #3079 "failed to lower" degradation),
// i.e. a base-refused program turned into a silent drop.
//
// THE FIX. The stand-down is for a head the user spelled BARE. A head that
// CAME FROM A QUALIFIED SPELLING carries `aliasQualifiedBuiltin`, and the
// qualifier pinned the entity (§2.7 rule 2: the remainder resolves against
// the matched module's public declarations; §2.2: the alias is an additional
// qualifier, not a rename), so the bare name's ambiguity is beside the point
// and the arity question IS the right one.
//
// THE ORACLE. These rows assert the BASE'S DIAGNOSTIC and that the program
// does NOT run. A row that only checked "it compiles" would be satisfied by
// a silent drop — so the rows assert the exact §22.6 arity error, the empty
// stdout, and the absence of the run.

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_environment_arity_is_rejected_under_an_ambiguous_bare_name) {
    // per §22.6 ("a call at a count no declared form takes is a compile
    // error") + stdlib §2.4 (`Get(name: string): string` — exactly one
    // argument): the MODULE-QUALIFIED spelling of an always-available class is
    // asked the arity question, even in a file where the BARE name is
    // ambiguous.
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", kAmbiguousEnvironmentLib});
    mods.push_back({"App", R"BZ(
module App;
import System;
import Lib;
class P
  public static method Main(): int
    Console.WriteLine(System.Environment.Get("X", "Y"));
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    // The program is REFUSED: it does not run, and it prints nothing (a
    // silently-dropped statement would have run and printed nothing).
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    // The refusal is the §22.6 arity error — the diagnostic the BASE gave.
    BEZEL_EXPECT_TRUE(hasError(r,
        "the call to `Environment.Get` supplies 2 argument(s) but stdlib §2.4 "
        "declares `Get` with exactly 1 argument(s)"));
    // ... and it is the ONLY error: the qualified head still RESOLVES to the
    // built-in (§2.7 rule 2), so "module `System` has no public type
    // `Environment`" does NOT come back.
    BEZEL_EXPECT_FALSE(hasError(r, "has no public type"));
    BEZEL_EXPECT_EQ(errorCount(r), static_cast<std::size_t>(1));
}

BEZEL_TEST(builtin_fs_qualified_call_7996, alias_qualified_environment_arity_is_rejected_under_an_ambiguous_bare_name) {
    // the same, through the import alias (§2.2's alias rule): the alias
    // names `System`, so `sys.Environment` is `System.Environment`.
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", kAmbiguousEnvironmentLib});
    mods.push_back({"App", R"BZ(
module App;
import System as sys;
import Lib;
class P
  public static method Main(): int
    Console.WriteLine(sys.Environment.Get("X", "Y"));
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "the call to `Environment.Get` supplies 2 argument(s) but stdlib §2.4 "
        "declares `Get` with exactly 1 argument(s)"));
    BEZEL_EXPECT_FALSE(hasError(r, "has no public type"));
    BEZEL_EXPECT_EQ(errorCount(r), static_cast<std::size_t>(1));
}

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_environment_answers_under_an_ambiguous_bare_name) {
    // The fix narrows the §22.6 STAND-DOWN, not the admission: the qualified
    // spelling still resolves to the built-in and ANSWERS (§2.7 rule 2). The
    // row asserts the exact stdout, so a silent drop — which emits nothing,
    // not even the line terminator — cannot satisfy it.
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", kAmbiguousEnvironmentLib});
    mods.push_back({"App", R"BZ(
module App;
import System;
import Lib;
class P
  public static method Main(): int
    System.Environment.Set("BEZEL_7996_F6", "42");
    Console.WriteLine(System.Environment.Get("BEZEL_7996_F6"));
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, alias_qualified_environment_answers_under_an_ambiguous_bare_name) {
    // the same, through the import alias.
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", kAmbiguousEnvironmentLib});
    mods.push_back({"App", R"BZ(
module App;
import System as sys;
import Lib;
class P
  public static method Main(): int
    sys.Environment.Set("BEZEL_7996_F6", "42");
    Console.WriteLine(sys.Environment.Get("BEZEL_7996_F6"));
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

BEZEL_TEST(builtin_fs_qualified_call_7996, module_qualified_file_arity_is_rejected_under_a_shadowed_bare_name) {
    // CONTROL: `File` is NOT always-available (stdlib §7.3's `File` lives in
    // `System.IO`, which §2.3 requires an explicit import for), so a user
    // module that exports one SHADOWS the standard library's — the bare name
    // is not ambiguous, and the §22.6 arity gate keeps its own question.
    // This row pins that the fix did not narrow the gate where the stand-down
    // does NOT apply.
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", R"BZ(
module Lib;
public static class File
  public static method Exists(path: string): bool
    return true;
  end Exists;
end File;
)BZ"});
    mods.push_back({"App", R"BZ(
module App;
import System.IO;
import Lib;
class P
  public static method Main(): int
    Console.WriteLine(System.IO.File.Exists("a", "b"));
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "the call to `File.Exists` supplies 2 argument(s) but stdlib §7.3 "
        "declares `Exists` with exactly 1 argument(s)"));
    BEZEL_EXPECT_EQ(errorCount(r), static_cast<std::size_t>(1));
}
