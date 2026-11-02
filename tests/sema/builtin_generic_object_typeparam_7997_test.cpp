// fix #7997 / #7992 (§5.1, §8.1, §2.2, v0.350) — a USER type named
// `Object` buys NO exemption from §5.1's "no conversion from a concrete
// class or record to a bare type parameter" rule.
//
// AUTHORITY (quoted from `docs/Bezel_Language_Spec.md`, v0.350):
//
//   §5.1 *Implicit conversions*, "Reference conversions" (the CLOSED list):
//
//     - **identity**: every type converts to itself;
//     - **class to base class**, transitively, up to `Object`;
//     - **any type to `Object`**: …
//     - **a type to an interface it implements or conforms to**: …
//     - **a type parameter to an interface or class its constraints guarantee
//       conformance to** (§14.2 …)
//
//     (variance-induced conversions of §12.9)
//
//   There is NO row "any type → a bare type parameter". The only conversion
//   INTO a bare type parameter is the one the type parameter's own
//   CONSTRAINTS guarantee — so a CONCRETE CLASS or RECORD has none, and the
//   compiler enforces that with an explicit diagnostic.
//
//   §8.1 *Definition* (line 1159):
//
//     "The ban is on a user module-level type declaration — a class, record,
//     enum, interface, choice or type alias — named `Object` with no type
//     parameters, in any user module and at any visibility. Every other
//     declaration of the name is ordinary: a generic `Object<T>` is a
//     different name (§2.2) …"
//
//   §2.2 *Imports* (line 139): the root `Object` (arity 0) and a user's
//   `Object<T>` (arity 1) are TWO names.
//
//   ⇒ A user's generic `Object<T>` is an ordinary user class. It gets NO
//     exemption from §5.1 that any other user class does not get.
//
// THE DEFECT (MEASURED, introduced by the #7992 commit `cc8d3170`, which is
// the first commit to admit a user's generic `Object<T>` at all).
//
// `ExprTC::checkAssignable` carries the §5.1 rule as its LAST arm (the
// #5469 check, `src/sema/ExprTypecheck.cpp`). Its predicate was:
//
//     bool fromIsConcreteClassRecord =
//         from->kind == TypeRef::Kind::Named &&
//         from->declaration &&
//         (from->declaration->kind == ast::TypeDeclKind::Class ||
//          from->declaration->kind == ast::TypeDeclKind::Record) &&
//         from->headName() != "Object";            // <-- NAME-KEYED
//
// The exemption is keyed on the SPELLING `Object`. The ROOT `Object` resolves
// to a `Kind::Primitive` ref (`PrimitiveType::Object`, §3.4/§8.1), so it
// never reaches this arm at all — the `from->kind == Kind::Named &&
// from->declaration` requirement already excludes it. The clause's ONLY
// reachable effect was therefore to exempt a USER class spelled `Object`.
//
// The consequence is not a missing diagnostic but a WRONG VALUE: the program
//
//     public static method M<T>(): owned T => new Object<int>();
//
// returns a class instance where a bare `T` is declared, so
// `var x: int := M<int>();` reads a HEAP ADDRESS as an `int` (the review's
// probe printed `151413536`, then `42`). The base rejects the same program.
//
// THE FIX (one clause, `src/sema/ExprTypecheck.cpp`): the `!= "Object"`
// clause is DELETED, so the exemption is CATEGORY-BLIND — the root is
// exempted by its RESOLVED IDENTITY (a declaration-less ref, which the arm
// already excludes), never by its spelling. Any ref that HAS a declaration
// is subject to the rule, whatever it is called.
//
// WHAT THIS FILE PINS
//   ROW 1  F1 — a user generic `Object<T>` returned where a bare `T` is
//          declared is REJECTED with §5.1's rule (the miscompile's exact
//          shape; on the unfixed arm it COMPILES and prints a heap address).
//   ROW 2  F1 — the same rule at a LOCAL-VARIABLE initializer site, i.e. the
//          other §5.1 conversion site, not only the return site.
//   ROW 3  CONTROL — the ROOT `Object` still converts to a bare type
//          parameter (§5.1's list has no such row either, but the root is
//          exempted by its RESOLVED IDENTITY — this row is what keeps the
//          fix from becoming a name-independent over-rejection of the root).
//   ROW 4  CONTROL — the user's generic `Object<T>` is STILL ADMITTED as a
//          user type: it is instantiated, its own member is called, and the
//          right value is printed (§8.1, §2.2).
//   ROW 5  CONTROL — an ordinary user class (`Cell`) into a bare type
//          parameter is still rejected with the SAME §5.1 wording: the fix
//          changed the rule's KEY, not the rule.
//   ROW 6  CONTROL — a NESTED `Outer.Object` into a bare type parameter is
//          still rejected, naming `Outer` (its head is not `Object`, so it
//          was never exempt — it must not become exempt).
//
// FAIL-WITHOUT: reverting `src/sema/ExprTypecheck.cpp` to the base (keeping
// the #7992 `TypeResolution_Names.cpp` change) and REBUILDING THE TEST BINARY
// takes rows 1 and 2 RED (the program compiles and the row's "must be
// rejected" assertion fails); every control row stays green on both arms.
//
// FLIPPED TESTS: none. The §5.1 wording this file pins is produced by the
// #5469 arm for EVERY concrete class/record source, and this fix only REMOVES
// one exclusion from it — no existing row's expectation changes (the #5469
// rows for `Cell` and for a nested `Outer.Object` are unchanged, and ROWS 5
// and 6 here re-pin them).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §5.1's own words for "a concrete class or record is not implicitly
// convertible to a bare type parameter". Pinned as SUBSTRINGS on the rule's
// identifying tokens — the SOURCE type, the TARGET type parameter, the
// §5.1 citation and the rule's own sentence — so that a rephrase which
// keeps them does not silently break the pin, and so that a message which
// merely said "not convertible" (without naming the type parameter) would
// NOT satisfy it.
//
// `sourceType` is the HEAD of the source type as the diagnostic names it:
// `Object` for a user's generic `Object<T>` (its type-argument list is not
// part of the head), `Cell` for an ordinary user class, `Outer` for the
// nested `Outer.Object`.
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
// ROW 1 — F1: THE MISCOMPILE'S EXACT SHAPE. A user generic `Object<T>` is
// returned where a bare `T` is declared, and the result is then read as an
// `int`.
//
// Pre-fix (with the #7992 half landed, i.e. the user's `Object<T>` admitted):
// COMPILES, RUNS, and prints a heap address where an `int` was declared —
// the wrong value, not a missing diagnostic.
// Post-fix: REJECTED with §5.1's rule.
// =====================================================================

