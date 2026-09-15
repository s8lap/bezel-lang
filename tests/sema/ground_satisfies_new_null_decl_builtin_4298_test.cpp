// Spec-derived tests for issue #4298 (§14.2 "Constraints" / §19.6 v0.196 /
// §26.4, v0.298): `ExprTC::groundSatisfiesNew` (src/sema/ExprTypecheck.cpp)
// had NO dispatch arm at all for a null-declaration `Named` TypeRef — every
// arm above its final catch-all requires `arg->declaration` to be
// non-null, so EVERY null-declaration builtin (the ~130-name population
// issue #4229 catalogues: `DictionaryEntry`, `Span`, `IEnumerable`,
// `IDisposable`, ...) fell straight through to the unconditional "defer"
// (`decided` stays false) at BOTH of this function's call sites — the
// issue's own claim, verified here: exactly two,
// `leafSolutionViolatesScalarConstraint` (§19.6 choice-exhaustiveness leaf
// filtering) and `extConstraintItemProvablyViolated` (§26.4 extension-
// applicability filtering).
//
// fix #4298 closes the CATEGORICAL slice of this gap in `groundSatisfiesNew`
// itself: the closed 25-name set of null-declaration builtin INTERFACES
// already exposed via `bezel::sema::nonConstructibleBuiltinInterfaceNames()`
// (TypeResolution_Checks.cpp, fix #4309), plus `IGrouping` (fix #4294's own
// one-off) — both unconditionally non-constructible per §14.2 ("Interface
// substitutions never satisfy `new()` — no constructor exists"), with no
// per-type fact to get wrong. The instantiation-dependent remainder
// (`DictionaryEntry`, the `Span` family, `ExceptionInfo`,
// `ReadOnlyStringSpan`) is deliberately OUT of scope for this fix — see the
// follow-up filed alongside it (issue 4333).
//
// ⚠ CORRECTION TO THE ISSUE'S OWN CHARACTERIZATION, found while writing this
// file's exhaustiveness rows (LANE_RULES §1: "verify the issue's claims,
// don't just implement them"): although `groundSatisfiesNew` is genuinely
// blind at BOTH call sites (fixed at both, symmetrically, by this commit),
// the fix's effect is only OBSERVABLE through ONE of them.
// `leafSolutionViolatesScalarConstraint`'s own `violates` lambda
// (ExprTypecheck.cpp) gates on `isFullyGroundTypeRef(arg)` BEFORE ever
// inspecting `item.kind` — and `isFullyGroundTypeRef`'s own header comment
// is explicit that a null-declaration `Named` TypeRef is treated as
// non-ground UNCONDITIONALLY ("a rigid type-parameter reference... or a
// null-declaration stdlib builtin; BOTH are treated as non-ground...
// Conservative on purpose"). So a null-declaration-builtin-bound leaf never
// reaches `groundSatisfiesNew` through THIS consumer at all, whether or not
// `groundSatisfiesNew` itself has an arm for it — confirmed by measurement,
// row (1) below (see its own comment for the exact numbers). The only
// consumer where this fix has a measurable effect is
// `extConstraintItemProvablyViolated` (§26.4), which has no such gate
// (rows (3)/(4)). Fixing `groundSatisfiesNew` itself (rather than only its
// §26.4 call site) is still the right shape — it is the general, correct
// answer to "does a fully-ground σ satisfy `new()`", available to a future
// third consumer or a future loosening of `isFullyGroundTypeRef` — but this
// file's coverage reflects what is ACTUALLY observable today, not the
// issue's original "both call sites" framing.
//
// Row map (neither consumer is covered by the existing #4309 test file,
// which exercises the SIBLING function `Resolver::argSatisfiesNew` only):
//
//   (1) §19.6 STRUCTURAL-BOUNDARY CONTROL: a closed hierarchy's
//       `where Y: new()` leaf bound to `IDisposable` stays REQUIRED
//       (warning still fires) even after this fix — pins that
//       `isFullyGroundTypeRef`'s pre-existing gate keeps this consumer
//       unaffected, so nobody mistakes this file's silence on that
//       consumer for an oversight.
//   (2) §19.6 CONTROL: the same hierarchy with a defaultable `Y` (`int`)
//       also stays REQUIRED — the ordinary, reachable case, unaffected by
//       this fix (`int` is a Primitive TypeRef, decided by the
//       pre-existing Primitive arm) — proves row (1) is not just "every
//       leaf is always required regardless" by having the SAME warning
//       fire for a different, unrelated reason.
//   (3) §26.4 extension applicability: `extension Box<T> where T: new()`
//       is NOT a candidate for a null-declaration-interface receiver
//       (`Box<IDisposable>`), so calling its member is a compile error.
//   (4) §26.4 CONTROL: the SAME block IS a candidate for a defaultable-
//       class receiver (`Box<Widget>`) — must not over-reject.
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

