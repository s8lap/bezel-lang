// fix #8008 (§8.1, §2.7, §2.3, v0.350) — §8.1's SHADOWING RULE: a nested
// type or a type parameter named `Object` SHADOWS the root.
//
// AUTHORITY (quoted from `docs/Bezel_Language_Spec.md`, v0.350):
//
//   §8.1 *Definition* (line 1159):
//
//     "User code cannot redeclare or replace `Object`. The ban is on a user
//     module-level type declaration — a class, record, enum, interface,
//     choice or type alias — named `Object` with no type parameters, in any
//     user module and at any visibility. Every other declaration of the name
//     is ordinary: a generic `Object<T>` is a different name (§2.2); a nested
//     type or a type parameter named `Object` shadows the root under §2.7 as
//     any declaration shadows an always-available name, and `System.Object`
//     still reaches it; and a method-local type may not take the name
//     (§21.10). Wherever this specification names `Object`, it means the
//     root."
//
//   §2.3 *Always-available names*, "Core types" row (line 154):
//
//     "… a nested type or type parameter named `Object` shadows it, and
//     `System.Object` still reaches it."
//
//   §2.7 *Name resolution for dotted names* (line 215):
//
//     "A nested type (§11.6) ranks as the table ranks the enclosing type's
//     members at each head shape: last at a chain head, … and innermost at a
//     type position, bare or at the head of a qualified name, where nothing
//     shadows it."
//
//   ⇒ TWO declarations shadow the root `Object`: a TYPE PARAMETER named
//     `Object`, and a NESTED TYPE named `Object`.
//
// THE DEFECT (MEASURED on the lane's base `6f956384`, which carries the
// picked #7992/#7997 commit `7eb248a5`).
//
// `resolveNamed`'s bare-name arm does the "Primitive check FIRST":
//
//     if (t.name->parts.size() == 1) {
//       PrimitiveType prim = primitiveFromName(first);
//       … if (prim != PrimitiveType::None && !userDeclaredObject) { … }  //
//     }
//
// (`src/sema/TypeResolution_Names.cpp`.) So the bare name `Object` is claimed
// as the ROOT PRIMITIVE BEFORE the scope chain is walked, and a type
// parameter named `Object` and a nested type named `Object` never get the
// chance to shadow it (§2.7: "any declaration shadows an always-available
// name").
//
// The picked #7992/#7997 commit `7eb248a5` narrows that arm only for a
// reference that SUPPLIES type arguments (`!t.genericArgs.empty()`), so it
// leaves the bare-name case, and the nested-type case, exactly as they were.
//
// THE MEASURED SYMPTOMS on the base (the reviewer's table, reproduced):
//
//   * `p9a` — inside `Tag<T>`, `var q: T := new K();` — the ORDINARY
//     type-parameter twin — REJECTED with §5.1's rule.
//   * `p9b` — inside `Tag<Object>`, `var q: Object := new K();` — the TYPE
//     PARAMETER named `Object` — ADMITTED (exit 0). The shadowing rule is
//     NOT implemented: the bare name resolved to the ROOT.
//   * `p22` — inside `Outer`, a bare `Object` must name the NESTED
//     `Outer.Object` — ADMITTED, but with two `internal:` degradation
//     warnings ("codegen cannot dispatch member 'Z' on receiver of type
//     'Object'", "the argument of this `Console.WriteLine` failed to lower
//     to a value"), so the program PRINTS NOTHING where it must print `7` —
//     a SILENTLY WRONG program.
//   * `p21` — a user's module-level generic `Object<T>` — ADMITTED, prints
//     `7` (that is the picked #7992/#7997 commit's own fix).
//
// THE FIX (`src/sema/TypeResolution_Names.cpp`, `resolveNamed`'s bare-name
// arm): the same arm now asks the scope chain whether the name `Object`
// binds to a TYPE PARAMETER (the generic-parameter marker) or to a NESTED
// TYPE, and when it does the reference FALLS THROUGH to the ordinary
// name-resolution path instead of claiming the root.
//
// WHAT THIS FILE PINS (every row asserts the PROGRAM'S OUTPUT and its EXIT
// STATUS — never "it compiles"):
//
//   ROW 1  the TYPE-PARAMETER half, at a generic METHOD's scope (the
//          reviewer's `p9b`): a bare `Object` names the TYPE PARAMETER, so
//          §5.1 rejects it exactly like its ORDINARY twin (`p9a`, ROW 5).
//   ROW 2  the TYPE-PARAMETER half, at a generic CLASS's scope: the same
//          rule, at the other place a type parameter can be declared.
//   ROW 3  the NESTED-TYPE half (the reviewer's `p22`, in a program that is
//          otherwise valid — §28.3 asks a static method that returns a class
//          type to state the ownership): the program COMPILES and PRINTS `7`
//          — the nested `Outer.Object`'s own `Z()`. Before the fix it
//          compiled and printed NOTHING.
//   ROW 4  the reviewer's EXACT `p22` (no `owned`): it must no longer be the
//          SILENTLY WRONG program. The nested type now resolves, so §28.3's
//          ownership rule applies and the program is REJECTED — it no longer
//          compiles into a binary that prints nothing.
//   ROW 5  CONTROL — the ORDINARY type-parameter twin (`p9a`) is still
//          REJECTED, with the same §5.1 wording.
//   ROW 6  CONTROL — a user's module-level generic `Object<T>` is still
//          ADMITTED as an ordinary user type and PRINTS `7` (§8.1: "a
//          generic `Object<T>` is a different name (§2.2)"). This fix must
//          not undo the picked #7992/#7997 commit.
//   ROW 7  CONTROL — `System.Object` still REACHES THE ROOT (§8.1's own
//          last clause): a value of the root and a value of `System.Object`
//          are the SAME type, so the two cross-assignments compile.
//   ROW 8  CONTROL — an ordinary type parameter `T` whose FORMAL is the ROOT
//          `Object` still compiles and prints `1` (the shadowing rule is
//          about what SHADOWS the root, not about the root's own use).
//   ROW 9  CONTROL — inside a scope that declares a nested `Object`, the
//          QUALIFIED spelling `System.Object` still reaches the root.
//   ROW 10 CONTROL — the same, through an IMPORT ALIAS (`import System as S;
//          `S.Object`).
//
// FAIL-WITHOUT: reverting `src/sema/TypeResolution_Names.cpp` to the base
// `6f956384` and REBUILDING THE TEST BINARY takes ROWS 1, 2, 3 and 4 RED
// (rows 1 and 2: the program compiles and the "must be rejected" assertion
// fails; row 3: the program compiles and prints nothing where the row
// demands `7`; row 4: the program compiles and runs, and the "must not have
// run" assertion fails). Every CONTROL row stays green on both arms.
//
// FLIPPED TESTS: none. No existing test row is deleted, relaxed or
// re-valued; this file only ADDS rows. The one behaviour change the fix
// causes outside these rows is §28.3's ownership rule now reaching a nested
// type named `Object` (ROW 4) — the rule was already there, and it already
// fired for every OTHER nested class type (the name-independent twin).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §5.1's own words for "a concrete class or record is not implicitly
// convertible to a bare type parameter", pinned as SUBSTRINGS on the rule's
// identifying tokens — the SOURCE type, the TARGET type parameter, the §5.1
// citation and the rule's own sentence — so that a rephrase which keeps them
// does not silently break the pin, and so that a message which merely said
// "not convertible" (without naming the type parameter) would NOT satisfy it.
bool sectionFiveOneClassToTypeParamDiagnostic(const JitResult& r,
                                              const char* sourceType,
                                              const char* typeParam) {
    return diagnosticMessageContains(r, "the type `") &&
           diagnosticMessageContains(r, sourceType) &&
           diagnosticMessageContains(r, "is not implicitly convertible to the "
                                        "type parameter `") &&
           diagnosticMessageContains(r, typeParam) &&
           diagnosticMessageContains(r, "5.1") &&
           diagnosticMessageContains(
               r, "no conversion from a concrete class or record to a bare "
                  "type parameter");
}

}  // namespace

