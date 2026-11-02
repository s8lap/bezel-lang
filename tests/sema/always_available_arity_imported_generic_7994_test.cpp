// Regression test for issue #7994 (§2.2 / §2.3 / §8.1, v0.350).
//
// THE DEFECT. §2.3's always-available names are visible in EVERY file, and
// §2.3's rule for a same-named public of an explicitly imported module is
// §2.2's COLLISION rule — and §2.2 compares names together with their
// GENERIC ARITY:
//
//   §2.2 *Imports* (docs/Bezel_Language_Spec.md, v0.350):
//     "For this collision rule and the shadowing rule below, names are
//     compared together with their generic arity, the component §2.8.3 rule 3
//     adds to a type's identifier, and every reference fixes that arity by its
//     type-argument list ...: a non-generic `Counter` and the always-available
//     `Counter<K>` are two names, neither colliding with nor shadowing the
//     other (so a file importing a library's `Counter` uses both
//     `Counter.Bump()` and `Counter<string>`), while an imported `Counter<T>`
//     and `Counter<K>` collide."
//
//   §8.1 *Definition* (docs/Bezel_Language_Spec.md, v0.350):
//     "Every other declaration of the name is ordinary: a generic `Object<T>`
//     is a different name (§2.2) ..."
//
// The compiler's §2.3 always-available AMBIGUITY set
// (`Resolver::ambiguousAlwaysAvailableNames_`, populated in
// `src/sema/TypeResolution.cpp`'s import loop and consumed by
// `resolveNamed`'s §2.3 arm) was keyed on the NAME alone: an imported
// public of the same identifier — of ANY arity — was recorded as ambiguous
// and `continue`d, so it never reached the module scope or the
// (name, arity) side table. The consequence: an IMPORTED module's generic
// `Environment<T>` / `Type<T>` / `Exception<T>` was unreachable by a bare
// name, because the always-available NON-GENERIC name of the other arity
// claimed it.
//
// THE FIX (this lane). The set is keyed on (name, arity) at the point where
// it is populated: an always-available NON-GENERIC name claims the bare name
// only against a NON-GENERIC imported public (the §2.3 ambiguity of today);
// against a GENERIC imported public the two are two names, so the imported
// type is an ordinary import, reachable at its OWN arity.
//
// HELD, and pinned here as controls: the always-available GENERIC names
// (`Array<T>`, `Counter<K>`, `Span<T>`, the `Task` family, ...) keep today's
// behaviour — an imported public of the same identifier still makes the bare
// name ambiguous — pending #5140's companion (identifier, generic arity)
// ruling for those rows; and a NON-GENERIC imported public keeps claiming
// the name for EVERY always-available name.
//
// THE ISSUE'S OWN REPRO, AND WHY THIS FILE DOES NOT PIN IT. #7994's repro
// spells the imported generic `Object<T>`. On this base the PRIMITIVE arm
// of `resolveNamed` (`src/sema/TypeResolution_Names.cpp`) claims the bare
// name `Object` before the scope lookup runs, so `Object<int>` is rejected
// with "primitive type cannot take generic arguments" — MEASURED, base and
// tip alike. That is #7992's half (the primitive arm), whose fix
// (`cc8d3170`) is NOT on this base; the two halves compose, because #7992's
// predicate asks the SCOPE chain whether the name binds to a type
// declaration — so the imported `Object<T>` must reach the module scope,
// which this fix's scope-slot rule guarantees. This file therefore pins the
// TYPE-RESOLUTION half with the always-available names the primitive arm
// does not claim (`Environment`, `Type`, `Exception`, `StringBuilder`), and
// reports the primitive-arm half as the measured blocker.
//
// runAotModules is the strict AOT oracle for a MULTI-MODULE program (the
// real driver + clang link + native exec, fork-isolated). ASan is OFF in
// lanes by default (CLAUDE.md): this is name resolution, not allocation,
// layout, retain/release or disposal.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotModules;