BEZEL_TEST(builtin_generic_object_typeparam_7997,
           user_generic_object_is_not_convertible_to_a_bare_type_parameter_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "public class Object<T> "
        "  public method Hi(): int => 42; "
        "end Object; "
        "public class Program "
        "  public static method Main(): int "
        "    var x: int := M<int>(); "
        "    Console.WriteLine(x); "
        "    using var o := new Object<int>(); "
        "    Console.WriteLine(o.Hi()); "
        "    return 0; "
        "  end Main; "
        "  public static method M<T>(): owned T => new Object<int>(); "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        sectionFiveOneClassToTypeParamDiagnostic(r, "Object", "T"));
}

// =====================================================================
// ROW 2 — F1 at the OTHER §5.1 conversion site: a local-variable
// initializer inside a generic method (the enclosing method's own type
// parameter is the target). §5.1's rule is a CONVERSION rule, so it applies
// at every conversion site, not only at a return.
//
// Pre-fix: COMPILES (the local is initialized with a class instance where a
// bare `T` is declared).
// Post-fix: REJECTED with §5.1's rule.
// =====================================================================

BEZEL_TEST(builtin_generic_object_typeparam_7997,
           user_generic_object_local_initializer_is_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "public class Object<T> "
        "  public method Hi(): int => 7; "
        "end Object; "
        "public class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "  public static method M<T>(): int "
        "    var x: T := new Object<int>(); "
        "    return 0; "
        "  end M; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        sectionFiveOneClassToTypeParamDiagnostic(r, "Object", "T"));
}

// =====================================================================
// ROW 3 — CONTROL: the ROOT `Object` still converts to a bare type
// parameter. This is the row that keeps the fix from becoming a
// name-independent over-rejection: the root is exempted by its RESOLVED
// IDENTITY (it is the root's own declaration-less `Kind::Primitive` ref),
// never by its spelling, and the §5.1 arm — whose predicate now has NO
// name clause at all — never sees it.
//
// The root's default `ToString` renders `"System.Object"` (§8.5), so the
// printed value names the root. GREEN on both arms.
// =====================================================================

BEZEL_TEST(builtin_generic_object_typeparam_7997,
           root_object_still_converts_to_a_bare_type_parameter_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var r: Object := M<Object>(); "
        "    Console.WriteLine(r.ToString()); "
        "    return 0; "
        "  end Main; "
        "  public static method M<T>(): owned T => new Object(); "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("System.Object\n"));
}

// =====================================================================
// ROW 4 — CONTROL: the user's generic `Object<T>` is STILL ADMITTED as an
// ordinary user type (§8.1: "a generic `Object<T>` is a different name
// (§2.2)"). It is instantiated, its OWN member is called, and the right
// value is printed. This fix narrows §5.1's KEY; it must not narrow
// §8.1's admission.
//
// GREEN on both arms.
// =====================================================================

BEZEL_TEST(builtin_generic_object_typeparam_7997,
           user_generic_object_is_still_admitted_spec_8_1) {
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
// ROW 5 — CONTROL: an ORDINARY user class into a bare type parameter is
// still rejected, with the SAME §5.1 wording. The fix changed the rule's
// KEY (spelling → resolved identity), not the rule: the diagnostic this
// row pins is the one the #5469 arm has always produced for `Cell`.
//
// GREEN on both arms.
// =====================================================================

BEZEL_TEST(builtin_generic_object_typeparam_7997,
           an_ordinary_user_class_is_still_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "public class Cell "
        "  public method Hi(): int => 7; "
        "end Cell; "
        "public class Program "
        "  public static method Main(): int "
        "    var x: int := M<int>(); "
        "    Console.WriteLine(x); "
        "    return 0; "
        "  end Main; "
        "  public static method M<T>(): owned T => new Cell(); "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        sectionFiveOneClassToTypeParamDiagnostic(r, "Cell", "T"));
}

// =====================================================================
// ROW 6 — CONTROL: a NESTED `Outer.Object` into a bare type parameter is
// still rejected, and the diagnostic names `Outer` (the head of the
// qualified reference). §8.1: "a nested type … named `Object` shadows the
// root" — so it is an ordinary user type, and its head was never `Object`,
// so it was never exempt and must not become exempt.
//
// GREEN on both arms.
// =====================================================================

BEZEL_TEST(builtin_generic_object_typeparam_7997,
           a_nested_object_type_is_still_rejected_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "public class Outer "
        "  public class Object "
        "    public method Hi(): int => 7; "
        "  end Object; "
        "end Outer; "
        "public class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "  public static method M<T>(): T => new Outer.Object(); "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        sectionFiveOneClassToTypeParamDiagnostic(r, "Outer", "T"));
}
