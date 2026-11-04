// fix #8044 (spec §2.2 / §8.1 / §14.1 / §19.3, v0.353): a user's GENERIC
// `Object<T>` shadowed the ROOT `Object` in a bare type-test PATTERN —
// `case c of Object` stopped matching.
//
// ── WHAT §2.2 SAYS ABOUT TWO TYPES OF ONE NAME ─────────────────────────────
//
// Spec §2.2 (v0.353, *Imports* → *Collisions*):
//
//   "For this collision rule and the shadowing rule below, names are compared
//    together with their generic arity … and every reference fixes that arity
//    by its type-argument list …: a non-generic `Counter` and the
//    always-available `Counter<K>` are two names, neither colliding with nor
//    shadowing the other … Arity selects among the types rule 1 of §2.7 finds
//    for an identifier: a reference resolves to the first, in that rule's
//    order, whose arity its type-argument list fixes; where none has that
//    arity, it resolves to the first of them and is Appendix C.1's arity
//    error, never an alias, a member or a module prefix."
//
// Spec §8.1 (*Definition*): "User code cannot redeclare or replace `Object`.
// The ban is on a user module-level type declaration — a class, record, enum,
// interface, choice or type alias — named `Object` WITH NO TYPE PARAMETERS, in
// any user module and at any visibility. Every other declaration of the name
// is ordinary: a generic `Object<T>` is a different name (§2.2) … Wherever
// this specification names `Object`, it means the root."
//
// ⇒ The root `Object` (arity 0) and a user's `Object<T>` (arity 1) are TWO
//   names. A BARE reference — one that fixes arity 0 — selects the arity-0
//   one: the root.
//
// §14.1 (*Instantiation and its site*) fixes the arity a bare reference
// fixes: "The rule holds inside a generic type's own declaration — … there,
// too, the bare `Box` fixes arity 0". A PATTERN carries no type-argument
// list at all (grammar A.7 has no type arguments in a pattern — `Box<int>` is
// not a pattern spelling), so a bare type-test pattern name fixes arity 0
// exactly as a bare type reference does.
//
// ── THE DEFECT ──────────────────────────────────────────────────────────────
//
// The pattern position resolved a bare name by NAME ALONE:
//
//   * the sema half — `ExprTC::checkTopLevelNamedPattern`
//     (src/sema/ExprTypecheck.cpp) — took the FIRST `TypeSymbol` of that
//     name out of `ctx_.symbols()`, with no arity comparison;
//   * the runtime half — `Emitter::descriptorForPatternName`
//     (src/codegen/LlvmBackend_Setup.cpp) — took the first CLASS/INTERFACE
//     symbol of that name, and its `descArray_` entry (the shared template
//     descriptor of the generic class), with no arity comparison either.
//
// So a user's `class Object<T>` took the bare `Object` pattern, and the
// pattern's test became an is-a walk against the GENERIC TEMPLATE's
// descriptor — a global no instance's descriptor chain contains (an
// `Object<int>` instance's chain holds the PER-INSTANTIATION descriptor) —
// so the arm never matched.
//
// MEASURED on the parent of this fix (bezelc at 726e0b38):
//
//   class Object<T> … end Object;
//   var c: Object<int> := new Object<int>();
//   (case c of Object => "matched", _ => "nomatch")
//     -> COMPILES, prints `nomatch` — the root `Object` no longer matches.
//
//   class Object<T> … end Object;
//   var o: Object? := "hello";
//   (case o of Object => "matched", _ => "nomatch")
//     -> COMPILES, prints `nomatch` — the ROOT's own pattern stopped
//        matching too, for the same reason.
//
//   (no user `Object` at all)
//   var o: Object? := "hello";
//   (case o of Object => "matched", _ => "nomatch")
//     -> COMPILES, prints `matched` — the ADMITTING TWIN, which must stay
//        green.
//
// ⇒ The TYPE position already applied §2.2's arity selection (a bare
//   `Object` in an annotation names the ROOT even beside a user's
//   `Object<T>` — #7992 / #8008), and the `is` operator agrees
//   (`c is Object` is `True` both before and after this fix). Only the
//   PATTERN position did not.
//
// ── THE FIX ─────────────────────────────────────────────────────────────────
//
// Both halves of the pattern's name resolution now apply §2.2's arity
// SELECTION: the candidate sets are scanned for the arity-0 candidate of the
// name, and an arity>0 candidate (a generic class, a generic alias) is not
// selected by a bare pattern name. For the root `Object` the arity-0
// candidate is the always-available one — the root's own descriptor, the
// `builtinDesc_` entry the #6952 fix's root-`Object` test already uses — so
// the bare `Object` pattern is the ROOT's type test again, and §19.3's
// "Type patterns without binding: `string`, `int`, `Dog`" reads it as the
// root it names.
//
// A name with NO arity-0 candidate at all keeps the pre-fix behavior (the
// generic candidate), so this fix changes ONLY the shadowing case and leaves
// every other shape's verdict where it was.
//
// ── WHAT EACH ROW IS FOR ───────────────────────────────────────────────────
//
//   8044-1  the issue's own shape: with a user's generic `Object<T>`
//            declared, `case c of Object` over an `Object<int>` instance
//            MATCHES (before the fix: `nomatch`).
//   8044-2  the ADMITTING TWIN: with NO user `Object` declared, the root
//            `Object` still matches a non-generic `Object` value (before
//            the fix: `matched` — this row must stay green).
//   8044-3  the admitting twin WITH the user's generic `Object<T>` present:
//            the root `Object` still matches a non-generic `Object` value
//            (before the fix: `nomatch`).
//   8044-4  CONTROL: the user's generic `Object<T>` is an ORDINARY type
//            (§8.1): its instantiation is still constructed, and the TYPED
//            binding `var x: Object<int>` still matches it.
//   8044-5  CONTROL: the `is` operator is untouched — it already applied
//            §2.2's arity selection (the TYPE position, #7992 / #8008).
//   8044-6  CONTROL: the sema half's §19.3 rejection of a reference-typed
//            pattern over a VALUE-typed scrutinee is unchanged, and its
//            diagnostic text is unchanged.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"          // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The user's generic `Object<T>` — §8.1: "a generic `Object<T>` is a
// different name (§2.2)", so the DECLARATION is admitted.
constexpr const char* kUserGenericObject =
    "class Object<T> "
    "  public v: int; "
    "end Object; ";

}  // namespace

