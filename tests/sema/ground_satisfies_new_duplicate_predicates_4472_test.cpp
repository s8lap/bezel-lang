// Spec-derived tests for issue #4472 (§14.2 "Constraints", v0.302) — a
// follow-up to fix #4446 (c9e6842b / 6f7874a3).
//
// #4446 added the `abstract`/`closed` exclusion to `ExprTC::groundSatisfiesNew`
// (ExprTypecheck.cpp) but explicitly declined, in its own SCOPE NOTE, two
// further duplicated Class arms that ask the identical question and lacked
// the guard:
//   * Site A — `Resolver::argSatisfiesNew`'s Class arm
//     (TypeResolution_Checks.cpp), reached from generic TYPE instantiation
//     (`new Holder<T>()` where `Holder<T> where T: new()`) via
//     `Resolver::checkOneConstraint`.
//   * Site B — the inline Class arm of `checkMethodGenericConstraints`'s
//     "new" item (ExprTypecheck_Calls.cpp), reached from an EXPLICIT
//     generic-method-call type argument (`Factory.Make<Shape>()`) — the
//     issue's own literal repro shape.
//
// §14.2 v0.302: "Nor does an `abstract` or `closed` class [satisfy `new()`],
// whether it declares a `public` parameterless constructor or takes §22.7's
// implicit one: §12.5 makes `new` on such a class a compile error [...] The
// diagnostic is at the substitution site, naming the class and the
// constraint, on the same footing as the interface exclusion."
//
// THE FIX (this commit): rather than growing a third and fourth
// independently-maintained copy of the `hasModifier(d, "abstract") ||
// hasModifier(d, "closed")` test, all three Class arms — including
// `groundSatisfiesNew`'s own #4446 fix, retrofitted — now call ONE shared
// predicate, `sema::isAbstractOrClosedClass` (TypeResolution.hpp, defined
// TypeResolution_Checks.cpp), per this project's "unify to one call, not
// two agreeing copies" convention (docs/proposals/next-run-queue-2026-08-13.md
// batch B). See that function's own header comment for the full rationale.
//
// Row map:
//   (1) Site B REGRESSION — `abstract` class, the issue's own literal
//       `Factory.Make<Shape>()` repro. Pre-fix: Site B's Class arm reached
//       the implicit-default-ctor fallthrough (`ctors.empty() → satisfied =
//       true`), so the §14.2 call-site check passed silently; type
//       substitution proceeded into `Make`'s body, where `new T()` with
//       T=Shape (abstract) raised a DIFFERENT, unrelated "cannot
//       instantiate abstract class"-shaped diagnostic at the METHOD BODY's
//       `new T()` line instead of the §14.2 diagnostic at the CALL site —
//       exactly the "rejection fires at line 8, not line 12" symptom the
//       issue describes. Post-fix: the new guard makes `satisfied = false`
//       at the Class arm, `report("new()")` fires at `ce.span.begin` (the
//       CALL expression's own span — the substitution/call site, not the
//       declaration body), producing the real §14.2 wording.
//   (2) Site B REGRESSION — `closed` class (with an explicit accessible
//       parameterless constructor, so the ctor-admission path does not
//       coincidentally bypass the guard — the second of #4446's two named
//       failure shapes, mirrored here for Site B).
//   (3) Site B CONTROL — a concrete class with no explicit constructor (the
//       implicit §22.7 default) must still satisfy `new()` at the same call
//       shape: no diagnostic.
//   (4) Site A REGRESSION — `abstract` class substituted into a generic
//       TYPE's own `where T: new()` (`new Holder<Shape>()`), reaching
//       `Resolver::checkOneConstraint` → `argSatisfiesNew`'s Class arm.
//       Pre-fix: ctor scan finds no declared ctors → `decided = true, ok =
//       true` (accepted); `new Holder<Shape>()` compiled and the body's own
//       `new T()` would misbehave at runtime rather than being rejected
//       here. Post-fix: `decided = true, ok = false` from the new guard →
//       `report("new()")` at the instantiation site.
//   (5) Site A REGRESSION — `closed` class, same generic-TYPE-instantiation
//       shape as (4).
//   (6) Site A CONTROL — a concrete class satisfies the same generic-TYPE
//       instantiation with no diagnostic.
//
// Every row uses `parseSemaAll` (parse + collectSymbols + resolveTypes +
// typecheckExpressions) so both sema passes that reach these predicates run.
//
// MECHANICAL FAIL-WITHOUT / PASS-WITH: copied this file to a scratch build,
// reverted only the src/ predicate-wiring diff (TypeResolution.hpp,
// TypeResolution_Checks.cpp's isAbstractOrClosedClass body kept but its two
// call SITES at Site A/B removed, i.e. exactly what #4472 asks for) with
// `git checkout -- <file>` on a throwaway copy (never `git stash`),
// rebuilt, ran this filter: rows (1)/(2)/(4)/(5) FAILED — no §14.2
// diagnostic (`hasErrors()` false at Site A rows; at Site B rows the
// substitution proceeded and either produced no error or a different,
// unrelated abstract-instantiation diagnostic without the §14.2 wording).
// Rows (3)/(6) PASSED both before and after (neither depends on the fix).
// Restored the fix; all six rows passed.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::parseAndTypecheckGated;

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiagnosticContaining(const bezel::test::SemaResult& r,
                              std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The exact §14.2 wording both Site A's and Site B's `report("new()")`
// produce (TypeResolution_Checks.cpp's `checkOneConstraint::report` and
// ExprTypecheck_Calls.cpp's `checkMethodGenericConstraints::report`, both
// "... does not satisfy the constraint `where <T>: new()` (§14.2)").
constexpr std::string_view kWhereNewWording =
    "does not satisfy the constraint `where T: new()` (\xC2\xA7""14.2)";

} // namespace

