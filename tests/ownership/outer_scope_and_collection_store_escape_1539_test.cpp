// Spec-derived tests for issue #1539 — §28.3 outer-scope borrow escape and
// §28.11 borrowing-collection store checks (docs v0.180).
//
// (a) §28.3 "Borrow lifetime rules": "A borrow may not be assigned to a
// local whose scope strictly encloses the borrow's source's scope (v0.180)
// — including flowing into a loop-carried binding along a back edge when
// the source is declared inside the loop body (`prev := cur;` where `cur`
// is per-iteration). The scope comparison is structural and decidable; the
// diagnostic is the standard 'Borrow held by `x` would outlive its source
// `y`.'" Before this fix, checkBorrowStoreEscape only recognized this
// OuterLocal escape when the RHS was a CLOSURE capturing the scope-bound
// source (#861); a PLAIN borrow read flowing straight into an outer local —
// including the named loop-carried back-edge shape — compiled and dangled
// (probe own-st/p7).
//
// (b) §28.11 "Storing a borrow" (v0.180): storing a class-typed borrow into
// a BORROWING collection (Array<T>, Dictionary<K,V>, Set<T>, Stack<T>,
// Queue<T>, LinkedList<T>) "follows the same shape as §28.3's borrowed-field
// rule": parameter-, receiver-, and field-SOURCED borrows are admitted;
// "a borrow of a scope-bound local stored into a collection that provably
// outlives that scope — the collection is a field, is reachable from one,
// escapes the method, or is a local of strictly wider scope — is rejected
// at the store site." No prior check recognized this store site at all
// (probe own-st/p4).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::OwnershipResult;

