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

using bezel::test::runAot;

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
