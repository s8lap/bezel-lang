// Regression test for issue #8002 (§2.2 / §2.3 / §8.1, v0.350).
//
// THE DEFECT. §2.3's always-available names are visible in EVERY file, and
// §2.3's rule for a same-named public of an explicitly imported module is
// §2.2's COLLISION rule — and §2.2 compares names together with their
// GENERIC ARITY:
//
//   §2.2 *Imports* (docs/Bezel_Language_Spec.md, v0.350):
//     "For this collision rule and the shadowing rule below, names are
//     compared together with their generic arity, the component §2.8.3 rule 3
//     adds to a type's identifier, and every reference fixes that arity by
//     its type-argument list ...: a non-generic `Counter` and the
//     always-available `Counter<K>` are two names, neither colliding with nor
//     shadowing the other (so a file importing a library's `Counter` uses
//     both `Counter.Bump()` and `Counter<string>`), while an imported
//     `Counter<T>` and `Counter<K>` collide."
//
//   §2.3 *Always-available names* (docs/Bezel_Language_Spec.md, v0.350):
//     "a same-named public exported by an explicitly imported module does not
//     shadow it but makes the bare name **ambiguous** in that file under
//     §2.2's collision rule".
//
//   §8.1 *Definition* (docs/Bezel_Language_Spec.md, v0.350):
//     "Every other declaration of the name is ordinary: a generic `Object<T>`
//     is a different name (§2.2) ... Wherever this specification names
//     `Object`, it means the root."
//
// The §2.3 always-available AMBIGUITY set is consulted at TWO arms, each
// with its own copy of the set and its own import loop:
//
//   * the TYPE arm — `Resolver::ambiguousAlwaysAvailableNames_`, populated in
//     `src/sema/TypeResolution.cpp`'s import loop, consumed by `resolveNamed`'s
//     §2.3 arm (`src/sema/TypeResolution_Names.cpp`);
//   * the EXPRESSION arm — `ExprTC::ambiguousAlwaysAvailableNames_`,
//     populated in `src/sema/ExprTypecheck.cpp`'s import loop, consumed by the
//     IdentifierExpr visitor (`src/sema/ExprTypecheck_Exprs.cpp`).
//
// The TYPE arm was re-keyed on (name, arity) by #7994 (commit 82bb7eb3). The
// EXPRESSION arm was NOT: it was keyed on the NAME alone, so an imported
// public of the same identifier — of ANY arity — was recorded as ambiguous
// there. The consequence: in a file that imports a module exporting a
// GENERIC public whose identifier is an always-available NON-GENERIC name,
// the TYPE arm resolved the imported generic while the EXPRESSION arm
// rejected the very same file with §2.3's ambiguity diagnostic.
//
//   §2.3 lists `Console`, `Math`, `Convert`, `Environment` (Row 8) among the
//   `System` auto-imported STATIC classes, and `Object` (Row 3) as the
//   root — none of them generic. So `import Lib;` where `Lib` declares
//   `public class Console<T>` (or `public class Object<T>`) makes the bare
//   name `Console` (arity 0) and the bare name `Console<T>` (arity 1) TWO
//   names: no §2.2 collision, no §2.3 ambiguity, and the imported generic is
//   an ordinary import, reachable at its OWN arity.
//
// THE REPRO (the issue's own). A file that imports a module declaring a
// generic `Console<T>`:
//
//     import Other;   -- Other declares `public class Console<T>`
//     ...
//     Console.WriteLine("x");   -- REJECTED at the EXPRESSION arm
//
// THE FIX (this lane). The §2.2 arity question is asked ONCE, of the SHARED
// predicate `Resolver::alwaysAvailableClaimsImportedName`
// (`src/sema/TypeResolution.cpp`), and both arms ask it — so the TYPE arm
// and the EXPRESSION arm cannot drift apart.
//
// HELD, and pinned here as controls: an imported NON-GENERIC public of an
// always-available name still makes the bare name ambiguous (the §2.3 rule
// this fix does not touch), and an always-available GENERIC name (`Array<T>`,
// `Counter<K>`, `Span<T>`, the `Task` family, ...) still claims the bare name
// against an imported public of ANY arity — the #5140 HOLD, pending its own
// (identifier, generic arity) ruling.
//
// THE ORACLE. `runAotModules` is the strict AOT oracle for a MULTI-MODULE
// program (the real driver + clang link + native exec, fork-isolated). Every
// admitting row asserts the program's STDOUT and its exit code — the
// program's BEHAVIOUR, not "it compiled". Every refusing row asserts the
// exact §2.3 ambiguity diagnostic. ASan is OFF in lanes by default
// (CLAUDE.md): this is name resolution, not allocation, layout,
// retain/release or disposal.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotModules;

