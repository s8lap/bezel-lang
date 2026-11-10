// Regression test for issue #8022 — the QUALIFIED / ALIAS-QUALIFIED built-in
// STATIC CLASS used as a VALUE is SILENTLY DROPPED (spec §2.2 / §2.7, v0.355;
// stdlib §2.4 / §7.3 / §7.4).
//
// THE DEFECT. §2.7 rule 2 resolves the REMAINDER of a dotted chain against
// the matched module's public DECLARATIONS, and stdlib §7.3/§7.4/§2.4
// declare `File`, `Directory`, `Path` and `Environment` as `public static
// class` declarations of `System.IO` / `System`. §2.2 then makes the alias
// spelling name the SAME entity ("The alias is an additional *qualifier*,
// not a rename"). So `System.IO.Path.Combine(..)` and `sio.Path.Combine(..)`
// denote one entity and must behave exactly like the bare
// `Path.Combine(..)` — that is what #7996 admitted, and it works.
//
// A built-in static class, though, is a NAME, not a value. When the
// QUALIFIED name is the WHOLE expression — `var x := System.IO.Path;` — there
// is no enclosing member access to rewrite the head to the bare spelling, so
// the qualified name reached codegen still qualified, matched no built-in
// static-call arm, and lowered to NOTHING: the statement was emitted
// nowhere, and the program ran on without it.
//
// MEASURED on the pre-fix code (this tip's base, `r30-c1` @ 4e0604a35):
//
//   import System.IO;      +  var x := System.IO.Path;
//     -> rc=0, a WARNING ("internal: lowerLocalDecl's initializer expression
//        failed to lower to a value for this local's slot (degraded to a
//        zero-initialized value) — #2434"), the program RAN, and it printed
//        "after". The statement was GONE.
//
//   import System.IO as sio;  +  var x := sio.Path;
//     -> rc=0, the same #2434 warning, the program RAN and printed "after".
//
// That is a SILENT DROP — the worst outcome for a statement: the program
// compiles, runs, and does something else.
//
// THE ORACLE IS THE DIAGNOSTIC, NOT "IT DID NOT CRASH" AND NOT "IT COMPILED".
// Each reject row below asserts the EXACT diagnostic text AND that the
// program no longer runs: on the pre-fix code the program RAN and printed
// "after" (the statement was silently absent), so `r.ran` was true and
// `r.stdoutText` was "after\n" — the stdout assertion is what makes the
// silence mechanically detectable. A test that only checked "it still
// compiles" would prove nothing: a silent drop compiles.
//
// WHAT MUST NOT CHANGE (the rows below pin each of them):
//   * the alias-qualified call to a name the aliased module does not home
//     is refused with the module-qualified arm's text (the #8022 arm);
//   * the qualified and alias-qualified CALLS of a homed built-in static
//     class still resolve and answer (the #7996 rows);
//   * the bare spelling still resolves;
//   * a USER TYPE of the same name still WINS over the built-in (both in the
//     alias form and in the module-qualified form) — the bare-name ambiguity
//     machinery must not turn a resolvable user type into a diagnostic;
//   * the `System.Threading` built-in heads are still admitted.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotModules;

namespace {

// The program prints "after" when it reaches the end of Main, so a row that
// asserts `stdoutText == ""` is asserting that the program did not run at
// all — the observable a silent drop breaks.
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

// ── THE DEFECT: the qualified static class used as a VALUE ──────────────────

BEZEL_TEST(alias_qualified_static_class_value_8022,
           module_qualified_static_class_used_as_a_value_is_rejected) {
    // per §2.7 rule 2: `System.IO.Path` names a public static CLASS of
    // `System.IO`, and a class name is not a value. MEASURED on the pre-fix
    // code: this compiled, ran, and printed "after" — the statement was
    // silently absent.
    auto r = runAot(prog("import System.IO;\n",
        "    var x := System.IO.Path;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System.IO` has no public type `Path`"));
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           alias_qualified_static_class_used_as_a_value_is_rejected) {
    // per §2.2: the alias is an additional QUALIFIER, not a rename, so
    // `sio.Path` names the same entity as `System.IO.Path` — and must be
    // refused the same way. MEASURED on the pre-fix code: this compiled,
    // ran, and printed "after" — the statement was silently absent.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    var x := sio.Path;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System.IO` has no public type `Path`"));
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           module_qualified_static_class_of_system_used_as_a_value_is_rejected) {
    // The THIRD admission arm: the SINGLE-SEGMENT module spelling
    // (`System.Environment`, stdlib §2.4). MEASURED on the pre-fix code:
    // this compiled, ran, and printed "after" — the statement was silently
    // absent.
    auto r = runAot(prog("import System;\n",
        "    var x := System.Environment;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System` has no public type `Environment`"));
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           alias_qualified_static_class_of_system_used_as_a_value_is_rejected) {
    // the alias twin of the row above.
    auto r = runAot(prog("import System as st;\n",
        "    var x := st.Environment;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System` has no public type `Environment`"));
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           the_silent_drop_is_visible_in_the_programs_output) {
    // The mechanical fail-without, spelled as an OUTPUT oracle: the program
    // prints "before" and then, if the offending statement is silently
    // dropped, "after" as well. Pre-fix the program ran and printed BOTH
    // lines (the statement was gone, and nothing said so); post-fix the
    // program is refused and prints NOTHING.
    auto r = runAot(prog("import System.IO;\n",
        "    Console.WriteLine(\"before\");\n"
        "    var x := System.IO.Path;\n"
        "    Console.WriteLine(\"after\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System.IO` has no public type `Path`"));
}

// ── THE #8022 ALIAS ARM: a name the aliased module does not home ───────────

BEZEL_TEST(alias_qualified_static_class_value_8022,
           alias_unknown_type_call_head_is_rejected) {
    // per §2.2: `sio.NotAType` names no type of `System.IO`.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    sio.NotAType.Exists(\"x\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System.IO` has no public type `NotAType`"));
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           alias_builtin_class_of_another_module_is_rejected) {
    // per §2.2 + stdlib §7.3: `File` is homed by `System.IO`, not `System`.
    auto r = runAot(prog("import System as st;\n",
        "    Console.WriteLine(st.File.Exists(\"x\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System` has no public type `File`"));
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           module_qualified_unknown_type_is_rejected_with_the_same_text) {
    // control: the module-qualified spelling's text, which the alias reuses.
    auto r = runAot(prog("import System.IO;\n",
        "    System.IO.NotAType.Exists(\"x\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System.IO` has no public type `NotAType`"));
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           alias_type_position_static_class_is_rejected) {
    // control: the alias TYPE-position form already diagnoses (§2.2), and
    // says the same thing about the same name.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    var x: sio.Path := \"y\";"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_TRUE(hasError(r,
        "module `System.IO` has no public type `Path`"));
}

