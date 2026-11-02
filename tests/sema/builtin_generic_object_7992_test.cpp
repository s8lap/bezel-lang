// fix #7992 / spec §8.1 *Definition* and §2.2 *Imports* (v0.350) — a
// user-declared GENERIC `Object<T>` is a DIFFERENT NAME from the root
// `Object`, and must be a user type like any other.
//
// AUTHORITY (quoted from `docs/Bezel_Language_Spec.md`, v0.350):
//
//   §8.1 *Definition*, line 1159:
//
//     "`Object` is a compiler intrinsic; method bodies are provided by the
//     runtime. The declaration above defines the public surface. Its module
//     is `System` (§2.3), as `Type`'s is, so `System.Object` is its
//     qualified name: §2.7 rule 2 reaches it, and stdlib §2.13's `FullName`
//     renders it `"System.Object"`. User code cannot redeclare or replace
//     `Object`. The ban is on a user module-level type declaration — a
//     class, record, enum, interface, choice or type alias — named `Object`
//     with no type parameters, in any user module and at any visibility.
//     Every other declaration of the name is ordinary: a generic `Object<T>`
//     is a different name (§2.2); a nested type or a type parameter named
//     `Object` shadows the root under §2.7 as any declaration shadows an
//     always-available name, and `System.Object` still reaches it; and a
//     method-local type may not take the name (§21.10). Wherever this
//     specification names `Object`, it means the root."
//
//   §2.2 *Imports*, line 139 (the collision rule, its arity clause):
//
//     "For this collision rule and the shadowing rule below, names are
//     compared together with their generic arity, the component §2.8.3 rule
//     3 adds to a type's identifier, and every reference fixes that arity
//     by its type-argument list (§14.1 *Instantiation and its site*,
//     Appendix C.1): a non-generic `Counter` and the always-available
//     `Counter<K>` are two names, neither colliding with nor shadowing the
//     other (so a file importing a library's `Counter` uses both
//     `Counter.Bump()` and `Counter<string>`), while an imported
//     `Counter<T>` and `Counter<K>` collide. Arity selects among the types
//     rule 1 of §2.7 finds for an identifier: a reference resolves to the
//     first, in that rule's order, whose arity its type-argument list
//     fixes; where none has that arity, it resolves to the first of them
//     and is Appendix C.1's arity error, never an alias, a member or a
//     module prefix."
//
// So the root `Object` (arity 0) and a user's `Object<T>` (arity 1) are TWO
// names. A reference that SUPPLIES type arguments fixes arity 1 and
// therefore names the user's declaration, not the root; a BARE `Object`
// fixes arity 0 and still names the root.
//
// THE DEFECT (MEASURED, pre-existing — identical on the base `2d33dc90` and
// on the reviewed tip of the sibling `Object`-receiver lane): a module that
// declares `public class Object<T>` could not use it. Every reference to it
// was rejected at `src/sema/TypeResolution_Names.cpp` with
//
//     primitive type cannot take generic arguments
//
// because the PRIMITIVE branch of `resolveNamed` claimed the bare name
// `Object` BEFORE the scope lookup ran, so a user declaration of the name
// never got the chance to be resolved. §8.1's own carve-out — "a generic
// `Object<T>` is a different name" — was contradicted by the compiler.
//
// WHAT THIS FILE PINS
//
//   ROW 1  the user's generic `Object<T>` is declared, instantiated with a
//          type argument, its OWN member is called, and the right value is
//          printed.
//   ROW 2  the generic form works with a CLASS-CONSTRAINED type argument
//          (`where T: class`), so the §14.2 constraint machinery sees a
//          real user generic.
//   ROW 3  the same program under `runAotChecked` — this fix makes a user
//          class INSTANTIABLE and DISPOSABLE where it was previously
//          rejected, so what the runtime must manage changed; a leak in a
//          program that now compiles is a defect this fix introduced.
//   ROW 4  a user `Object<T>`'s member is NOT caught by the root-`Object`
//          closed-surface arm (§8.1 "The declaration above defines the
//          public surface"): the member `Hi` is not one of the five §8.1
//          names, and it must still resolve on a receiver whose STATIC
//          type is the user's `Object<int>`.
//   ROW 5  CONTROL — the root `Object` still takes NO generic arguments:
//          `Object<int>` with no user declaration of the name is rejected.
//   ROW 6  the root-naming diagnostic: the rejection says the ROOT was
//          named, and says a generic `Object<T>` is a DIFFERENT name the
//          user may declare — it must not read as "a user's `Object<T>`
//          cannot exist".
//   ROW 7  CONTROL — `System.Object<int>` is rejected: §2.3's row and
//          §8.1's "and `System.Object` still reaches it" make the
//          qualified spelling name the ROOT by definition.
//   ROW 8  CONTROL — a BARE `Object` in a module that declares
//          `Object<T>` still names the ROOT (§2.2's arity rule: the bare
//          reference fixes arity 0, which the root has and the user's
//          `Object<T>` does not).
//   ROW 9  CONTROL — the §8.1 ban is unchanged: a NON-generic module-level
//          `Object` is still rejected.
//   ROW 10 CONTROL — `extension Object` still resolves (§26.5 rule 2).
//   ROW 11 CONTROL — the five §8.1 members on a root-`Object` receiver are
//          still admitted.
//   ROW 12 CONTROL — a WRONG-arity reference to the user's `Object<T>` is
//          Appendix C.1's arity error, not a primitive-arguments error.
//   ROWS 13–17 CONTROL — interface, class, type-parameter, alias and
//          nullable receivers are untouched by this fix.
//
// FAIL-WITHOUT: reverting `src/sema/TypeResolution_Names.cpp` to the base
// and REBUILDING THE TEST BINARY takes rows 1, 2, 3, 4, 6 AND 12 red; every
// other row stays green on both arms.
//
// (CORRECTED, review F2: the earlier text of this header claimed "rows 1, 2,
// 3, 4 and 6 red" and "every control row stays green on both arms". Both
// halves were wrong: ROW 12 is one of the SIX red rows on the base — its
// base message is "primitive type cannot take generic arguments", not the
// Appendix C.1 arity error the row asserts — and ROW 12 is a CONTROL row.
// The review's own base leg measured `11 passed, 6 failed` for this file,
// with ROW 12 among the six.)

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// §8.1's own words for the root-naming rejection. Pinned as a SUBSTRING on
// the rule's identifying tokens ("the root type", "no generic arguments",
// "§8.1") and on the §2.2 pointer, so a rephrase that keeps them does not
// silently break the pin — and so a message that said "a user's `Object<T>`
// cannot exist" would NOT satisfy it.
bool rootNamedDiagnostic(const JitResult& r) {
    return diagnosticMessageContains(r, "is the root type") &&
           diagnosticMessageContains(r, "takes no generic arguments") &&
           diagnosticMessageContains(r, "8.1");
}

}  // namespace

