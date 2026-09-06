// fix #3632 (§28.9, §3.6 rule 2, v0.286) — the §28.9 "no `owned` fields of
// NON-TRIVIAL-DISPOSE TYPE" recursive clause, walked coinductively. §28.9
// verbatim:
//
//   "an `owned` field whose declared type is itself trivial-dispose does not
//    disqualify the holder — the synthesized disposal cascade over such fields
//    frees memory and runs no user code, which is exactly what the property
//    asserts. Mutually-recursive `owned` trivial-dispose references are
//    admitted; the property is evaluated coinductively (assume the types
//    under test trivial-dispose, then check the rules)"
//
// The pre-#3632 `common::isNonTrivialDisposeClass` (the `TypeSymbol`-only
// overload, retained for the exporter and other ctx-free callers) used the
// conservative short-circuit: any non-static `owned` field disqualifies.
// This file exercises the new `TypeRef+SemaContext` overload that does the
// widening — the SAME predicate, just with access to a `SemaContext` so a
// field's RESOLVED TypeRef (its post-substitution genericArgs included,
// §3.6 rule 2) can be inspected.
//
// These rows would all return `true` under the pre-#3632 conservative
// predicate; the widening is the spec's reading. The widening does NOT
// reach the §21.9 / §28.3 / §22.15 / §28.10 call sites from this lane
// (those are L1 / W2-L2 / exporter-owned — see the follow-up issue filed
// with this PR); each must update its single `isNonTrivialDisposeClass`
// call to pass the active `SemaContext`/TypeRef. This test pins the
// predicate's NEW behaviour and would fail without the widening.

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "common/OwnershipFacts.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::common::ImportedDisposeVerdict;
using bezel::common::isNonTrivialDisposeClass;
using bezel::sema::Symbol;
using bezel::sema::TypeRef;
using bezel::sema::TypeSymbol;
using bezel::test::SemaResult;

namespace {

// Same harness as `choice_classification_35_4_test.cpp`: lex+parse+collect
// + resolve-types, then locate the named TypeSymbol via the module's
// children list. Resolution is what populates the field TypeRefs the
// widening reads through `sema::storageComponentType`.
// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
SemaResult resolveAll(std::string_view src) {
    return bezel::test::parseAndResolveGated(src);
}

const TypeSymbol* findType(const SemaResult& r, std::string_view name) {
    if (!r.ctx || !r.ctx->module()) return nullptr;
    for (Symbol* child : r.ctx->module()->children) {
        if (auto* t = dynamic_cast<TypeSymbol*>(child)) {
            if (t->name == name) return t;
        }
    }
    return nullptr;
}

// Build the holder TypeRef the new overload expects (Kind::Named,
// declaration pointing at the named class).
bool nonTrivialViaNewOverload(const SemaResult& r, std::string_view name) {
    auto* ts = findType(r, name);
    if (!ts || !r.ctx) return false;
    TypeRef rt;
    rt.kind = TypeRef::Kind::Named;
    rt.declaration = const_cast<TypeSymbol*>(ts);
    return isNonTrivialDisposeClass(&rt, *r.ctx);
}

// PRE-/POST-FIX DOUBLE-CHECK: the legacy pre-#3632 (TypeSymbol-only)
// overload MUST keep its short-circuit answer — the safe-direction
// requirement on ctx-free callers (the exporter). Both overloads live in
// the same translation unit; this asserts the conservative arm is
// preserved alongside the widening.
bool nonTrivialViaLegacyOverload(const SemaResult& r, std::string_view name) {
    auto* ts = findType(r, name);
    if (!ts) return false;
    return isNonTrivialDisposeClass(ts);
}

}  // namespace

// ===========================================================================
// T1 — CARDINAL (issue body): `class K { owned inner: Inner? }` where Inner
// is trivial-dispose. The pre-#3632 short-circuit returns TRUE (K is
// non-trivial-dispose, because it has an `owned` field at all). The
// WIDENED overload returns FALSE — the recursive clause admits Inner as a
// trivial-dispose field type, so K itself is trivial-dispose.
//
// per §28.9 (v0.286) "an `owned` field whose declared type is itself
// trivial-dispose does not disqualify the holder"; coinductive clause.
// ===========================================================================
BEZEL_TEST(trivial_dispose_recursive_clause_3632,
           owned_field_of_trivial_dispose_is_not_disqualifying_spec_28_9) {
    auto r = resolveAll(
        "module M; "
        "class Inner "
        "  public v: int := 0; "
        "  public constructor() end constructor; "
        "end Inner; "
        "class K "
        "  public owned inner: Inner? := null; "
        "  public constructor() end constructor; "
        "end K;");
    // Pre-#3632 behavior: any non-static `owned` field disqualifies.
    BEZEL_EXPECT_TRUE(nonTrivialViaLegacyOverload(r, "K"));
    // WIDENED (this fix): the field's type is trivial-dispose → K is too.
    BEZEL_EXPECT_FALSE(nonTrivialViaNewOverload(r, "K"));
}