// =====================================================================
// ROW 1 — the TYPE-PARAMETER half of §8.1's shadowing rule, at a generic
// METHOD's scope (the reviewer's `p9b`). The bare name `Object` in a type
// position must name the TYPE PARAMETER (which shadows the root under
// §2.7, per §8.1), so §5.1 rejects it exactly like the ORDINARY
// type-parameter twin (`p9a`, ROW 5) — with the type parameter's own name in
// the message.
//
// Pre-fix: ADMITTED (exit 0) — the bare name resolved to the ROOT.
// Post-fix: REJECTED with §5.1's rule, naming the type parameter `Object`.
// =====================================================================

BEZEL_TEST(root_object_shadowing_8008,
           a_type_parameter_named_object_shadows_the_root_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class K "
        "  public method Z(): int => 7; "
        "end K; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(1); "
        "    return 0; "
        "  end Main; "
        "  public static method Tag<Object>(x: Object): int "
        "    var q: Object := new K(); "
        "    return 1; "
        "  end Tag; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        sectionFiveOneClassToTypeParamDiagnostic(r, "K", "Object"));
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// =====================================================================
// ROW 2 — the TYPE-PARAMETER half at a generic CLASS's scope: the same
// rule, at the other place a type parameter can be declared. §8.1 names
// the type parameter, not the scope it was declared in.
//
// Pre-fix: ADMITTED (exit 0).
// Post-fix: REJECTED with §5.1's rule, naming the type parameter `Object`.
// =====================================================================

BEZEL_TEST(root_object_shadowing_8008,
           a_class_level_type_parameter_named_object_shadows_the_root_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class K "
        "  public method Z(): int => 7; "
        "end K; "
        "public class Tag<Object> "
        "  public method Bad(): int "
        "    var q: Object := new K(); "
        "    return 1; "
        "  end Bad; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(1); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        sectionFiveOneClassToTypeParamDiagnostic(r, "K", "Object"));
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// =====================================================================
// ROW 3 — the NESTED-TYPE half of §8.1's shadowing rule (the reviewer's
// `p22`), in a program that is otherwise VALID: §28.3 asks a static method
// that returns a class type to state the ownership, so the return is
// `owned Object`.
//
// Pre-fix: COMPILES and PRINTS NOTHING — the bare `Object` named the ROOT,
// so `new Object()` and `o.Z()` could not be lowered and two `internal:`
// degradation warnings were emitted. A SILENTLY WRONG program.
// Post-fix: COMPILES and PRINTS `7` — the nested `Outer.Object`'s own `Z()`.
//
// This is the SHARPEST oracle of the lot: it asserts the program's OUTPUT,
// not merely that it compiles.
// =====================================================================

BEZEL_TEST(root_object_shadowing_8008,
           a_nested_type_named_object_shadows_the_root_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Outer "
        "  public class Object "
        "    public method Z(): int => 7; "
        "  end Object; "
        "  public static method Make(): owned Object "
        "    return new Object(); "
        "  end Make; "
        "end Outer; "
        "public class Program "
        "  public static method Main(): int "
        "    using var o := Outer.Make(); "
        "    Console.WriteLine(o.Z()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// =====================================================================
// ROW 4 — the reviewer's EXACT `p22` (no `owned`): it must no longer be
// the SILENTLY WRONG program. The bare `Object` now names the NESTED
// class, so §28.3's ownership rule reaches it (it already reached every
// OTHER nested class type — the rule is name-independent) and the program
// is REJECTED instead of compiling into a binary that prints nothing.
//
// Pre-fix: COMPILES, RUNS, exit 0, PRINTS NOTHING (the wrong program).
// Post-fix: REJECTED, with §28.3's diagnostic.
// =====================================================================

BEZEL_TEST(root_object_shadowing_8008,
           the_nested_object_program_is_not_silently_wrong_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Outer "
        "  public class Object "
        "    public method Z(): int => 7; "
        "  end Object; "
        "  public static method Make(): Object "
        "    return new Object(); "
        "  end Make; "
        "end Outer; "
        "public class Program "
        "  public static method Main(): int "
        "    using var o := Outer.Make(); "
        "    Console.WriteLine(o.Z()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "28.3"));
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// =====================================================================
// ROW 5 — CONTROL: the ORDINARY type-parameter twin (the reviewer's
// `p9a`) is still REJECTED, with the same §5.1 wording. The fix changed
// what SHADOWS the root, not §5.1's rule.
//
// GREEN on both arms.
// =====================================================================

BEZEL_TEST(root_object_shadowing_8008,
           the_ordinary_type_parameter_twin_is_still_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "public class K "
        "  public method Z(): int => 7; "
        "end K; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(1); "
        "    return 0; "
        "  end Main; "
        "  public static method Tag<T>(x: T): int "
        "    var q: T := new K(); "
        "    return 1; "
        "  end Tag; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        sectionFiveOneClassToTypeParamDiagnostic(r, "K", "T"));
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(""));
}

// =====================================================================
// ROW 6 — CONTROL: a user's module-level GENERIC `Object<T>` is still
// ADMITTED as an ordinary user type (§8.1: "a generic `Object<T>` is a
// different name (§2.2)"). It is instantiated, its OWN member is called,
// and the right value is printed. This fix must not undo the picked
// #7992/#7997 commit.
//
// GREEN on both arms.
// =====================================================================

BEZEL_TEST(root_object_shadowing_8008,
           a_user_generic_object_is_still_admitted_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Object<T> "
        "  public method Hi(): int => 7; "
        "end Object; "
        "public class Program "
        "  public static method Main(): int "
        "    using var o := new Object<int>(); "
        "    Console.WriteLine(o.Hi()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// =====================================================================
// ROW 7 — CONTROL: `System.Object` still REACHES THE ROOT (§8.1's own last
// clause: "and `System.Object` still reaches it"). The qualified spelling
// names the root BY DEFINITION, so a value of the root and a value of
// `System.Object` are the SAME type — the two cross-assignments compile
// only if the qualified spelling reaches the root.
//
// GREEN on both arms.
// =====================================================================

BEZEL_TEST(root_object_shadowing_8008,
           system_object_still_reaches_the_root_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var a: Object := null; "
        "    var b: System.Object := a; "
        "    var c: Object := b; "
        "    Console.WriteLine(1); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// =====================================================================
// ROW 8 — CONTROL: an ordinary type parameter `T` whose FORMAL is the ROOT
// `Object` still compiles and prints `1`. The shadowing rule is about what
// SHADOWS the root, not about the root's own use — and the root is still
// reachable by its bare name when nothing shadows it.
//
// GREEN on both arms.
// =====================================================================

BEZEL_TEST(root_object_shadowing_8008,
           an_ordinary_type_parameter_may_still_take_the_root_object_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Tag<int>(5)); "
        "    return 0; "
        "  end Main; "
        "  public static method Tag<T>(x: Object): int => 1; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// =====================================================================
// ROW 9 — CONTROL: a nested type named `Object` shadows the BARE name
// ONLY. Inside the scope that declares it, the QUALIFIED spelling
// `System.Object` still reaches the root (§8.1's last clause), so a value
// of the root and a value of `System.Object` are the SAME type.
//
// GREEN on both arms.
// =====================================================================

BEZEL_TEST(root_object_shadowing_8008,
           system_object_still_reaches_the_root_inside_a_shadowing_scope_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Outer "
        "  public class Object "
        "    public method Z(): int => 7; "
        "  end Object; "
        "  public static method Root(): System.Object "
        "    return null; "
        "  end Root; "
        "end Outer; "
        "public class Program "
        "  public static method Main(): int "
        "    var r: System.Object := null; "
        "    var q: Object := r; "
        "    Console.WriteLine(1); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// =====================================================================
// ROW 10 — CONTROL: the same, through an IMPORT ALIAS
// (`import System as S;` then `S.Object`). The qualified spelling names
// the ROOT by definition (§2.3's row, §2.7 rule 2), so it reaches the root
// even in a scope whose BARE `Object` is shadowed by a nested type.
//
// GREEN on both arms.
// =====================================================================

BEZEL_TEST(root_object_shadowing_8008,
           an_aliased_system_object_still_reaches_the_root_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "import System as S; "
        "public class Outer "
        "  public class Object "
        "    public method Z(): int => 7; "
        "  end Object; "
        "  public static method Root(): S.Object "
        "    return null; "
        "  end Root; "
        "end Outer; "
        "public class Program "
        "  public static method Main(): int "
        "    var r: S.Object := null; "
        "    Console.WriteLine(1); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}