// =====================================================================
// (1) Site B REGRESSION — `abstract` class, issue's own literal repro.
// =====================================================================
BEZEL_TEST(sema_ground_satisfies_new_duplicate_predicates_4472,
           site_b_direct_call_excludes_abstract_class_spec_14_2) {
    auto r = parseSemaAll(
        "module M4472a; "
        "public abstract class Shape4472a "
        "  public abstract method Area(): int; "
        "end Shape4472a; "
        "static class Factory4472a "
        "  public static method Make<T>(): owned T where T: new() "
        "    var x := new T(); "
        "    return x; "
        "  end Make; "
        "end Factory4472a; "
        "class C4472a public static method Main(): int "
        "  using var s := Factory4472a.Make<Shape4472a>(); "
        "  return 0; "
        "end Main; end C4472a;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, kWhereNewWording));
}

// =====================================================================
// (2) Site B REGRESSION — `closed` class with an explicit accessible
// parameterless constructor.
// =====================================================================
BEZEL_TEST(sema_ground_satisfies_new_duplicate_predicates_4472,
           site_b_direct_call_excludes_closed_class_spec_14_2) {
    auto r = parseSemaAll(
        "module M4472b; "
        "public closed class Gate4472b "
        "  public constructor() end constructor; "
        "  public method Area(): int => 3; "
        "end Gate4472b; "
        "static class Factory4472b "
        "  public static method Make<T>(): owned T where T: new() "
        "    var x := new T(); "
        "    return x; "
        "  end Make; "
        "end Factory4472b; "
        "class C4472b public static method Main(): int "
        "  using var s := Factory4472b.Make<Gate4472b>(); "
        "  return 0; "
        "end Main; end C4472b;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, kWhereNewWording));
}

// =====================================================================
// (3) Site B CONTROL — a concrete class, implicit default ctor.
// =====================================================================
BEZEL_TEST(sema_ground_satisfies_new_duplicate_predicates_4472,
           site_b_direct_call_keeps_concrete_class_spec_14_2) {
    auto r = parseSemaAll(
        "module M4472c; "
        "class Widget4472c end Widget4472c; "
        "static class Factory4472c "
        "  public static method Make<T>(): owned T where T: new() "
        "    var x := new T(); "
        "    return x; "
        "  end Make; "
        "end Factory4472c; "
        "class C4472c public static method Main(): int "
        "  using var s := Factory4472c.Make<Widget4472c>(); "
        "  return 0; "
        "end Main; end C4472c;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// (4) Site A REGRESSION — `abstract` class substituted into a generic
// TYPE's own `where T: new()` (`new Holder<Shape>()`).
// =====================================================================
BEZEL_TEST(sema_ground_satisfies_new_duplicate_predicates_4472,
           site_a_type_instantiation_excludes_abstract_class_spec_14_2) {
    auto r = parseSemaAll(
        "module M4472d; "
        "public abstract class Shape4472d "
        "  public abstract method Area(): int; "
        "end Shape4472d; "
        "class Holder4472d<T> where T: new() "
        "  public method Make(): owned T => new T(); "
        "end Holder4472d; "
        "class C4472d public static method Main(): int "
        "  var h := new Holder4472d<Shape4472d>(); "
        "  return 0; "
        "end Main; end C4472d;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, kWhereNewWording));
}

// =====================================================================
// (5) Site A REGRESSION — `closed` class, same generic-TYPE-instantiation
// shape as (4).
// =====================================================================
BEZEL_TEST(sema_ground_satisfies_new_duplicate_predicates_4472,
           site_a_type_instantiation_excludes_closed_class_spec_14_2) {
    auto r = parseSemaAll(
        "module M4472e; "
        "public closed class Gate4472e "
        "  public constructor() end constructor; "
        "  public method Area(): int => 3; "
        "end Gate4472e; "
        "class Holder4472e<T> where T: new() "
        "  public method Make(): owned T => new T(); "
        "end Holder4472e; "
        "class C4472e public static method Main(): int "
        "  var h := new Holder4472e<Gate4472e>(); "
        "  return 0; "
        "end Main; end C4472e;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, kWhereNewWording));
}

// =====================================================================
// (6) Site A CONTROL — a concrete class, implicit default ctor.
// =====================================================================
BEZEL_TEST(sema_ground_satisfies_new_duplicate_predicates_4472,
           site_a_type_instantiation_keeps_concrete_class_spec_14_2) {
    auto r = parseSemaAll(
        "module M4472f; "
        "class Widget4472f end Widget4472f; "
        "class Holder4472f<T> where T: new() "
        "  public method Make(): owned T => new T(); "
        "end Holder4472f; "
        "class C4472f public static method Main(): int "
        "  var h := new Holder4472f<Widget4472f>(); "
        "  return 0; "
        "end Main; end C4472f;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