bool hasNonExhaustiveWarning(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool nonExhaustiveWarningMentions(const bezel::test::SemaResult& r,
                                  std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("non-exhaustive") != std::string::npos &&
            d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

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

// Shared hierarchy for the §19.6 rows — same shape as
// `ground_satisfies_new_generic_record_subst_4220_test.cpp`'s own
// `kPrelude4220` + `BGSN4220` closed base.
const char* kPrelude4298 =
    "module M4298; "
    "closed class BGSN4298<T> end BGSN4298; ";

} // namespace

// (1) §19.6 STRUCTURAL-BOUNDARY CONTROL — see this file's header comment
// for the full account. A leaf bound to `IDisposable` (25-name closed-set
// member, now provably non-constructible per §14.2 inside
// `groundSatisfiesNew` itself) stays REQUIRED here, UNCHANGED by this fix,
// because `leafSolutionViolatesScalarConstraint`'s own `violates` lambda
// never reaches `groundSatisfiesNew` for a null-declaration-bound leaf —
// `isFullyGroundTypeRef(arg)` (this file's companion, ExprTypecheck.cpp)
// returns false first and the lambda short-circuits before inspecting
// `item.kind` at all.
//
// MEASURED identical on both sides of the fix (`--area sema`
// `sema_ground_satisfies_new_4298`, this lane's own tree): warning FIRES
// pre-fix and FIRES post-fix — not a FAIL-WITHOUT/PASS-WITH row, a
// same-on-both-sides boundary pin (§20 in this run's standing lane rules:
// state a control's reason explicitly rather than let a same-on-both-sides
// row read as an oversight).
BEZEL_TEST(sema_ground_satisfies_new_4298,
           null_decl_builtin_interface_leaf_unaffected_boundary_spec_19_6) {
    std::string src =
        std::string(kPrelude4298) +
        "sealed class LGSN4298a<Y> : BGSN4298<Y> where Y: new() end LGSN4298a; "
        "sealed class RGSN4298a : BGSN4298<IDisposable> end RGSN4298a; "
        "class CalcGSN4298a "
        "  public method M(b: BGSN4298<IDisposable>): int "
        "    var a := (case b of "
        "      var r: RGSN4298a => 1 "
        "); "
        "    return a; "
        "  end M; "
        "end CalcGSN4298a;";
    auto r = parseAndTypecheckGated(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));  // stays required, unaffected
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "LGSN4298a"));
}

// (2) §19.6 CONTROL: a defaultable `Y` (`int`) also stays REQUIRED, for a
// genuinely different (ordinary, reachable) reason — proves row (1)'s
// warning is the real §19.6 exhaustiveness mechanism at work, not e.g. a
// parse/typecheck failure that fires a warning regardless of leaf shape.
// Unaffected by this fix either way (`int` is a Primitive TypeRef, decided
// by the pre-existing Primitive arm — never reaches the new arm).
BEZEL_TEST(sema_ground_satisfies_new_4298,
           defaultable_primitive_leaf_stays_required_spec_19_6) {
    std::string src =
        std::string(kPrelude4298) +
        "sealed class LGSN4298c<Y> : BGSN4298<Y> where Y: new() end LGSN4298c; "
        "sealed class RGSN4298c : BGSN4298<int> end RGSN4298c; "
        "class CalcGSN4298c "
        "  public method M(b: BGSN4298<int>): int "
        "    var a := (case b of "
        "      var r: RGSN4298c => 1 "
        "); "
        "    return a; "
        "  end M; "
        "end CalcGSN4298c;";
    auto r = parseAndTypecheckGated(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonExhaustiveWarning(r));
    BEZEL_EXPECT_TRUE(nonExhaustiveWarningMentions(r, "LGSN4298c"));
}

// (3) §26.4 THE REGRESSION ROW: `extension Box4298d<T> where T: new()`'s
// applicability filter must drop the block for a null-declaration-
// interface receiver (`Box4298d<IDisposable>`) — `IDisposable` provably
// violates `new()`, so the candidate is excluded and `.Make()` is a
// compile error. Since the ONLY extension declaring `Make` on `Box4298d`
// is the (excluded) candidate itself, `resolveMemberAccess`'s
// `anySameNamedExtension` fallback fires (ExprTypecheck_Calls.cpp) — the
// SAME "no extension method ... takes N argument(s) (§26.5)" wording this
// file's sibling #4306 fix's own tests assert, not the plain "no member"
// text (there IS a same-named extension member on the type; it is merely
// inapplicable at this instantiation).
//
// FAILS WITHOUT THE FIX: `extConstraintItemProvablyViolated`'s `new` arm
// calls the SAME `groundSatisfiesNew`; pre-fix it deferred on
// `IDisposable` (`decided` stays false), so the block was never provably
// excluded and the call wrongly resolved (no diagnostic at all).
BEZEL_TEST(sema_ground_satisfies_new_4298,
           extension_where_new_excludes_null_decl_interface_receiver_spec_26_4) {
    auto r = parseSemaAll(
        "module M4298d; "
        "class Box4298d<T> end Box4298d; "
        "extension Box4298d<T> where T: new() "
        "  public method Make(): int => 5; "
        "end; "
        "class C4298d public static method Main(): int "
        "  var b := new Box4298d<IDisposable>(); "
        "  return b.Make(); "
        "end Main; end C4298d;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasDiagnosticContaining(r, "no extension method `Make`"));
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, "\xC2\xA7""26.5"));
}

// (4) §26.4 CONTROL: the SAME extension applied to a receiver that DOES
// satisfy `new()` (`Widget4298e`, a plain class with the implicit default
// constructor) stays a candidate — the fix must not over-reject.
BEZEL_TEST(sema_ground_satisfies_new_4298,
           extension_where_new_keeps_defaultable_class_receiver_spec_26_4) {
    auto r = parseSemaAll(
        "module M4298e; "
        "class Box4298e<T> end Box4298e; "
        "extension Box4298e<T> where T: new() "
        "  public method Make(): int => 5; "
        "end; "
        "class Widget4298e end Widget4298e; "
        "class C4298e public static method Main(): int "
        "  var b := new Box4298e<Widget4298e>(); "
        "  return b.Make(); "
        "end Main; end C4298e;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