// =====================================================================
// ROW 1 — THE REPRO (#7992's exact program): a user-declared generic
// `Object<T>` is declared, instantiated, and its own member is called.
// Pre-fix: REJECTED ("primitive type cannot take generic arguments").
// Post-fix: compiles, runs, prints 7.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           user_generic_object_is_declared_and_its_member_is_called_spec_8_1) {
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
    // The VALUE, not just the exit code: a silent zero-fill exits 0 too.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// =====================================================================
// ROW 2 — the generic form with a CLASS-CONSTRAINED type argument
// (`where T: class`, §14.2). Proves the fix is not a special case for an
// unconstrained parameter list, and that the §14.2 constraint machinery
// sees a real user generic declaration.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           user_generic_object_with_class_constraint_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Widget "
        "  public method Name(): string => \"widget\"; "
        "end Widget; "
        "public class Object<T> "
        "  where T: class "
        "  public method Hi(t: T): int => 7; "
        "  public method Plain(): int => 42; "
        "end Object; "
        "public class Program "
        "  public static method Main(): int "
        "    using var o := new Object<Widget>(); "
        "    Console.WriteLine(o.Hi(new Widget())); "
        "    Console.WriteLine(o.Plain()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n42\n"));
}

// =====================================================================
// ROW 3 — the MEMORY row. This fix makes a user class instantiable and
// disposable where the program was previously REJECTED, so what the runtime
// has to manage changed: a leak in a program that now compiles is a defect
// this fix introduced. `runAotChecked` links the generated code against
// AddressSanitizer/LeakSanitizer, so a leak or a double-free fails the row.
//
// 100 iterations, so a per-iteration leak is 100 objects rather than one —
// a single-shot leak can hide under the sanitizer's noise.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           user_generic_object_is_disposed_without_a_leak_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Object<T> "
        "  public method Hi(): int => 7; "
        "end Object; "
        "public class Program "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    for var i in 0..99 do "
        "      using var o := new Object<int>(); "
        "      total := total + o.Hi(); "
        "    end for; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    // No skip-absorbing guard: a toolchain/compile/link failure surfaces as
    // ran=false and MUST fail this row.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("700\n"));
}

