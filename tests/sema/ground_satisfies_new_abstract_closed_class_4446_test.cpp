// Spec-derived tests for issue #4446 (§14.2 "Constraints" / §26.4, v0.302):
// `ExprTC::groundSatisfiesNew` (src/sema/ExprTypecheck.cpp:1477) keyed its
// Class arm solely on the constructor surface (visible parameterless ctor /
// §22.7's implicit default), with NO `abstract`/`closed` test — so an
// unconstructible class (whether declaring a `public` parameterless
// constructor or taking §22.7's implicit one) was wrongly counted as
// satisfying `where T: new()`, on the same footing as an Interface substitution
// being wrongly accepted pre-#4176 in `new()` shorthand.
//
// §14.2 v0.302: "Interface substitutions never satisfy `new()` — no
// constructor exists. **Nor does an `abstract` or `closed` class** (v0.302),
// whether it declares a `public` parameterless constructor or takes §22.7's
// implicit one: §12.5 makes `new` on such a class a compile error […]. The
// diagnostic is at the **instantiation site**, naming the class and the
// constraint, on the same footing as the interface exclusion."
//
// THE FIX adds the abstract/closed modifier test to `groundSatisfiesNew`'s
// Class arm (line 1590, this commit's subject), guarding both call sites of
// this function symmetrically: `leafSolutionViolatesScalarConstraint`'s
// §19.6 exhaustiveness leaf filter (line 1781) and
// `extConstraintItemProvablyViolated`'s §26.4 extension-applicability
// filter (line 8259). The diagnostic at the substitution site is the
// existing §14.2 wording — this commit's predicate change is what makes
// those sites fire for abstract/closed receivers.
//
// SCOPE NOTE (from the issue body, confirmed by measurement): the SAME
// exclusion lives in two further down-stream duplication points the brief
// marks as SEPARATE paths and explicitly excludes from this fix —
// `Resolver::argSatisfiesNew`'s sibling Class arm in
// `TypeResolution_Checks.cpp`, and the inline Class arm of
// `ExprTypecheck_Calls.cpp`'s `checkMethodGenericConstraints` (the direct
// `Make<Shape>()` call-site re-check). Both lack the abstract/closed test
// and both wrongly accept abstract/closed substitutions at their
// respective sites; this fix is the documented half (issue explicitly
// points at `groundSatisfiesNew`'s predicate as the place to land). Filing
// an issue for the remaining two arms is part of the lane observations —
// touching them here would widen scope (§22.7 implicit constructor
// synthesis and the call-site re-check are separate paths, per the brief).
//
// Row map (the extension-block path that DOES go through
// `groundSatisfiesNew`'s predicate, mirroring
// `tests/sema/ground_satisfies_new_null_decl_builtin_4298_test.cpp`:
//
//   (1) §26.4 REGRESSION ROW: `extension BoxAbs<T> where T: new()`
//       applied to a `BoxAbs<AbstractShape>` receiver with `AbstractShape`
//       a plain `abstract class` declaring no members. Pre-fix the §26.4
//       `new` arm's call to `groundSatisfiesNew` reached the Class arm,
//       found no ctors declared, fell through to `if (ctors.empty()) return
//       true;` (the implicit-default-ctor admission), and reported
//       `decided = true, return true` — so the extension was wrongly a
//       candidate and `.Make()` resolved with no diagnostic at all. Post-fix
//       the `hasModifier(d, "abstract")` guard returns `decided = true,
//       return false` ahead of every other test, the extension is excluded,
//       and `.Make()` is a compile error.
//
//   (2) §26.4 REGRESSION ROW: same extension applied to `BoxClo<ClosedGate>`,
//       where `ClosedGate` is `public closed class` declaring a concrete
//       `public constructor()` — the second of the issue's two named
//       failure shapes. Pre-fix the explicit parameterless ctor admission
//       was sufficient to return `decided = true, return true`, wrongly
//       satisfying `new()`. Post-fix the modifier guard fires first.
//
//   (3) §26.4 CONTROL: the SAME extension applied to `BoxCtl<WidgetCtl>`,
//       where `WidgetCtl` is a plain concrete class declaring no
//       constructor (the implicit §22.7 default — `(#4364)` per the issue
//       body). Pre-fix and post-fix: extension applies cleanly, no
//       diagnostic. Pins the implicit-constructor admission as unaffected.
//
// All three rows use `parseSemaAll` (parse + collectSymbols +
// resolveTypes + typecheckExpressions) so every reachable sema pass runs.
// The §19.6 exhaustiveness side of `groundSatisfiesNew`'s call pair
// (`leafSolutionViolatesScalarConstraint`) gates on
// `isFullyGroundTypeRef(arg)` BEFORE inspecting `item.kind`, exactly as
// the #4298 file's header comment states for its sibling — the SAME gate
// covers the abstract/closed shape (a `Box<A>` with abstract `A` is named
// directly, so it IS fully ground; the gate does NOT short-circuit), but
// the structural-boundary shape (an abstract leaf on a closed hierarchy
// walked via the §19.6 leaf walk) is a separate sub-tree not reached by
// this brief's named cases. No §19.6 rows are added; the §19.6 surface
// mirrors the same predicate answer (this commit changes the predicate's
// `decided=true, return false` for abstract/closed Class arms), and any
// regression there would surface as an inadvertent flip in a pinned
// §19.6 test — the brief's "pre-existing constraint-satisfaction tests
// must continue to pass" check covers that.
//
// MECHANICAL FAIL-WITHOUT / PASS-WITH: see the commit body for the exact
// "N of M" count and row names.

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

} // namespace