// ===========================================================================
// T2 — invariant: a Holder whose owned field's type IS non-trivial-dispose
// must STILL classify non-trivial under the widened overload. (If the
// widening silently admitted the previous case, this case would
// regress — the fix must widen, not invert the verdict.)
//
// `Res` has a destructor → non-trivial-dispose per §28.9 v0.286.
// ===========================================================================
BEZEL_TEST(trivial_dispose_recursive_clause_3632,
           owned_field_of_non_trivial_dispose_keeps_disqualifying_spec_28_9) {
    // fix #3985: `destructor begin end destructor;` used a `begin` keyword
    // that does not exist anywhere in Bezel's grammar (block bodies are a
    // bare `Statement*` — `DestructorDecl ::= "destructor" Statement* "end"
    // "destructor"? ";"`, Appendix A). This never parsed, so the row was
    // reaching sema on resolveAll's ungated, error-recovery-truncated AST.
    // VERIFIED against the real `bezelc -c`: dropping `begin` parses
    // cleanly.
    auto r = resolveAll(
        "module M; "
        "class Res "
        "  public v: int := 0; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class H "
        "  public owned r: Res? := null; "
        "  public constructor() end constructor; "
        "end H;");
    // Both old and new agree: H is non-trivial-dispose (Res has destructor,
    // and an owned-non-trivial field disqualifies whether the walker reads
    // the field's modifier or its resolved type — both reject).
    BEZEL_EXPECT_TRUE(nonTrivialViaLegacyOverload(r, "H"));
    BEZEL_EXPECT_TRUE(nonTrivialViaNewOverload(r, "H"));
}

// ===========================================================================
// T3 — invariant 2: a class with NO owned field at all stays trivial under
// both overloads (sanity — the widening does not perturb the trivial case).
// ===========================================================================
BEZEL_TEST(trivial_dispose_recursive_clause_3632,
           plain_class_with_no_owned_field_is_trivial_both_overloads_spec_28_9) {
    auto r = resolveAll(
        "module M; "
        "class Plain "
        "  public v: int := 0; "
        "  public constructor() end constructor; "
        "end Plain;");
    BEZEL_EXPECT_FALSE(nonTrivialViaLegacyOverload(r, "Plain"));
    BEZEL_EXPECT_FALSE(nonTrivialViaNewOverload(r, "Plain"));
}

// ===========================================================================
// T4 — coinduction (mutually-recursive owned trivial-dispose references):
//   class A { owned b: B? := null; }
//   class B { owned a: A? := null; }
// Neither A nor B has a destructor or non-trivial-dispose storage. The
// pre-#3632 short-circuit would BOTH return TRUE (any owned field
// disqualifies); the COINDUCTIVE widening admits both.
//
// `class K { owned inner: Inner? }` (T1) and
// `class P { owned other: Q? } class Q { owned p: P? }` (this case) are
// the same shape — the property is mutual recursion through owned fields,
// and the spec text names it verbatim. Verifies the in-progress path set
// terminates each cycle to the coinductive fixpoint (trivially
// trivial-dispose) and does not infinite-loop or stack-overflow.
// ===========================================================================
BEZEL_TEST(trivial_dispose_recursive_clause_3632,
           mutually_recursive_owned_trivial_dispose_admitted_spec_28_9) {
    auto r = resolveAll(
        "module M; "
        "class P "
        "  public owned other: Q? := null; "
        "  public constructor() end constructor; "
        "end P; "
        "class Q "
        "  public owned p: P? := null; "
        "  public constructor() end constructor; "
        "end Q;");
    // The widening admits both — coinductive fixpoint.
    BEZEL_EXPECT_FALSE(nonTrivialViaNewOverload(r, "P"));
    BEZEL_EXPECT_FALSE(nonTrivialViaNewOverload(r, "Q"));
    // Legacy short-circuit keeps them non-trivial in the conservative path;
    // that is documented as the safe direction for ctx-free callers.
    BEZEL_EXPECT_TRUE(nonTrivialViaLegacyOverload(r, "P"));
    BEZEL_EXPECT_TRUE(nonTrivialViaLegacyOverload(r, "Q"));
}
