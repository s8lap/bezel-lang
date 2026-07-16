// Spec-derived tests for issue #1886 — §28.11 "Storing a borrow" via the bulk
// AddRange sink (docs v0.196).
//
// §28.11's store rule governs every store into a BORROWING collection; the
// v0.180 rule rejects "a borrow of a scope-bound local stored into a collection
// that provably outlives that scope." `Array<T>.AddRange(items: IEnumerable<T>)`
// (stdlib §4) is a BULK store — it copies every element borrow of `items` into
// the receiver — so copying a scope-bound owned local collection's element
// borrows into an escaping collection is the same escape as `.Add`-ing one of
// them. `AddRange` was absent from the storing-method whitelist in
// checkCollectionBorrowStoreEscape, so the store compiled clean and dangled
// (heap-use-after-free: the source local is disposed at scope end while the
// receiver still aliases its copied elements). This closes that hole, with an
// element-type gate so a value-typed element collection (no borrow to escape)
// is not over-rejected.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// §28.11 AddRange borrow-store escape diagnostic: cites §28.11, the
// "would outlive its source" store-rule text, the AddRange sink, and both the
// receiver and the source collection.
bool rejectsAddRangeStoreEscape(const OwnershipResult& r,
                                std::string_view recvName,
                                std::string_view srcName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.11") == std::string::npos) continue;
        if (d.message.find("would outlive its source") == std::string::npos)
            continue;
        if (d.message.find("AddRange") == std::string::npos) continue;
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
// NEGATIVE — a scope-bound local collection AddRange-copied into a longer-lived
// borrowing collection is rejected.
// =============================================================================

// The exact shape the issue names: a local `Array<Res>` holding a borrow of a
// scope-bound owned local `r` is AddRange-copied into the field array `_all`.
// The field outlives the method, so the copied borrow of `r` dangles once `r`
// is disposed. Before the fix this compiled clean.
BEZEL_TEST(addrange_store_escape_1886, field_collection_addrange_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public V: int := 6; "
        "  public method Ping(): int => V; "
        "  destructor end destructor; end Res; "
        "class Box "
        "  public owned _all: Array<Res>; "
        "  public constructor() _all := new Array<Res>(); end constructor; "
        "  public method Stash() "
        "    var r := new Res(); "
        "    var local := new Array<Res>(); "
        "    local.Add(r); "
        "    _all.AddRange(local); "
        "  end Stash; "
        "  public method Peek(): int => _all[0].Ping(); "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsAddRangeStoreEscape(r, "_all", "local"));
}

// The receiver is an OUTER local (strictly wider scope than the source
// collection): `outer.AddRange(inner)` where `inner` is loop-body scoped and
// `outer` is the method scope. `outer` outlives each iteration's `inner`.
BEZEL_TEST(addrange_store_escape_1886, outer_local_receiver_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Main(seed: Res): int "
        "  var outer := new Array<Res>(); "
        "  var i := 0; "
        "  while i < 3 do "
        "    var inner := new Array<Res>(); "
        "    inner.Add(seed); "
        "    outer.AddRange(inner); "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsAddRangeStoreEscape(r, "outer", "inner"));
}

// =============================================================================
// POSITIVE — no over-rejection.
// =============================================================================

// AddRange of a PARAMETER collection: the parameter's source outlives this
// method (§28.11: parameter-sourced borrows are admitted), so copying its
// elements into a field array is admitted (residual trust).
BEZEL_TEST(addrange_store_escape_1886, parameter_collection_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class Box "
        "  public owned _all: Array<Res>; "
        "  public constructor() _all := new Array<Res>(); end constructor; "
        "  public method Merge(items: Array<Res>) _all.AddRange(items); end Merge; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// AddRange of an `owned` PARAMETER collection: an owned parameter is seeded Owned
// in the ownership lattice (like an owned local), but its source is the caller,
// which outlives this method — so copying its elements into a field array is
// admitted exactly as the plain-parameter case is (§28.11: parameter-sourced
// borrows are admitted). The store-escape detection must exclude PARAMETERS
// (LocalSymbol only), matching ownedLocalBorrowSource's element-store exclusion;
// keying off `isOwnedTrackedLocal` alone would wrongly reject this — an owned
// param is Owned-tracked but is not a scope-bound local.
BEZEL_TEST(addrange_store_escape_1886, owned_parameter_collection_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class Box "
        "  public owned _all: Array<Res>; "
        "  public constructor() _all := new Array<Res>(); end constructor; "
        "  public method Merge(owned items: Array<Res>) _all.AddRange(items); end Merge; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// AddRange of a VALUE-typed local collection: the elements are `int` values,
// COPIED into the receiver — no borrow escapes. The element-type gate admits
// it; a naive whitelist entry would have over-rejected this legal pattern.
BEZEL_TEST(addrange_store_escape_1886, value_element_local_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public owned _ints: Array<int>; "
        "  public constructor() _ints := new Array<int>(); end constructor; "
        "  public method Fill() "
        "    var tmp := new Array<int>(); "
        "    tmp.Add(1); "
        "    tmp.Add(2); "
        "    _ints.AddRange(tmp); "
        "  end Fill; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Same-scope AddRange: receiver and source collection are declared at the same
// depth — the receiver does not PROVABLY outlive the source, so the structural
// check admits it (residual trust, per §28.11).
BEZEL_TEST(addrange_store_escape_1886, same_scope_local_receiver_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Main(seed: Res): int "
        "  var dst := new Array<Res>(); "
        "  var src := new Array<Res>(); "
        "  src.Add(seed); "
        "  dst.AddRange(src); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