namespace {

// A disposable resource class with no state — its destructor makes any
// local binding of it an owned scope-bound local (§28.3).
std::string wrap(const std::string& mainBody) {
    return std::string(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Main(): int ") + mainBody +
        " end Main; end C;";
}

// §28.3 (v0.180, #1539): the general outer-local escape diagnostic — leads
// with the spec's own recommended text ("borrow held by `x` would outlive
// its source `y`") and cites §28.3.
bool rejectsOuterLocalBorrowEscape(const OwnershipResult& r,
                                   std::string_view targetName,
                                   std::string_view srcName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.3") == std::string::npos) continue;
        if (d.message.find("would outlive its source") == std::string::npos)
            continue;
        if (d.message.find(std::string(targetName)) == std::string::npos)
            continue;
        if (d.message.find(std::string(srcName)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

// §28.11 (v0.180, #1539): the borrowing-collection store escape diagnostic.
bool rejectsCollectionBorrowStoreEscape(const OwnershipResult& r,
                                        std::string_view recvName,
                                        std::string_view srcName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.11") == std::string::npos) continue;
        if (d.message.find("would outlive its source") == std::string::npos)
            continue;
        if (d.message.find(std::string(recvName)) == std::string::npos)
            continue;
        if (d.message.find(std::string(srcName)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// (a) NEGATIVE — a plain borrow assigned to an outer-scope local escapes.
// =============================================================================

// The exact loop-carried back-edge shape the spec names by example:
// `prev := cur;` where `cur` is declared inside the loop body and `prev`
// outside it. `cur` dies at the end of each iteration; `prev` survives to
// the next iteration (and past the loop), so the borrow dangles. This is
// the exact own-st/p7 repro that compiled (and dangled) before the fix.
BEZEL_TEST(outer_scope_escape_1539, loop_back_edge_prev_cur_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var prev: Res? := null; "
        "var i := 0; "
        "while i < 3 do "
        "  using var cur := new Res(); "
        "  prev := cur; "
        "  i := i + 1; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsOuterLocalBorrowEscape(r, "prev", "cur"));
}

// The same rule without a loop: `cur` is scope-bound to the if-branch; `b`
// is declared in the enclosing method scope, which outlives the branch.
BEZEL_TEST(outer_scope_escape_1539, if_branch_local_to_outer_local_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var b: Res? := null; "
        "if true then "
        "  using var cur := new Res(); "
        "  b := cur; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsOuterLocalBorrowEscape(r, "b", "cur"));
}

// A plain (non-`using`) owned local is scope-bound the same way — auto-drop
// (§28.3) disposes it at the end of its declaring block if not moved out
// first, so a bare re-borrow of it escaping to an outer local is the same
// hazard as the `using var` case above.
BEZEL_TEST(outer_scope_escape_1539, plain_owned_local_source_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var prev: Res? := null; "
        "var i := 0; "
        "while i < 3 do "
        "  var cur := new Res(); "
        "  prev := cur; "
        "  i := i + 1; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsOuterLocalBorrowEscape(r, "prev", "cur"));
}

// =============================================================================
// (a) POSITIVE — no over-rejection.
// =============================================================================

// Same-block assignment: `alias` and `cur` are declared at the same depth —
// `alias`'s scope does not strictly enclose `cur`'s. Accepted.
BEZEL_TEST(outer_scope_escape_1539, same_scope_alias_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "using var cur := new Res(); "
        "var alias := cur; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Inner-block target: `inner` is declared INSIDE the if, `src` outside it.
// `inner` dies no later than `src` — not an escape.
BEZEL_TEST(outer_scope_escape_1539, inner_block_target_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "using var src := new Res(); "
        "if true then "
        "  var inner := src; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Equalizing to a parameter-sourced borrow across the loop (§28.3 Pattern 1
// shape): `prev` is assigned a borrow of `outerRes` (a parameter, which
// outlives the loop), never of the loop-body-scoped `cur`. Accepted.
BEZEL_TEST(outer_scope_escape_1539, loop_carried_param_sourced_borrow_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Main(outerRes: Res): int "
        "  var prev: Res? := null; "
        "  var i := 0; "
        "  while i < 3 do "
        "    using var cur := new Res(); "
        "    prev := outerRes; "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// (b) NEGATIVE — a scope-bound local's borrow stored into a longer-lived
// borrowing collection is rejected.
// =============================================================================

// The exact shape the issue names: a `using var` Add-ed into a collection
// declared OUTSIDE the loop body that scopes the `using var`.
BEZEL_TEST(collection_store_escape_1539, loop_local_into_outer_collection_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Main(count: int): int "
        "  using var pool := new Array<Res>(); "
        "  var i := 0; "
        "  while i < count do "
        "    using var cur := new Res(); "
        "    pool.Add(cur); "
        "    i := i + 1; "
        "  end; "
        "  return pool.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionBorrowStoreEscape(r, "pool", "cur"));
}

// The collection is a FIELD of the class — a field always outlives a
// method-scope local.
BEZEL_TEST(collection_store_escape_1539, field_collection_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C "
        "  private owned _pool: Array<Res>; "
        "  public constructor() _pool := new Array<Res>(); end constructor; "
        "  public method Fill() "
        "    using var cur := new Res(); "
        "    _pool.Add(cur); "
        "  end Fill; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionBorrowStoreEscape(r, "_pool", "cur"));
}

// The collection is a PARAMETER — caller-supplied storage that outlives
// this method call.
BEZEL_TEST(collection_store_escape_1539, parameter_collection_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Fill(pool: Array<Res>) "
        "  using var cur := new Res(); "
        "  pool.Add(cur); "
        "end Fill; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionBorrowStoreEscape(r, "pool", "cur"));
}

// =============================================================================
// (b) POSITIVE — no over-rejection.
// =============================================================================

// Same-block store: `pool` and `cur` are declared at the same depth — the
// collection does not PROVABLY outlive `cur`'s scope, so the structural
// check admits it (residual trust, per §28.11).
BEZEL_TEST(collection_store_escape_1539, same_scope_store_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "using var pool := new Array<Res>(); "
        "using var cur := new Res(); "
        "pool.Add(cur); "
        "return pool.Count; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The collection is declared in a NARROWER scope than the source — it dies
// no later than the source, so storing into it is not an escape.
BEZEL_TEST(collection_store_escape_1539, inner_scope_collection_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "using var cur := new Res(); "
        "if true then "
        "  using var pool := new Array<Res>(); "
        "  pool.Add(cur); "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A PARAMETER-sourced borrow stored into a method-scope collection is
// admitted — the parameter's source outlives this method (§28.11:
// parameter-sourced borrows are admitted).
BEZEL_TEST(collection_store_escape_1539, parameter_sourced_element_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Fill(item: Res): int "
        "  using var pool := new Array<Res>(); "
        "  pool.Add(item); "
        "  return pool.Count; "
        "end Fill; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An OWNING collection's `Add(owned item)` is governed by the ownership-
// transfer rules (§28.8), not this borrow-store rule — a fresh producer
// argument stays accepted regardless of the collection's own scope.
BEZEL_TEST(collection_store_escape_1539, owning_collection_add_unaffected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Fill(): int "
        "  using var pool := new OwningArray<Res>(); "
        "  pool.Add(new Res()); "
        "  return pool.Count; "
        "end Fill; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// (c) issue #2656 — the TRIVIAL-DISPOSE-class variant of the same §28.11 rule.
//
// Every fixture above binds `Res`, a DESTRUCTOR-BEARING class, so strictly
// they pin only the non-trivial case. The rule itself is about BORROW
// LIFETIME, not about disposal work, so it should not care whether the stored
// element's class is trivial-dispose (§28.9) — but nothing pinned that, and
// the distinction is live in the implementation.
//
// ⚠ THESE PIN TODAY'S BEHAVIOUR DELIBERATELY. Read before changing them.
//
// The rejection below is CURRENT behaviour, verified on this revision. It is
// load-bearing for proposal 0077's silent-tracking reading (docs/proposals/
// 0077-silent-tracking.md, tracking issue #2653): a scope-local trivial
// instance is still TRACKED for borrow lifetime even though its disposal is a
// no-op. A silent regression toward ADMITTING these stores would change the
// language's de-facto semantics unobserved — which is exactly why the pin
// exists rather than being left to the destructor-bearing fixtures above.
//
// The implementation has NOT decided the general question. The §28.3/§28.11
// suspend-local carve-out in src/ownership/Ownership.cpp records that whether
// §28.9's "implementation-defined reclaim" carries zero leak risk for an
// arbitrary trivial-dispose class "is a separate, NOT-YET-DECIDED design
// question this fix declines to resolve silently (tracked #2332)".
//
// What would justify changing these tests: a ruling on #2332, or a proposal
// 0077 (#2653) graduation that explicitly relaxes §28.11 for trivial-dispose
// elements — with the spec amended first. NOT an implementation convenience,
// and not a drive-by "trivial classes need no tracking" simplification. If a
// future change makes these fail, that is the pin working; resolve it against
// the spec revision in force, not by deleting the assertion.
//
// §28.11 "Storing a borrow" (v0.180, quoted in full at the head of this file)
// draws its line at the SCOPE of the collection versus the scope of the
// borrow's source, with no trivial-dispose qualification anywhere in the rule.
// §28.9's trivial-dispose section likewise grants only weaker DISCHARGE
// requirements — never weaker BORROW-LIFETIME requirements.
// =============================================================================

// A trivial-dispose class (§28.9): no destructor, no `owned` field, no
// owning-inline-array field, no base. Its `Dispose()` is provably a no-op.
constexpr const char* kTrivialBox =
    "class Box public constructor() end constructor; "
    "  public Value: int := 1; end Box; ";

// The probe-C shape named by issue #2656: a scope-local trivial `Box` `Add`ed
// into an `Array<Box>` that lives in an ENCLOSING scope. Rejected today, with
// the same §28.11 diagnostic the destructor-bearing twin
// (`loop_local_into_outer_collection_rejected`, above) draws — the element
// class's trivial-dispose status does not enter the rule.
BEZEL_TEST(collection_store_escape_2656, trivial_class_loop_local_into_outer_collection_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivialBox +
        "class C public static method Main(count: int): int "
        "  var pool := new Array<Box>(); "
        "  var i := 0; "
        "  while i < count do "
        "    var b := new Box(); "
        "    pool.Add(b); "
        "    i := i + 1; "
        "  end; "
        "  return pool.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionBorrowStoreEscape(r, "pool", "b"));
}

// The same shape with the element bound by `using var`. §28.9: "`using var` on
// a trivial-dispose class is permitted and has no observable effect other than
// scope limitation" — so this is the explicitly-scope-limited spelling of the
// test above, and it must be rejected on the same footing.
BEZEL_TEST(collection_store_escape_2656, trivial_class_using_var_loop_local_into_outer_collection_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivialBox +
        "class C public static method Main(count: int): int "
        "  using var pool := new Array<Box>(); "
        "  var i := 0; "
        "  while i < count do "
        "    using var b := new Box(); "
        "    pool.Add(b); "
        "    i := i + 1; "
        "  end; "
        "  return pool.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionBorrowStoreEscape(r, "pool", "b"));
}

// The collection is a FIELD — it always outlives a method-scope local. The
// trivial twin of `field_collection_rejected` above.
BEZEL_TEST(collection_store_escape_2656, trivial_class_field_collection_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivialBox +
        "class C "
        "  private owned _pool: Array<Box>; "
        "  public constructor() _pool := new Array<Box>(); end constructor; "
        "  public method Fill() "
        "    var b := new Box(); "
        "    _pool.Add(b); "
        "  end Fill; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCollectionBorrowStoreEscape(r, "_pool", "b"));
}

// NO OVER-REJECTION control. `pool` and `b` are declared at the same depth, so
// the collection does not PROVABLY outlive `b`'s scope and the structural
// check admits the store (residual trust, per §28.11) — exactly as it does for
// the destructor-bearing `same_scope_store_accepted` above. Without this, the
// three rejections could be satisfied by a blanket "reject every trivial-class
// collection store", which is not the rule.
BEZEL_TEST(collection_store_escape_2656, trivial_class_same_scope_store_accepted) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivialBox +
        "class C public static method Main(): int "
        "  var pool := new Array<Box>(); "
        "  var b := new Box(); "
        "  pool.Add(b); "
        "  return pool.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NO OVER-REJECTION control 2. A PARAMETER-sourced trivial element stored into
// a method-scope collection is admitted (§28.11: parameter-sourced borrows are
// admitted) — the trivial twin of `parameter_sourced_element_accepted`.
BEZEL_TEST(collection_store_escape_2656, trivial_class_parameter_sourced_element_accepted) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivialBox +
        "class C public static method Fill(item: Box): int "
        "  var pool := new Array<Box>(); "
        "  pool.Add(item); "
        "  return pool.Count; "
        "end Fill; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