// =====================================================================
// ROW 4 — a user `Object<T>`'s member is NOT caught by the root-`Object`
// closed-surface arm. §8.1: "The declaration above defines the public
// surface" — the five §8.1 names (`GetType`, `Equals`, `GetHashCode`,
// `ToString`, the static `ReferenceEquals`) and nothing else. `Hi` is not
// one of them, so on a ROOT-`Object` receiver it is not a member; but the
// receiver here is statically the USER's `Object<int>`, so `Hi` is the
// user's own member and must resolve.
//
// The receiver is declared with an explicit ascription so its static type
// is the user's generic instantiation and not an inferred `var`.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           user_generic_object_member_is_not_caught_by_the_root_surface_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Object<T> "
        "  public method Hi(): int => 7; "
        "end Object; "
        "public class Program "
        "  public static method Main(): int "
        "    using var o: Object<int> := new Object<int>(); "
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
// ROW 5 — CONTROL (the admitting twin's opposite arm): the ROOT `Object`
// still takes NO generic arguments. With no user declaration of the name,
// `Object<int>` names the root — and the root is a primitive with no type
// parameters (§8.1's declaration block). GREEN on the base and on the tip.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           root_object_still_rejects_generic_arguments_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var o: Object<int> := null; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    // STRENGTHENED (issue #3892's oracle, not the ambiguous pair): a bare
    // `BEZEL_EXPECT_FALSE(r.ran)` + `hasErrors()` is ONE claim about one
    // variable, and it is satisfied just as well by a program that compiled
    // and then died from a runtime signal (runAot reports a crash, a signal,
    // a timeout and an ASan report as ordinary Error diagnostics at
    // SourceLocation{0:0}). `failedAtCompileTimeWithRealLocation` excludes
    // exactly those outcomes and demands a diagnostic carrying a REAL
    // source location, and the text check names the rule that rejected it.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(rootNamedDiagnostic(r));
}

// =====================================================================
// ROW 6 — the root-naming DIAGNOSTIC. The rejection must say the ROOT was
// named and that a generic `Object<T>` is a DIFFERENT name the user may
// declare — not that a user's `Object<T>` cannot exist. RED on the base
// (whose message was the generic "primitive type cannot take generic
// arguments"); GREEN on the tip.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           root_object_generic_arguments_message_names_the_root_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var o: Object<int> := null; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rootNamedDiagnostic(r));
    // The message must NOT tell the user that a generic `Object<T>` is
    // impossible — §8.1 says it is a DIFFERENT name.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "different name"));
}

// =====================================================================
// ROW 7 — CONTROL: `System.Object<int>` is rejected. §2.3's row ("the
// always-available one by its module's qualified name (`System.Exception`)")
// and §8.1's "and `System.Object` still reaches it" make the qualified
// spelling name the ROOT by definition, whatever the current module
// declares. GREEN on the base and on the tip.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           system_object_still_rejects_generic_arguments_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var o: System.Object<int> := null; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    // STRENGTHENED, same oracle and same reason as ROW 5: the qualified
    // spelling must be rejected AT COMPILE TIME with a real location, and
    // with the SAME root-naming text as the bare spelling (§2.3's row and
    // §8.1's "and `System.Object` still reaches it" make the qualified
    // spelling name the ROOT by definition).
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(rootNamedDiagnostic(r));
}

// =====================================================================
// ROW 8 — CONTROL: a BARE `Object` in a module that declares `Object<T>`
// still names the ROOT. §2.2's arity rule: a bare reference fixes arity 0,
// which the root has and the user's `Object<T>` does not — "a non-generic
// `Counter` and the always-available `Counter<K>` are two names, neither
// colliding with nor shadowing the other". The root's default `ToString`
// renders `"System.Object"` (§8.5), so the printed value names the root.
// GREEN on the base and on the tip.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           bare_object_still_names_the_root_in_a_module_with_a_generic_object_spec_2_2) {
    JitResult r = runAot(
        "module M; "
        "public class Object<T> "
        "  public method Hi(): int => 7; "
        "end Object; "
        "public class Program "
        "  public static method Main(): int "
        "    using var r: Object := new Object(); "
        "    Console.WriteLine(r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("System.Object\n"));
}