// ── WHAT MUST NOT CHANGE: the qualified CALLS still resolve ────────────────

BEZEL_TEST(alias_qualified_static_class_value_8022,
           module_qualified_static_class_call_still_resolves) {
    // control (#7996): `System.IO.Path.Combine(..)` still names the built-in
    // and answers.
    auto r = runAot(prog("import System.IO;\n",
        "    Console.WriteLine(System.IO.Path.Combine(\"a\", \"b\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "a/b\nafter\n");
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           alias_qualified_static_class_call_still_resolves) {
    // control (#7996): `sio.Path.Combine(..)` still names the built-in and
    // answers.
    auto r = runAot(prog("import System.IO as sio;\n",
        "    Console.WriteLine(sio.Path.Combine(\"a\", \"b\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "a/b\nafter\n");
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           bare_static_class_call_still_resolves) {
    // control: the BARE spelling is the entity the qualified one denotes
    // (§2.7 rule 2), so it must keep answering.
    auto r = runAot(prog("import System.IO;\n",
        "    Console.WriteLine(Path.Combine(\"a\", \"b\"));\n"
        "    Console.WriteLine(File.Exists(\"x\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "a/b\nFalse\nafter\n");
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           module_qualified_environment_call_still_resolves) {
    // control: the SINGLE-SEGMENT module spelling's CALL still resolves —
    // `System.Environment` is the receiver of a static member read.
    auto r = runAot(prog("import System;\n",
        "    System.Environment.Set(\"BEZEL_R41_8022\", \"v\");\n"
        "    Console.WriteLine(\"set\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "set\nafter\n");
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           alias_qualified_environment_call_still_resolves) {
    // control: the alias twin.
    auto r = runAot(prog("import System as st;\n",
        "    st.Environment.Set(\"BEZEL_R41_8022\", \"v\");\n"
        "    Console.WriteLine(\"set\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "set\nafter\n");
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           qualified_threading_heads_are_still_admitted) {
    // control (#5012/#5013): the `System.Threading` built-in heads are
    // exempt from the module-qualified "has no public type" reject.
    auto r = runAot(prog("import System.Threading;\n",
        "    Console.WriteLine(System.Threading.CancellationToken.None);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "System.Threading.CancellationToken\nafter\n");
}

BEZEL_TEST(alias_qualified_static_class_value_8022,
           alias_qualified_threading_heads_are_still_admitted) {
    // control (#5012/#5013): the ALIAS spelling of the same head.
    auto r = runAot(prog("import System.Threading as th;\n",
        "    Console.WriteLine(th.CancellationToken.None);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "System.Threading.CancellationToken\nafter\n");
}

// ── WHAT MUST NOT CHANGE: a USER TYPE still wins ───────────────────────────

BEZEL_TEST(alias_qualified_static_class_value_8022,
           alias_user_type_still_wins) {
    // per §2.7 rule 1: an alias of a module that exports a USER type named
    // `File` selects THAT type, not a diagnostic — the bare-name ambiguity
    // machinery must not turn a resolvable user type into a diagnostic.
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

BEZEL_TEST(alias_qualified_static_class_value_8022,
           module_qualified_user_type_still_wins) {
    // the MODULE-qualified twin of the row above: `Lib.File.Exists` reaches
    // the user's type.
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
import Lib;
class P
  public static method Main(): int
    Console.WriteLine(Lib.File.Exists("x"));
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}