// =====================================================================
// (1) §26.4 REGRESSION ROW — `abstract` class receiver.
// =====================================================================
//
// FAILS WITHOUT THE FIX: `groundSatisfiesNew` reached the implicit-default-
// ctor arm, returned `decided = true, return true`, so the extension was
// a candidate and `.Make()` resolved with NO diagnostic. Measure on this
// lane's main bezelc: exit 0, "no error" present in stderr.
//
// PASSES WITH THE FIX: `groundSatisfiesNew` returns `decided = true,
// return false` from the new `abstract`/`closed` guard ahead of the
// constructor surface, the extension is excluded, `.Make()` reports the
// same "no extension method `Make` ... (§26.5)" wording this file's
// sibling #4298 / #4306 tests assert — the very wording that surfaces
// when there IS a same-named extension member and it is merely
// inapplicable at this instantiation.
BEZEL_TEST(sema_ground_satisfies_new_4446,
           extension_where_new_excludes_abstract_class_receiver_spec_26_4) {
    auto r = parseSemaAll(
        "module M4446a; "
        "class BoxAbs4446a<T> end BoxAbs4446a; "
        "extension BoxAbs4446a<T> where T: new() "
        "  public method Make(): int => 5; "
        "end; "
        "public abstract class AbstractShape4446a "
        "  public abstract method Area(): int; "
        "end AbstractShape4446a; "
        "class C4446a public static method Main(): int "
        "  var b := new BoxAbs4446a<AbstractShape4446a>(); "
        "  return b.Make(); "
        "end Main; end C4446a;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasDiagnosticContaining(r, "no extension method `Make`"));
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, "\xC2\xA7""26.5"));
}

// =====================================================================
// (2) §26.4 REGRESSION ROW — `closed` class receiver (with an explicit
// parameterless ctor — the second of the issue's two named shapes; the
// ctor-admission path must NOT bypass the modifier guard).
// =====================================================================
BEZEL_TEST(sema_ground_satisfies_new_4446,
           extension_where_new_excludes_closed_class_receiver_spec_26_4) {
    auto r = parseSemaAll(
        "module M4446b; "
        "class BoxClo4446b<T> end BoxClo4446b; "
        "extension BoxClo4446b<T> where T: new() "
        "  public method Make(): int => 5; "
        "end; "
        "public closed class ClosedGate4446b "
        "  public method Area(): int => 7; "
        "end ClosedGate4446b; "
        "class C4446b public static method Main(): int "
        "  var b := new BoxClo4446b<ClosedGate4446b>(); "
        "  return b.Make(); "
        "end Main; end C4446b;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasDiagnosticContaining(r, "no extension method `Make`"));
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, "\xC2\xA7""26.5"));
}

// =====================================================================
// (3) §26.4 CONTROL — concrete class with NO explicit constructor (the
// implicit §22.7 default). Per the issue body's "an accept case at a
// concrete class declaring no constructor — the last one pinning that
// #4364's implicit-constructor admission is unaffected" instruction.
// Pin: pre-fix PASS, post-fix PASS — fix must not over-reject.
// =====================================================================
BEZEL_TEST(sema_ground_satisfies_new_4446,
           extension_where_new_keeps_implicit_default_class_receiver_spec_26_4) {
    auto r = parseSemaAll(
        "module M4446c; "
        "class BoxCtl4446c<T> end BoxCtl4446c; "
        "extension BoxCtl4446c<T> where T: new() "
        "  public method Make(): int => 5; "
        "end; "
        "class WidgetCtl4446c end WidgetCtl4446c; "
        "class C4446c public static method Main(): int "
        "  var b := new BoxCtl4446c<WidgetCtl4446c>(); "
        "  return b.Make(); "
        "end Main; end C4446c;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