namespace {

// A library exporting a GENERIC public whose identifier is an §2.3
// always-available name. `Hi()` returns a value so the program's OUTPUT
// proves WHICH declaration the bare name reached.
std::string genericLib(const char* name) {
    return std::string("module Lib; public class ") + name +
           "<T> public method Hi(): int => 42; end " + name + ";";
}

// A library exporting a NON-GENERIC public of the same identifier.
std::string plainLib(const char* name) {
    return std::string("module Lib; public class ") + name +
           " public method Hi(): int => 42; end " + name + ";";
}

// The importing module: `import Lib;` and a body that names the imported
// type by the spelling under test.
JitResult runApp(const std::string& libSrc, const std::string& body,
                 const std::string& imports = "import Lib;\n",
                 const std::string& extraMembers = "") {
    std::string app =
        "module App;\n" + imports +
        "public class Program\n" + extraMembers +
        "  public static method Main(): int\n" + body +
        "  end Main;\n"
        "end Program;\n";
    return runAotModules({ModuleSource{"Lib", libSrc}, ModuleSource{"App", app}},
                         "App");
}

// Does the rejection carry the §2.3 always-available ambiguity diagnostic?
// The §2.3 arm's own wording, quoted from the two sites that emit it:
//   TYPE arm:      "bare name `X` is ambiguous: an imported public from `M`
//                   shadows the always-available type; qualify it via a
//                   module-qualified name (e.g. `M.X`) or an import alias"
//   EXPRESSION arm:"bare name `X` is ambiguous: an imported public shadows
//                   the always-available type; qualify it via a module-
//                   qualified name or an import alias"
bool diagIsAlwaysAvailableAmbiguity(const JitResult& r, const char* name) {
    const std::string needle = std::string("bare name `") + name +
                               "` is ambiguous";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos &&
            d.message.find("always-available type") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── R1 (THE FIX): the always-available STATIC CLASS in EXPRESSION position,
//        with an imported GENERIC public of the same identifier in scope.
//
// `Console` is an §2.3 Row 8 always-available name — the `System`
// auto-imported static class — and it is NON-GENERIC. The library exports
// `Console<T>` (arity 1). §2.2: the always-available `Console` (arity 0)
// and the imported `Console<T>` (arity 1) are TWO names, so there is no
// collision and no §2.3 ambiguity: `Console.WriteLine` names the
// always-available static class, exactly as in a file with no import.
//
// PRE-FIX this is rejected at the EXPRESSION arm with §2.3's ambiguity
// diagnostic ("bare name `Console` is ambiguous: an imported public shadows
// the always-available type"), while the TYPE arm resolved the imported
// generic — the two arms disagreed.
BEZEL_TEST(always_available_arity_expression_8002,
           always_available_static_class_in_expression_position_with_an_imported_generic_is_not_ambiguous_spec_2_2) {
    JitResult r = runApp(genericLib("Console"),
                          "    Console.WriteLine(\"console-ok\");\n"
                          "    Console.WriteLine(\"done\");\n"
                          "    return 0;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) {
        BEZEL_EXPECT_EQ(r.stdoutText, "console-ok\ndone\n");
        BEZEL_EXPECT_EQ(0, r.exitCode);
    }
}

// ── R2 (THE FIX): the ROOT's own static surface, with an imported GENERIC
//        `Object<T>` in scope.
//
// `Object` is the §2.3 Row 3 always-available core type — the root (§8.1) —
// and it is NON-GENERIC. The library exports `Object<T>` (arity 1). §2.2:
// the root `Object` (arity 0) and the imported `Object<T>` (arity 1) are TWO
// names. §8.1: "Wherever this specification names `Object`, it means the
// root" — so the root's own static surface (`Object.ReferenceEquals`) is
// still the root's.
//
// PRE-FIX this is rejected at the EXPRESSION arm with §2.3's ambiguity
// diagnostic.
BEZEL_TEST(always_available_arity_expression_8002,
           root_object_static_surface_with_an_imported_generic_object_is_not_ambiguous_spec_8_1) {
    JitResult r = runApp(genericLib("Object"),
                          "    Console.WriteLine(Object.ReferenceEquals(null, null));\n"
                          "    return 0;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) {
        BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
        BEZEL_EXPECT_EQ(0, r.exitCode);
    }
}

// ── R3 (THE FIX): BOTH arms at once — the imported GENERIC public at its
//        own arity AND the always-available name at arity 0.
//
// The program names the imported generic by the BARE name at its own arity
// (`new Console<int>()`, the TYPE arm) and the always-available static
// class at arity 0 (`Console.WriteLine`, the EXPRESSION arm). Both must
// reach their own declaration.
BEZEL_TEST(always_available_arity_expression_8002,
           both_arms_reach_their_own_declaration_with_an_imported_generic_spec_2_2) {
    JitResult r = runApp(genericLib("Console"),
                          "    using var o := new Console<int>();\n"
                          "    Console.WriteLine(o.Hi());\n"
                          "    Console.WriteLine(\"both\");\n"
                          "    return 0;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) {
        BEZEL_EXPECT_EQ(r.stdoutText, "42\nboth\n");
        BEZEL_EXPECT_EQ(0, r.exitCode);
    }
}

// ── R4 (CONTROL): the USER's generic `Object<T>` still behaves as a USER
//        type — its own member is reachable by the bare name and the program
//        prints ITS value.
//
// The root `Object` and the user's `Object<T>` are two names (§2.2); the
// user's one is an ordinary user class and must not be swallowed by the
// root's surface.
BEZEL_TEST(always_available_arity_expression_8002,
           user_generic_object_still_behaves_as_a_user_type_spec_2_2) {
    JitResult r = runApp(genericLib("Object"),
                          "    using var o := new Object<int>();\n"
                          "    Console.WriteLine(o.Hi());\n"
                          "    return 0;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) {
        BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
        BEZEL_EXPECT_EQ(0, r.exitCode);
    }
}

// ── R5 (CONTROL): the ROOT `Object` still behaves as the ROOT — a bare
//        `Object` in TYPE position names the root even with an imported
//        generic `Object<T>` in scope, and the root's static surface works.
//
// The two names are two names: the arity-0 spelling is the root (§8.1), the
// arity-1 spelling is the user's class (§2.2).
BEZEL_TEST(always_available_arity_expression_8002,
           root_object_still_behaves_as_the_root_with_an_imported_generic_object_in_scope_spec_8_1) {
    JitResult r = runApp(genericLib("Object"),
                          "    var x: Object := null;\n"
                          "    Console.WriteLine(Object.ReferenceEquals(x, null));\n"
                          "    return 0;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) {
        BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
        BEZEL_EXPECT_EQ(0, r.exitCode);
    }
}

// ── R6 (CONTROL): an imported NON-GENERIC public of an always-available name
//        still makes the bare name AMBIGUOUS — the §2.3 rule this fix does
//        not touch.
//
// An imported `Console` (arity 0) and the always-available `Console`
// (arity 0) are the SAME §2.2 name: the bare name is ambiguous, and the
// remedy is qualification. GREEN ON BOTH ARMS.
BEZEL_TEST(always_available_arity_expression_8002,
           imported_non_generic_of_always_available_name_is_still_ambiguous_spec_2_3) {
    JitResult r = runApp(plainLib("Console"),
                          "    Console.WriteLine(\"x\");\n"
                          "    return 0;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagIsAlwaysAvailableAmbiguity(r, "Console"));
}

// ── R7 (CONTROL): the #5140 HOLD — an always-available GENERIC name still
//        claims the bare name against an imported public of ANY arity.
//
// `Array<T>` is an §2.3 Row 10 always-available name at arity 1. §2.2's
// arity rule WOULD make it and an imported `Array<T>` two names, but the
// always-available GENERIC rows are HELD pending their own ruling, so the
// same-arity collision is still the §2.3 ambiguity it has always been.
BEZEL_TEST(always_available_arity_expression_8002,
           imported_generic_of_always_available_generic_name_is_still_ambiguous_spec_2_3) {
    JitResult r = runApp(genericLib("Array"),
                          "    using var o := new Array<int>();\n"
                          "    return o.Hi();\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagIsAlwaysAvailableAmbiguity(r, "Array"));
}

// ── R8 (CONTROL): the always-available name at ARITY 0 still resolves as
//        ITSELF when an imported generic of the other arity is in scope.
//
// `StringBuilder` is an §2.3 Row 9 always-available name, non-generic. The
// library exports `StringBuilder<T>` (arity 1) — two names. The arity-0
// spelling must still be the always-available `StringBuilder`, so
// `new StringBuilder()` and `sb.Append(...)` are the standard library's.
BEZEL_TEST(always_available_arity_expression_8002,
           always_available_name_at_arity_zero_is_still_itself_with_an_imported_generic_spec_2_2) {
    JitResult r = runApp(genericLib("StringBuilder"),
                          "    using var sb := new StringBuilder();\n"
                          "    sb.Append(\"hi\");\n"
                          "    Console.WriteLine(\"sb-ok\");\n"
                          "    return 0;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) {
        BEZEL_EXPECT_EQ(r.stdoutText, "sb-ok\n");
        BEZEL_EXPECT_EQ(0, r.exitCode);
    }
}

// ── R9 (CONTROL): the QUALIFIED spelling still reaches the imported
//        generic — and so does the always-available name at arity 0.
//
// §2.3: "The imported name is reached by qualification (its module name or
// an alias)"; §2.2: the always-available one is reached by its own
// spelling. Both hold at once.
BEZEL_TEST(always_available_arity_expression_8002,
           qualified_spelling_still_reaches_the_imported_generic_spec_2_3) {
    JitResult r = runApp(genericLib("Console"),
                          "    using var o := new Lib.Console<int>();\n"
                          "    Console.WriteLine(o.Hi());\n"
                          "    return 0;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) {
        BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
        BEZEL_EXPECT_EQ(0, r.exitCode);
    }
}

// ── R10 (CONTROL): the ARITY-0 spelling of the always-available name is NOT
//         the imported generic — the two names do not collide, so the
//         arity-0 reference is the always-available one (which the compiler
//         has no TYPE for: `Console` is a static class, not a type), NOT the
//         user's `Console<T>`.
//
// The rejection must NOT be the §2.3 ambiguity diagnostic (the two are two
// names); it is the ordinary "no such type at arity 0" answer.
BEZEL_TEST(always_available_arity_expression_8002,
           arity_zero_reference_is_not_the_imported_generic_spec_2_2) {
    JitResult r = runApp(genericLib("Console"),
                          "    using var o := new Console();\n"
                          "    return 0;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(diagIsAlwaysAvailableAmbiguity(r, "Console"));
}
