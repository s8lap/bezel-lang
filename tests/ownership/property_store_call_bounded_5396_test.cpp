// Spec-derived regression test for #5396 (§28.3, v0.314 / #5303):
// a capture-bearing function value assigned to a BARE auto-property identifier
// OR a LOCAL INDEXER was not rejected. The §28.14 sink check keyed on
// FieldSymbol, so a bare auto-property identifier (which resolves to
// PropertySymbol, not FieldSymbol) slipped past. The same gap existed for
// a local indexer store.
//
// Fix: adds PropertySymbol to the fieldTarget check in checkBorrowStoreEscape
// and to the Dest::Field classification in checkRefOutLambdaCaptureOnStore.
// Also adds the IndexExpr case for local indexer stores in both functions.
//
// The v0.314 ruling: "a function value assigned to a property or indexer IS a
// store" — a capture-bearing function value stored to a property or indexer is
// rejected at the assignment site.
//
// §28.3 / §28.14 (v0.314)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Program that stores a call-bounded callback parameter to a bare auto-prop.
// The bare identifier `Prop` resolves to PropertySymbol (not FieldSymbol),
// which the old code missed.
const char* kCallBoundedStore = R"(
module M;
class C
  public property Prop: () -> ();
  public method Store(cb: () -> ())
    Prop := cb;
  end method;
end C;
)";

// Program that stores a call-bounded callback through a LOCAL INDEXER
// (items[0] := cb where items is a local variable of an indexer type).
// The indexer's set accessor may capture the source into escaping storage,
// per #5396 / #5303, §28.3, v0.314.
const char* kIndexerStore = R"(
module M;
class MyList
  public property [i: int]: () -> ()
    get begin end get;
    set begin end set;
  end;
end MyList;
class C
  public method Store(cb: () -> ())
    var items: MyList := new MyList();
    items[0] := cb;
  end method;
end C;
)";

// Program that stores a call-bounded alias to a bare auto-prop.
// The §28.14 aliasing rule makes `alias` a call-bounded source.
const char* kAliasStore = R"(
module M;
class C
  public property Prop: () -> ();
  public method Store(cb: () -> ())
    var alias := cb;
    Prop := alias;
  end method;
end C;
)";

} // namespace

// §28.3 / §28.14 / #5396 (v0.314): a call-bounded callback parameter stored
// into a BARE auto-property identifier is a sink. Rejected with §28.14 diagnostic.
BEZEL_TEST(property_store_call_bounded_5396,
           bare_autoprop_identifier_call_bounded_stored_rejected) {
    auto r = parseAndCheckOwnership(kCallBoundedStore);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(r, "call-bounded"));
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(r, "stored"));
}

// §28.3 / §28.14 / #5396 (v0.314): a call-bounded callback stored through
// a LOCAL INDEXER (items[0] := cb) is a sink — the indexer's set accessor
// may capture the source into escaping storage. This was missed because the
// existing IndexExpr arm only checked collection reachability, not whether
// the indexer's set accessor itself is a capturing sink.
BEZEL_TEST(property_store_call_bounded_5396,
           local_indexer_call_bounded_stored_rejected) {
    auto r = parseAndCheckOwnership(kIndexerStore);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(r, "call-bounded"));
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(r, "stored"));
}

// §28.3 / §28.14 / #5396 (v0.314): a call-bounded alias (a local aliased
// from a call-bounded parameter, §28.14 aliasing rule) stored to a bare
// auto-property identifier is also a sink.
BEZEL_TEST(property_store_call_bounded_5396,
           bare_autoprop_alias_to_call_bounded_stored_rejected) {
    auto r = parseAndCheckOwnership(kAliasStore);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(r, "call-bounded"));
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(r, "stored"));
}