// ── 8044-1 ─ the issue's own shape: with the user's generic `Object<T>`
// declared, the bare `Object` pattern names the ROOT (§2.2's arity
// selection: the bare name fixes arity 0, and the root is the arity-0
// candidate), so it MATCHES the `Object<int>` instance. Before the fix the
// pattern took the user's generic template descriptor and the arm never
// matched — the program printed `nomatch`.
BEZEL_TEST(user_generic_object_type_pattern_8044,
           issue_repro_bare_object_pattern_matches_the_generic_instance) {
    auto r = runAot(
        "module M8044A; " + std::string(kUserGenericObject) +
        "class C public static method Main(): int "
        "  var c: Object<int> := new Object<int>(); "
        "  var r := (case c of Object => \"matched\", _ => \"nomatch\"); "
        "  Console.WriteLine(r); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("matched\n"));
}

// ── 8044-2 ─ the ADMITTING TWIN: with NO user `Object` declared, the root
// `Object` matches a non-generic `Object` value (a string handed to an
// `Object?` carrier). This row is green before the fix and MUST stay green.
BEZEL_TEST(user_generic_object_type_pattern_8044,
           admitting_twin_root_object_matches_a_non_generic_object_value) {
    auto r = runAot(
        "module M8044B; "
        "class C public static method Main(): int "
        "  var o: Object? := \"hello\"; "
        "  var r := (case o of Object => \"matched\", _ => \"nomatch\"); "
        "  Console.WriteLine(r); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("matched\n"));
}

// ── 8044-3 ─ the admitting twin WITH the user's generic `Object<T>`
// present: the user's generic is a DIFFERENT name (§8.1), so it does not
// take the root's bare `Object` pattern, and the root still matches a
// non-generic `Object` value. Before the fix this printed `nomatch`.
BEZEL_TEST(user_generic_object_type_pattern_8044,
           admitting_twin_holds_beside_a_user_generic_object) {
    auto r = runAot(
        "module M8044C; " + std::string(kUserGenericObject) +
        "class C public static method Main(): int "
        "  var o: Object? := \"hello\"; "
        "  var r := (case o of Object => \"matched\", _ => \"nomatch\"); "
        "  Console.WriteLine(r); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("matched\n"));
}

// ── 8044-4 ─ CONTROL: the user's generic `Object<T>` is an ORDINARY type
// (§8.1: "Every other declaration of the name is ordinary"). Its
// instantiation is still constructed, and the TYPED binding
// `var x: Object<int>` — which names the instantiation, arity 1 — still
// matches it. The fix changes the BARE name's selection only.
BEZEL_TEST(user_generic_object_type_pattern_8044,
           control_the_user_generic_object_is_still_an_ordinary_type) {
    auto r = runAot(
        "module M8044D; " + std::string(kUserGenericObject) +
        "class C public static method Main(): int "
        "  var c: Object<int> := new Object<int>(); "
        "  var r := (case c of var x: Object<int> => \"matched\", _ => \"nomatch\"); "
        "  Console.WriteLine(r); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("matched\n"));
}

// ── 8044-5 ─ CONTROL: the `is` operator is untouched. Its target is a TYPE
// position, and the TYPE position already applied §2.2's arity selection
// (a bare `Object` there names the root beside a user's `Object<T>` — #7992
// / #8008), so `c is Object` answers `True` before and after this fix.
BEZEL_TEST(user_generic_object_type_pattern_8044,
           control_the_is_operator_is_unchanged) {
    auto r = runAot(
        "module M8044E; " + std::string(kUserGenericObject) +
        "class C public static method Main(): int "
        "  var c: Object<int> := new Object<int>(); "
        "  Console.WriteLine(c is Object); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

// ── 8044-6 ─ CONTROL: the sema half's §19.3 rejection of a reference-typed
// pattern over a VALUE-typed scrutinee is unchanged, and so is its
// diagnostic text (the pattern still names a reference type — the root).
BEZEL_TEST(user_generic_object_type_pattern_8044,
           control_the_value_scrutinee_rejection_is_unchanged) {
    auto r = runAot(
        "module M8044F; " + std::string(kUserGenericObject) +
        "class C public static method Main(): int "
        "  var x: int := 1; "
        "  var r := (case x of Object => \"matched\", _ => \"nomatch\"); "
        "  Console.WriteLine(r); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "type pattern `Object` is incompatible with the scrutinee's static "
        "type — a value-type scrutinee can never match a reference-typed type "
        "pattern (§19.3 / §19.5)"));
}
