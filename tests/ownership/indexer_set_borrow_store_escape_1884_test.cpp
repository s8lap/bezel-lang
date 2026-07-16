// Spec-derived tests for issue #1884 — §28.11 "Storing a borrow" via the
// indexer-set sink (docs v0.196).
//
// §28.11 lists the store operations of a BORROWING collection together —
// "store (`Add`, `Insert`, indexer-set)" — and the v0.180 store rule mandates:
// "a borrow of a scope-bound local stored into a collection that provably
// outlives that scope — the collection is a field, is reachable from one,
// escapes the method, or is a local of strictly wider scope — is rejected at
// the store site." The compiler rejected the `.Add`/`.Insert` method-call
// sinks but NOT the `coll[i] := local` indexer-set assignment sink, so a
// borrow of a scope-bound owned local stored through it into an escaping
// collection compiled clean and dangled (heap-use-after-free: the local is
// disposed at scope end while `coll[i]` still aliases it). This closes that
// hole: checkIndexerSetBorrowStoreEscape classifies the IndexExpr assignment
// target exactly as checkCollectionBorrowStoreEscape classifies a storing-
// method receiver.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// §28.11 indexer-set borrow-store escape diagnostic: cites §28.11, names the
// "would outlive its source" store-rule text, and both the collection and the
// borrow's source.
bool rejectsIndexerSetStoreEscape(const OwnershipResult& r,
                                  std::string_view recvName,
                                  std::string_view srcName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.11") == std::string::npos) continue;
        if (d.message.find("would outlive its source") == std::string::npos)
            continue;
        if (d.message.find("indexer-set") == std::string::npos) continue;
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
// NEGATIVE — a scope-bound local's borrow stored through indexer-set into a
// longer-lived borrowing collection is rejected.
// =============================================================================

// The exact shape the issue names: `_arr[0] := r` where `_arr` is a field
// (escapes the method) and `r` is a scope-bound owned local disposed at the
// end of `Stash()`. Before the fix this compiled clean and `_arr[0]` dangled.
BEZEL_TEST(indexer_set_store_escape_1884, field_array_indexer_set_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public V: int := 7; "
        "  public method Ping(): int => V; "
        "  destructor end destructor; end Res; "
        "class Box "
        "  public owned _arr: Array<Res>; "
        "  public constructor() _arr := new Array<Res>(); end constructor; "
        "  public method Seed(seed: Res) _arr.Add(seed); end Seed; "
        "  public method Stash() "
        "    var r := new Res(); "
        "    _arr[0] := r; "
        "  end Stash; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsIndexerSetStoreEscape(r, "_arr", "r"));
}

// The Dictionary indexer-set variant `_d[k] := r` — the issue notes it is
// identical. Same field-held escaping collection, same scope-bound source.
BEZEL_TEST(indexer_set_store_escape_1884, field_dictionary_indexer_set_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class Box "
        "  public owned _d: Dictionary<int, Res>; "
        "  public constructor() _d := new Dictionary<int, Res>(); end constructor; "
        "  public method Stash() "
        "    var r := new Res(); "
        "    _d[0] := r; "
        "  end Stash; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsIndexerSetStoreEscape(r, "_d", "r"));
}

// The collection is an OUTER local (strictly wider scope than the source):
// `arr[0] := cur` where `arr` is the method scope and `cur` is loop-body
// scoped. The collection outlives each iteration's `cur`, so the store escapes.
BEZEL_TEST(indexer_set_store_escape_1884, outer_local_collection_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Main(seed: Res): int "
        "  var arr := new Array<Res>(); "
        "  arr.Add(seed); "
        "  var i := 0; "
        "  while i < 3 do "
        "    var cur := new Res(); "
        "    arr[0] := cur; "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsIndexerSetStoreEscape(r, "arr", "cur"));
}

// =============================================================================
// POSITIVE — no over-rejection: the indexer-set is admitted when the source
// provably outlives the collection (structural residual-trust admissions).
// =============================================================================

// A PARAMETER-sourced borrow stored through indexer-set into a field array —
// the parameter's source outlives this method (§28.11: parameter-sourced
// borrows are admitted).
BEZEL_TEST(indexer_set_store_escape_1884, parameter_sourced_element_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class Box "
        "  public owned _arr: Array<Res>; "
        "  public constructor(seed: Res) "
        "    _arr := new Array<Res>(); _arr.Add(seed); end constructor; "
        "  public method Replace(item: Res) _arr[0] := item; end Replace; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A FIELD-sourced borrow stored through indexer-set into a field array — the
// field's referent outlives this method (§28.11: field-sourced borrows are
// admitted).
BEZEL_TEST(indexer_set_store_escape_1884, field_sourced_element_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class Box "
        "  public owned _arr: Array<Res>; "
        "  public _peer: Res; "
        "  public constructor(p: Res) "
        "    _arr := new Array<Res>(); _peer := p; _arr.Add(p); end constructor; "
        "  public method Refresh() _arr[0] := _peer; end Refresh; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Same-scope store: the collection and the source local are declared at the
// same depth — the collection does not PROVABLY outlive the source, so the
// structural check admits it (residual trust, per §28.11).
BEZEL_TEST(indexer_set_store_escape_1884, same_scope_local_collection_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Main(seed: Res): int "
        "  var arr := new Array<Res>(); "
        "  arr.Add(seed); "
        "  var cur := seed; "
        "  arr[0] := cur; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An OWNING-collection indexer set `coll[i] := move item` takes an OWNED value
// (§28.11 owning-slot policy) — an ownership transfer, not a borrow store — so
// this borrow-store rule does not apply and the transfer stays accepted.
BEZEL_TEST(indexer_set_store_escape_1884, owning_collection_indexer_set_unaffected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public constructor() end constructor; "
        "  destructor end destructor; end Res; "
        "class C public static method Main(): int "
        "  using var pool := new OwningArray<Res>(); "
        "  pool.Add(new Res()); "
        "  pool[0] := new Res(); "
        "  return pool.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
