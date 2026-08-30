// Spec-derived regression test for issue #3624 (§12.8 / §22.7 / §22.10,
// v0.286): "cleanup: two hand-written copies of the §12.8 chain-call
// recognizer remain beside `ast::chainCallOfStmt`".
//
// `ast::chainCallOfStmt` (src/ast/Node.hpp) is the ONE recognizer for "is this
// statement a `base(...)`/`this(...)` constructor chain call (§12.8), and
// which one is it?". Before this fix, `Resolver::isChainCall`
// (TypeResolution_Checks.cpp) and `MethodDA::getChainingCall`
// (DefiniteAssignment.cpp) carried their own copies of the same six-line
// `dynamic_cast` walk; `getChainingCall`'s two callers additionally
// re-derived the this/base discrimination with a THIRD `dynamic_cast` on the
// callee a few lines later.
//
// VERIFIED BY READING (not merely on the issue's say-so, per the lane trap):
// all three walks perform the identical sequence of dynamic_casts
// (ExpressionStmt -> CallExpr -> non-null callee -> IdentifierExpr) and the
// identical two name comparisons ("this", "base"), over the identical AST
// node in every call (`Resolver::isChainCall`'s callers always pass an
// already-null-checked `Stmt*`; `getChainingCall`'s single-argument form
// always derived `cd.body.front()`, which is exactly the Stmt this fix's
// wrapper passes to `chainCallOfStmt` after the same guard). No shape is
// admitted or excluded differently in either direction — the unification is
// genuinely behaviour-preserving, so these tests do not (and, on the
// evidence above, structurally cannot) distinguish old code from new code.
// What they DO pin down is that the shared authority produces the correct
// answer at *both* former call sites, and that the this/base discrimination
// it threads through actually drives the downstream decision at each site
// (rather than, say, a broken forward that always returns `isThis = false`).
//
// PROOF THAT THE TEST CAN FAIL (mutation, since old-vs-new cannot produce a
// difference by construction — see above): with `chainCallOfStmt`'s two
// branches swapped (`isThis = false` on "this", `isThis = true` on "base",
// Node.hpp:1466-1476) and `bezelc` rebuilt, `resolver_this_chain_...` and
// `da_this_chain_...` below FAIL (the this-chain is misread as a base-chain)
// and `resolver_base_chain_...` / `da_base_chain_...` FAIL the other way
// (the base-chain is misread as this-chain and the required-`this(...)`
// diagnostic disappears) — 4 of 4 new rows flip. Restoring the correct
// branches (git checkout -- src/ast/Node.hpp) and rebuilding restores all 4
// to PASS. See the commit body for the exact counts.
//
// The two former call sites, both exercised in one `parseAndCheckDA` pass
// (it runs resolveTypes, which calls Resolver::checkConstructorChains, then
// checkDefiniteAssignment):
//   - the RESOLVER path: §22.10's convenience-constructor chaining rule
//     (`Resolver::checkTypeConstructorChains` -> `isChainCall`).
//   - the DEFINITE-ASSIGNMENT path: §22.7 rule 1's chaining-position gate,
//     which also uses the this/base discrimination to decide whether the
//     chained-to constructor's non-nullable-field obligation has already
//     been discharged (`MethodDA::runConstructor` -> `getChainingCall`).

#include <string>

#include "support/da_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckDA;

namespace {

bool daHasError(const bezel::test::DAResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

constexpr const char* kMustChainDiag =
    "must chain to another constructor via `this(...)`";
constexpr const char* kOnlyPrimaryBaseDiag =
    "only the primary constructor may chain to `base(...)`";
constexpr const char* kBeforeAssignedDiag = "before non-nullable field";

}  // namespace

// ---------------------------------------------------------------------------
// RESOLVER path (§22.10, via `Resolver::isChainCall`).
// ---------------------------------------------------------------------------

// ACCEPT: a convenience constructor of a primary-constructor class chaining
// via `this(...)` satisfies §22.10 -- the shared recognizer must report
// isThis=true for a `this(...)` chain so the resolver does not also flag it
// as needing a `this(...)` chain it already has.
BEZEL_TEST(chain_call_recognizer_3624,
           resolver_this_chain_accepted_spec_22_10) {
    auto r = parseAndCheckDA(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int) "
        "    this(v, \"conv\"); "
        "  end constructor; "
        "end B;");
    BEZEL_EXPECT_FALSE(daHasError(r, kMustChainDiag));
}

// REJECT: a convenience constructor chaining via `base(...)` instead violates
// §22.10 -- the shared recognizer must report isThis=false for a `base(...)`
// chain so the resolver's "only the primary may chain to base(...)" clause
// fires (not the plain "must chain via this(...)" clause alone, which also
// fires for a NON-chaining constructor -- distinguishing the two clauses is
// exactly what a this/base mix-up in the shared recognizer would break).
BEZEL_TEST(chain_call_recognizer_3624,
           resolver_base_chain_rejected_spec_22_10) {
    auto r = parseAndCheckDA(
        "module M; "
        "class A(x: int) end A; "
        "class B(n: int): A(n) "
        "  public constructor(v: int, w: int) "
        "    base(v); "
        "  end constructor; "
        "end B;");
    BEZEL_EXPECT_TRUE(daHasError(r, kMustChainDiag));
    BEZEL_EXPECT_TRUE(daHasError(r, kOnlyPrimaryBaseDiag));
}

// ---------------------------------------------------------------------------
// DEFINITE-ASSIGNMENT path (§22.7 rule 1, via `MethodDA::getChainingCall`).
// ---------------------------------------------------------------------------

// ACCEPT: after a `this(...)` chain, the chained-to constructor has already
// discharged every §7.9-strict field, so a body observation of `this` is
// admitted on entry. This requires the shared recognizer's isThis=true to
// reach `runConstructor`'s "seed fieldsNeedingInit_ as assigned" branch --
// exactly the branch the old code reached via its OWN second dynamic_cast on
// `chain->callee`, now driven by the value `chainCallOfStmt` already computed.
BEZEL_TEST(chain_call_recognizer_3624,
           da_this_chain_seeds_observation_accepted_spec_22_7) {
    auto r = parseAndCheckDA(
        "module M; "
        "class W "
        "  private _name: string; "
        "  public constructor(n: string) "
        "    _name := n; "
        "  end constructor; "
        "  public constructor() "
        "    this(\"d\"); "
        "    Describe(); "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// REJECT: after a `base(...)` chain, the DECLARING class's own §7.9-strict
// fields are still unassigned (only the base's fields were discharged), so a
// body observation of `this` before `_name` is assigned is still an error.
// This is the isThis=false twin of the previous test -- if the shared
// recognizer's this/base discrimination were flipped or ignored, this row
// would wrongly compile clean (a silent widening of §22.7's admitted set).
BEZEL_TEST(chain_call_recognizer_3624,
           da_base_chain_does_not_seed_observation_rejected_spec_22_7) {
    auto r = parseAndCheckDA(
        "module M; "
        "class A(x: int) end A; "
        "class Derived: A "
        "  private _name: string; "
        "  public constructor(x: int, n: string) "
        "    base(x); "
        "    Describe(); "
        "    _name := n; "
        "  end constructor; "
        "  public method Describe() end Describe; "
        "end Derived;");
    BEZEL_EXPECT_TRUE(daHasError(r, kBeforeAssignedDiag));
}
