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
