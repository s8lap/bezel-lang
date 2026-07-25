// Spec-derived regression tests for issue #2111 — §14.2 "Derived-property
// constraints" (v0.214): "the per-instantiation re-check for those interior
// uses is subsumed by the constraint, and ONE diagnostic (the constraint
// failure, at the instantiation site) replaces the interior one."
//
// Today (pre-fix), a generic TYPE that both (a) carries `where K: hashable`
// and (b) uses K as an interior hash-container key (`Dictionary<K, …>`)
// produces TWO diagnostics for a non-equality-bearing instantiation: the
// §14.2 constraint failure (Resolver::checkOneConstraint) AND the interior
// §3.6-rule-2 hash-container-key failure (Resolver::checkHashContainerKeys
// pass 2). The spec says the constraint diagnostic should REPLACE the
// interior one — a quality/noise gap, not a soundness bug.
//
// The fix suppresses the interior check ONLY when the enclosing type declares
// a matching `where <param>: hashable` constraint on that exact key param, so
// an UNCONSTRAINED generic's interior use (the `bare_t_unconstrained_*` shape,
// tests/sema/hashable_constraint_2087_test.cpp) still draws its interior
// diagnostic — included here as a soundness guard against over-suppression.
//
// Authority: spec §14.2 "Derived-property constraints" (v0.214) + §3.6
// equality-bearing property. The interior reject text is pinned by the lit
// track; here we assert presence/absence of the two diagnostic shapes.

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::SemaResult;

namespace {

SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff the §14.2 constraint failure naming `hashable` was reported at the
// instantiation site ("type argument … does not satisfy the constraint
// `where K: hashable` (§14.2)").
bool hasHashableConstraintError(const SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not satisfy the constraint") != std::string::npos &&
            d.message.find("hashable") != std::string::npos)
            return true;
    return false;
}

// True iff the interior §3.6/§8.3 hash-container-key diagnostic was reported
// ("`Dictionary` requires an equality-bearing key type, but the type argument
// … is not equality-bearing (§3.6/§8.3)"). This is the diagnostic §14.2 says
// the constraint failure REPLACES for a `where K: hashable`-constrained param.
bool hasInteriorHashKeyError(const SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("requires an equality-bearing") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ─── THE BUG: a constrained interior Dictionary<K,…> double-diagnosed ────────

// §14.2 (v0.214): `Registry<K> where K: hashable` whose body uses K as an
// interior `Dictionary<K, int>` key, instantiated with a NON-equality-bearing
// record (`Holder` — a positional record over a class-typed field, §9.3),
// draws the §14.2 constraint failure at the instantiation site. The interior
// §3.6-rule-2 check is SUBSUMED: per §14.2 "one diagnostic … replaces the
// interior one." Pre-fix this reported BOTH (the double-diagnosis of #2111);
// post-fix only the constraint diagnostic remains.
BEZEL_TEST(hashable_interior_double_diag_2111,
           constrained_interior_dictionary_one_diag_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "public class Registry<K> where K: hashable "
        "  public method Put(d: Dictionary<K, int>) end Put; "
        "end Registry; "
        "class C public static method Main(): int "
        "  using var r := new Registry<Holder>(); "
        "  return 0; "
        "end Main; end C;");
    // The §14.2 constraint diagnostic IS reported (the bad instantiation is
    // still rejected — soundness preserved).
    BEZEL_EXPECT_TRUE(hasHashableConstraintError(r));
    // The interior §3.6/§8.3 hash-container-key diagnostic is NOT reported —
    // subsumed by the constraint per §14.2. (Pre-fix this fired; the fix
    // suppresses it.)
    BEZEL_EXPECT_FALSE(hasInteriorHashKeyError(r));
}

// ─── POSITIVE: a hashable concrete argument draws zero diagnostics ───────────

// §14.2 / §9.1: the same constrained interior shape with a HASHABLE concrete
// argument (`Point` — a positional record over admissible components) draws
// NEITHER diagnostic: the constraint is satisfied and the interior key is
// equality-bearing. Confirms the fix does not introduce false positives.
BEZEL_TEST(hashable_interior_double_diag_2111,
           constrained_interior_dictionary_hashable_arg_no_diag_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(X: int, Y: int); "
        "public class Registry<K> where K: hashable "
        "  public method Put(d: Dictionary<K, int>) end Put; "
        "end Registry; "
        "class C public static method Main(): int "
        "  using var r := new Registry<Point>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasHashableConstraintError(r));
    BEZEL_EXPECT_FALSE(hasInteriorHashKeyError(r));
}

// ─── SOUNDNESS GUARD: an UNCONSTRAINED interior use still fires ──────────────

// §14.2: "an unconstrained parameter never acquires [the property]." A generic
// `Registry2<K>` with NO `where K: hashable` clause carries its own interior
// §3.6-rule-2 obligation for `Dictionary<K, int>`; with a non-equality-bearing
// `Holder` it MUST still fire the interior diagnostic. The fix suppresses the
// interior check ONLY when a `hashable` constraint is present on that exact
// param, so this case is unaffected — silently dropping it would be a
// soundness regression. (Mirrors `bare_t_unconstrained_interior_hash_key_
// rejected_spec_14_2` in hashable_constraint_2087_test.cpp; duplicated here as
// a focused guard over the #2111 suppression's boundary.)
BEZEL_TEST(hashable_interior_double_diag_2111,
           unconstrained_interior_dictionary_still_rejected_spec_14_2) {
    auto r = parseSemaAll(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "public class Registry2<K> "
        "  public method Put(d: Dictionary<K, int>) end Put; "
        "end Registry2; "
        "class C public static method Main(): int "
        "  using var reg := new Registry2<Holder>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasInteriorHashKeyError(r));
    // No §14.2 `hashable` constraint exists to subsume it.
    BEZEL_EXPECT_FALSE(hasHashableConstraintError(r));
}