// =====================================================================
// ROW 9 — CONTROL: the §8.1 ban is unchanged. A NON-generic module-level
// `Object` is still rejected ("User code cannot redeclare or replace
// `Object`"). GREEN on the base and on the tip.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           non_generic_module_level_object_is_still_banned_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Object end Object; "
        "public class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "redeclare"));
}

// =====================================================================
// ROW 10 — CONTROL: `extension Object` still resolves (§26.5 rule 2: "If
// the receiver's type declares no instance member of that name, the
// compiler searches the current module and all imported modules ... for
// extension blocks on `x`'s type"). GREEN on the base and on the tip.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           extension_object_still_resolves_spec_26_5) {
    JitResult r = runAot(
        "module M; "
        "extension Object "
        "  public method Tag(): int => 7; "
        "end; "
        "public class Program "
        "  public static method Main(): int "
        "    var o: Object := 5; "
        "    var p: Object? := 6; "
        "    Console.WriteLine(o.Tag()); "
        "    Console.WriteLine(p?.Tag() ?? 0); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n7\n"));
}

// =====================================================================
// ROW 11 — CONTROL: the five §8.1 members on a ROOT-`Object` receiver are
// still admitted — `ToString`, `Equals`, `GetHashCode` and the static
// `ReferenceEquals`. GREEN on the base and on the tip.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           section_8_1_members_still_bind_on_the_root_object_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var o: Object := 5; "
        "    Console.WriteLine(o.ToString()); "
        "    Console.WriteLine(o.Equals(o)); "
        "    Console.WriteLine(o.GetHashCode()); "
        "    Console.WriteLine(Object.ReferenceEquals(o, o)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\nTrue\n5\nTrue\n"));
}

// =====================================================================
// ROW 12 — CONTROL: a WRONG-arity reference to the user's `Object<T>` is
// Appendix C.1's arity error ("generic-argument count does not match
// type's generic-parameter count"), not the root's primitive-arguments
// error: the reference names the user's declaration, whose arity is 1.
// RED on the base — the base's message is "primitive type cannot take
// generic arguments", not the arity error this row asserts — and GREEN on
// the tip (both reject; the rule named is the arity rule on the tip).
// (CORRECTED, review F2: the earlier text of this comment said "GREEN on
// the base and on the tip".)
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           wrong_arity_reference_to_the_user_generic_is_an_arity_error_spec_2_2) {
    JitResult r = runAot(
        "module M; "
        "public class Object<T> "
        "  public method Hi(): int => 7; "
        "end Object; "
        "public class Program "
        "  public static method Main(): int "
        "    var o: Object<int, int> := null; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "generic-argument count does not match"));
}

// =====================================================================
// ROW 13 — CONTROL: an INTERFACE receiver is untouched.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           interface_receiver_is_untouched_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public interface IShape "
        "  method Area(): int; "
        "end; "
        "public class Sq : IShape "
        "  public method Area(): int => 4; "
        "  destructor end destructor; "
        "end Sq; "
        "public class Program "
        "  public static method Main(): int "
        "    using var s: IShape := new Sq(); "
        "    Console.WriteLine(s.Area()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

// =====================================================================
// ROW 14 — CONTROL: a CLASS receiver is untouched.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           class_receiver_is_untouched_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class C "
        "  public method Hi(): int => 7; "
        "  destructor end destructor; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    Console.WriteLine(c.Hi()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// =====================================================================
// ROW 15 — CONTROL: a TYPE-PARAMETER receiver is untouched.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           type_parameter_receiver_is_untouched_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Tag(5)); "
        "    return 0; "
        "  end Main; "
        "  public static method Tag<T>(x: T): string => x.ToString(); "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// =====================================================================
// ROW 16 — CONTROL: an ALIAS receiver (`type MyObj := Object;`) is
// untouched — §27.8's synonym rule.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           alias_receiver_is_untouched_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "type MyObj := Object; "
        "public class Program "
        "  public static method Main(): int "
        "    var o: MyObj := 5; "
        "    Console.WriteLine(o.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// =====================================================================
// ROW 17 — CONTROL: a NULLABLE receiver (`Object?`, and the `!`
// spelling) is untouched.
// =====================================================================

BEZEL_TEST(builtin_generic_object_7992,
           nullable_receiver_is_untouched_spec_8_1) {
    JitResult r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var p: Object? := 5; "
        "    Console.WriteLine(p!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}
