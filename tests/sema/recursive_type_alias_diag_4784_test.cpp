// fix #4784 (§27.8, v0.307): recursive-alias diagnostic duplicated for a
// nested self-reference.
//
// The §17 review of fix #4764 (dc1ab538, commit 2334e1c0) found the on-stack
// cycle guard #4764 added reports "is recursive" once per re-entrant PUSH,
// not once per distinct self-referencing occurrence in the source. Two
// distinct mechanisms both re-enter the guarded alias-transparency arm on an
// occurrence that has already been reported:
//
//   (a) a built-in generic head (`Array<T>`, no user `TypeSymbol`) resolves
//       its Named `TypeRef` with `declaration == nullptr` by design (the
//       "always-available built-in type names" arm of `resolveNamed`) --
//       which is EXACTLY the shape `Resolver::resolveAliasForwardReferences`
//       (fix #4047, a separate fixed-point pass run once after
//       `resolveTopDecl`'s loops) uses to detect an UNRESOLVED forward alias
//       reference. So it re-resolves an already-correctly-rejected cyclic
//       alias body a SECOND time, re-walking the same self-reference and
//       reporting the same diagnostic at the same location again:
//       `type Loop := Array<Loop>;` reported "is recursive" TWICE, both at
//       the same `Loop` occurrence inside `Array<Loop>`.
//   (b) a compound alias body with MORE THAN ONE top-level occurrence of the
//       alias name (a function type's parameter AND return slot,
//       `type Fn := (Fn) -> Fn;`) resolves occurrence A's "underlying type"
//       by re-walking the WHOLE body -- including occurrence B -- while A is
//       on-stack, then resolves occurrence B independently by re-walking the
//       whole body again -- including A -- while B is on-stack: each
//       occurrence is reported once from the OTHER's expansion and once from
//       its OWN, four reports for two real occurrences.
//
// THE FIX (src/sema/TypeResolution_impl.hpp, src/sema/TypeResolution_Names.cpp):
// `Resolver::reportedRecursiveAliasSites_` tracks which (aliasDecl, exact
// source location) pairs have already had "is recursive" reported, and
// suppresses a repeat report at a SITE already reported for that alias. This
// collapses (a) to exactly one report (both re-entrant visits share the same
// location) and (b) to exactly one report PER DISTINCT occurrence (the
// parameter slot and the return slot carry different locations, so neither
// occurrence's own report is suppressed by the other's -- a genuinely
// distinct self-reference is never silenced, only a re-visit of a site
// already reported).
//
// Test oracle: `diagnosticMessageCount` (tests/support/aot_result_asserts.hpp,
// added by this fix) counts occurrences of a substring across ALL of a
// compile's diagnostics -- `diagnosticMessageContains`'s boolean "at least
// one" cannot distinguish "reported once" from "reported twice", which is
// precisely the defect being pinned. Every reject row below also uses
// `failedAtCompileTimeWithRealLocation` per the #4764 test file's own
// discipline, so a crash/signal/timeout outcome (which could otherwise
// satisfy a bare diagnostic-count check vacuously via its own backstop
// diagnostic) is excluded first.
//
// OPEN QUESTION (left unchanged, per this issue's second defect): a
// where-clause bound that names the alias itself, e.g.
// `type Sorted<T> := Array<T> where T: Sorted<T>;`, is reported with the
// SAME "type alias `Sorted` is recursive (§27.8)" wording even though the
// alias's aliased BODY (`Array<T>`) is not cyclic -- only the constraint
// bound refers back to the alias. Spec §14.2's literal self-referential-bound
// rule ("A self-referential bound (`where T: T`)... is a compile error at
// the declaration") names only the bare `where T: T` shape, not
// `where T: Alias<T>`; §14.2's "recurrence constraint" passage separately
// legitimizes `where T: INumber<T>` as ordinary F-bounded-polymorphism
// machinery for INTERFACES. Neither passage explicitly settles whether a
// type ALIAS's own where-clause self-reference should be (i) accepted like
// the interface case, (ii) rejected under the existing §14.2 self-bound
// diagnostic (`generic_type_param_self_bound_1844__self_reject.bzl`'s
// wording), or (iii) rejected under §27.8's alias-recursion wording as it is
// today. Before fix #4764 this shape crashed (SIGSEGV); today's rejection is
// an improvement either way, so this test PINS the current (still §27.8)
// wording as a known-current-behavior regression guard without deciding the
// open question -- re-routing it is left to a follow-up issue.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageCount
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// Defect (a): built-in generic head as the whole aliased body. The alias
// name occurs exactly ONCE in the source (`Array<Loop4784a>`); pre-fix this
// reported "is recursive" TWICE at that one occurrence.
// ---------------------------------------------------------------------------
BEZEL_TEST(recursive_type_alias_diag_4784, nested_generic_arg_reported_once) {
    auto r = runAot(
        "module M; "
        "type Loop4784a := Array<Loop4784a>; "
        "public static class Program4784a "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "type alias `Loop4784a` is recursive"));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, "is recursive"), 1);
}

// ---------------------------------------------------------------------------
// Defect (b): a compound body (function type) with TWO distinct top-level
// occurrences of the alias name -- one in parameter position, one in return
// position. Pre-fix this reported "is recursive" FOUR times (two per
// occurrence, one from its own expansion and one from the other's). Each
// occurrence is a genuinely distinct self-reference at a distinct source
// location, so the correct post-fix count is exactly TWO -- one per
// occurrence -- not one (which would silently swallow the return-position
// occurrence) and not four (the pre-fix duplication).
// ---------------------------------------------------------------------------
BEZEL_TEST(recursive_type_alias_diag_4784,
           function_type_each_occurrence_reported_once) {
    auto r = runAot(
        "module M; "
        "type Fn4784b := (Fn4784b) -> Fn4784b; "
        "public static class Program4784b "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "type alias `Fn4784b` is recursive"));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, "is recursive"), 2);
}

// ---------------------------------------------------------------------------
// Defect 2 (wording, LEFT UNCHANGED -- see the open question above): a
// where-clause bound on the alias's OWN type parameter names the alias
// itself. This is not the aliasedType cycle #4764/#4784 target -- the
// aliased body (`Array<T>`) is concrete and non-cyclic -- but it currently
// reports through the same §27.8 "is recursive" path because the
// constraint-validation call re-enters the guarded arm while the alias is
// still on-stack from the outer call validating that very constraint. This
// row pins CURRENT behavior (a single, non-duplicated report -- #4784's
// dedup applies here too) as a regression guard, not an endorsement of the
// wording.
// ---------------------------------------------------------------------------
BEZEL_TEST(recursive_type_alias_diag_4784,
           where_clause_self_bound_current_wording_pinned) {
    auto r = runAot(
        "module M; "
        "type Sorted4784c<T> := Array<T> where T: Sorted4784c<T>; "
        "public static class Program4784c "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, "is recursive"), 1);
}