namespace {

// A library exporting a GENERIC public whose identifier is an §2.3
// always-available name. `Hi()` returns a value so the program's exit code
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

// The body that instantiates the imported generic by the BARE name and
// returns its member's value: the exit code proves the bare name reached
// the IMPORTED declaration (42) and not something else.
std::string useBareGeneric(const char* typeName) {
    return std::string("    var o := new ") + typeName +
           "<int>();\n"
           "    var v := o.Hi();\n"
           "    return v;\n";
}

// The body that names the always-available type at ARITY 0 — the spelling
// the imported generic must NOT shadow (§2.2: the two names "neither
// collid[e] with nor shadow[] the other").
std::string useBareNonGeneric(const char* typeName) {
    return std::string("    var o := new ") + typeName +
           "();\n"
           "    var v := o.Hi();\n"
           "    return v;\n";
}

bool diagContains(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// ── R1: the imported GENERIC public of an always-available NON-GENERIC name
//        is reachable by the BARE name (the fix).
//
// `Environment` is a §2.3 Row 8 always-available name (`System`'s auto-
// imported static class). The library exports `Environment<T>`; the app
// writes `new Environment<int>()` — the BARE name, at the imported type's
// own arity. §2.2: the always-available `Environment` (arity 0) and the
// imported `Environment<T>` (arity 1) are two names, so there is no
// collision and no §2.3 ambiguity, and the reference resolves to the
// imported declaration.
//
// PRE-FIX this is rejected with §2.3's ambiguity diagnostic ("bare name
// `Environment` is ambiguous: an imported public from `Lib` shadows the
// always-available type"), because the set was keyed on the name alone.
BEZEL_TEST(always_available_arity_imported_generic_7994,
           imported_generic_of_always_available_non_generic_name_is_reachable_by_bare_name_spec_2_2) {
    JitResult r = runApp(genericLib("Environment"), useBareGeneric("Environment"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── R2: the same for a §2.3 Row 3 CORE type the compiler knows a type for
//        (`Type`), whose always-available spelling keeps working at arity 0.
//
// The bare `Type<int>` reaches the imported generic; the always-available
// `Type` (arity 0) is a DIFFERENT name and is not shadowed by it.
BEZEL_TEST(always_available_arity_imported_generic_7994,
           imported_generic_of_always_available_core_type_is_reachable_by_bare_name_spec_2_2) {
    JitResult r = runApp(genericLib("Type"), useBareGeneric("Type"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── R3: the same for a §2.3 Row 7 STANDARD EXCEPTION type (`Exception`).
BEZEL_TEST(always_available_arity_imported_generic_7994,
           imported_generic_of_always_available_exception_type_is_reachable_by_bare_name_spec_2_2) {
    JitResult r = runApp(genericLib("Exception"), useBareGeneric("Exception"));
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── R4: the always-available name at ARITY 0 still resolves EXACTLY as it
//        does without the import (§2.2's "neither ... shadowing the other").
//
// The library exports a generic `StringBuilder<T>`; the app writes the bare
// `StringBuilder` (arity 0). That spelling must still name the ALWAYS-
// AVAILABLE `StringBuilder` — the §2.3 Row 9 `System.Text` type — so
// `new StringBuilder()` and `sb.Append("hi")` are the standard library's.
//
// PRE-FIX the bare spelling is rejected (§2.3 ambiguity). POST-FIX it is
// the always-available type again, exactly as in a file with no import.
BEZEL_TEST(always_available_arity_imported_generic_7994,
           always_available_name_at_arity_zero_is_not_shadowed_by_the_imported_generic_spec_2_2) {
    JitResult r = runApp(
        genericLib("StringBuilder"),
        "    using var sb := new StringBuilder();\n"
        "    sb.Append(\"hi\");\n"
        "    return 0;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── R5 (CONTROL): a NON-GENERIC imported public of an always-available name
//        still makes the bare name ambiguous (§2.3 / #5140) — unchanged.
//
// This is the shape the §2.3 ambiguity rule exists for, and the shape the
// conformance case s2_3_0004 pins (`BitReader`).
BEZEL_TEST(always_available_arity_imported_generic_7994,
           imported_non_generic_of_always_available_name_is_still_ambiguous_spec_2_3) {
    JitResult r = runApp(plainLib("Environment"), useBareNonGeneric("Environment"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagContains(r, "is ambiguous"));
}

// ── R6 (CONTROL): an imported GENERIC public of an always-available GENERIC
//        name (`Array<T>`) still makes the bare name ambiguous — the #5140
//        HOLD for the always-available GENERIC rows, unchanged.
//
// §2.2's arity rule WOULD make the always-available `Array<T>` (arity 1) and
// an imported `Array<T,U>` (arity 2) two names; the always-available
// GENERIC rows are held pending their own ruling (see #5140), so the
// same-arity collision is still the §2.3 ambiguity it has always been.
BEZEL_TEST(always_available_arity_imported_generic_7994,
           imported_generic_of_always_available_generic_name_is_still_ambiguous_spec_2_3) {
    JitResult r = runApp(genericLib("Array"), useBareGeneric("Array"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagContains(r, "is ambiguous"));
}

// ── R7 (CONTROL): the `Counter` HOLD is unchanged — an imported NON-GENERIC
//        `Counter` still resolves by the bare name (the held rows of
//        tests/module/cross_module_import_test.cpp and the #5140 test's T5).
//
// §2.2's Counter row is the PRECEDENT this fix follows ("a non-generic
// `Counter` and the always-available `Counter<K>` are two names"), but the
// always-available GENERIC rows — `Counter<K>` among them — are held pending
// their own ruling, so the bare `Counter` still names the imported public.
BEZEL_TEST(always_available_arity_imported_generic_7994,
           held_counter_shape_is_unchanged_spec_2_2) {
    JitResult r = runApp(
        "module Lib; public class Counter public v: int := 0; "
        "public constructor(start: int) v := start; end constructor; "
        "public method Get(): int => v; end Counter;",
        "    var c := new Counter(10);\n"
        "    return c.Get();\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(10, r.exitCode);
}

// ── R8 (CONTROL): the QUALIFIED spelling reaches the imported generic —
//        and did so before the fix too (the issue's own report says so).
//
// §2.3: "The imported name is reached by qualification (its module name or
// an alias)".
BEZEL_TEST(always_available_arity_imported_generic_7994,
           qualified_spelling_reaches_the_imported_generic_spec_2_3) {
    JitResult r = runApp(
        genericLib("Environment"),
        "    var o := new Lib.Environment<int>();\n"
        "    var v := o.Hi();\n"
        "    return v;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── R9 (CONTROL): the ALIAS spelling reaches the imported generic (§2.2's
//        alias remedy) — unchanged.
BEZEL_TEST(always_available_arity_imported_generic_7994,
           alias_spelling_reaches_the_imported_generic_spec_2_2) {
    JitResult r = runApp(
        genericLib("Environment"),
        "    var o := new L.Environment<int>();\n"
        "    var v := o.Hi();\n"
        "    return v;\n",
        "import Lib as L;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── R10 (CONTROL): two imported modules exporting the SAME (name, arity)
//         are still the §2.2 collision — now reported at the arity the
//         reference fixes.
//
// §2.2: "When two imported modules export the same public name, the bare
// name is ambiguous: using it unqualified is a compile error citing both
// origins". The two imported `Environment<T>`s are the same (name, arity)
// pair, so the bare `Environment<int>` is that collision.
BEZEL_TEST(always_available_arity_imported_generic_7994,
           two_modules_exporting_the_same_generic_still_collide_spec_2_2) {
    std::string app =
        "module App;\n"
        "import LibA;\n"
        "import LibB;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var o := new Environment<int>();\n"
        "    var v := o.Hi();\n"
        "    return v;\n"
        "  end Main;\n"
        "end Program;\n";
    std::string libA =
        "module LibA; public class Environment<T> public method Hi(): int => 1; "
        "end Environment;";
    std::string libB =
        "module LibB; public class Environment<T> public method Hi(): int => 2; "
        "end Environment;";
    JitResult r = runAotModules(
        {ModuleSource{"LibA", libA}, ModuleSource{"LibB", libB},
         ModuleSource{"App", app}},
        "App");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagContains(r, "is ambiguous"));
}

// ── R11 (CONTROL): the ALWAYS-AVAILABLE side is still reachable by its own
//          module's qualified name (§2.3), even with the imported generic
//          in scope.
//
// `System.Exception` names the §31.4 built-in exception type — not the
// imported generic.
BEZEL_TEST(always_available_arity_imported_generic_7994,
           always_available_name_is_still_reachable_by_its_module_qualified_name_spec_2_3) {
    JitResult r = runApp(
        genericLib("Exception"),
        "    return 0;\n",
        "import Lib;\n",
        "  public static method Take(e: System.Exception): int => 0;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(0, r.exitCode);
}
