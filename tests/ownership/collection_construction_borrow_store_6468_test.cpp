// Spec-derived tests for issue #6468 — §28.11's borrow-source check over
// COLLECTION CONSTRUCTION, and §9.7's collection bullet (docs v0.338).
//
// §28.11 *Value-copy members* closes with: "An inline array's `ToArray`
// (§15.1), range slicing (§15.6) and a direct `FromCollectionLiteral` call
// are shape (i)'s copies; a collection literal is judged element by element
// (§15.4)." Each is judged "as a store of each placed value into the
// destination", and "The destination is where the result lands … or the field,
// collection or return slot it is placed in".
//
// The two channels that DID enforce the store rule — `.Add`/`Insert`/
// indexer-set (checkCollectionBorrowStoreEscape) and a direct assignment
// (checkBorrowStoreEscape) — both read the stored value's own TYPE. A shape
// (i) COPY has no borrow-bearing type of its own (its result is `Array<T>`),
// and a collection literal's type is the collection's, so neither reached any
// construction form: `_keep := [it]` and `_keep := xs.ToArray()` both compiled
// clean and left a field holding a borrow of a scope-bound `using var` that is
// disposed at the end of the method.
//
// Two regimes, and the difference is the spec's own:
//
//   - A CLASS / INTERFACE element type is §28.11's store rule WITH its
//     residual: "parameter-sourced … receiver-sourced, and field-sourced
//     borrows are admitted". Only a scope-bound source rejects.
//   - A VALUE TYPE THAT HOLDS BORROWS (a borrow-bearing record, choice or
//     builtin span) is §9.7's collection bullet, which "admits no trust
//     residual for a record-carried borrow" — the destination alone decides.
//
// Every negative row below is a program the spec ADMITS; a fix that flipped
// any of them would be over-rejecting, so they are the blast-radius
// measurement, not decoration.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The §28.11 store-rule diagnostic at a CONSTRUCTION, for a class/interface
// element: cites §28.11, the "would outlive its source" store-rule text, and
// the borrow source by name. Deliberately does NOT match the `.Add(...)`
// wording — the two channels are separate sinks and a row that matched both
// would not distinguish them.
bool rejectsConstructionStoreEscape(const OwnershipResult& r,
                                    std::string_view srcName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.11") == std::string::npos) continue;
        if (d.message.find("would outlive its source") == std::string::npos)
            continue;
        if (d.message.find("at this construction") == std::string::npos) continue;
        if (d.message.find(std::string(srcName)) == std::string::npos) continue;
        return true;
    }
    return false;
}

// The §9.7 collection-bullet diagnostic for a value type that holds borrows.
// §3.6 rule 4 requires a property-dependent diagnostic to NAME the property,
// so the assertion pins that the message names "holds borrows" — a row that
// asserted merely "was rejected" could be satisfied by any diagnostic at all.
bool rejectsHoldsBorrowsConstruction(const OwnershipResult& r,
                                     std::string_view carrier) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§9.7") == std::string::npos) continue;
        if (d.message.find("collection bullet") == std::string::npos) continue;
        if (d.message.find("holds borrows") == std::string::npos) continue;
        if (d.message.find("at this construction") == std::string::npos) continue;
        if (d.message.find(std::string(carrier)) == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — the shape-(i) copy and the element-by-element literal reject.
// =============================================================================

// The collection literal, CLASS element type: `_keep := [it]` where `it` is a
// `using var` of the method's own block and `_keep` is a field of `Box`.
// §15.4 judges the literal element by element; §28.11's store rule rejects a
// scope-bound source into a collection that provably outlives that scope.
BEZEL_TEST(collection_construction_store_6468,
           literal_class_element_into_field_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "class Box "
        "  public owned _keep: Array<Item>; "
        "  public constructor() _keep := new Array<Item>(); end constructor; "
        "  public method Stash() "
        "    using var it := new Item(); "
        "    _keep := [it]; "
        "  end Stash; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsConstructionStoreEscape(r, "it"));
}

// The shape-(i) copy, CLASS element type: `xs.ToArray()` over a local whose
// element is a borrow of the scope-bound `it`. §28.11 names `ToArray` (§15.1)
// as shape (i)'s copy and sources the copied values from the receiver, so the
// copy carries `it`'s borrow into the field.
BEZEL_TEST(collection_construction_store_6468,
           toarray_class_element_into_field_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "class Box "
        "  public owned _keep: Array<Item>; "
        "  public constructor() _keep := new Array<Item>(); end constructor; "
        "  public method Stash() "
        "    using var it := new Item(); "
        "    var xs: Array<Item> := [it]; "
        "    _keep := xs.ToArray(); "
        "  end Stash; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsConstructionStoreEscape(r, "it"));
}

// The literal, VALUE-TYPE-HOLDS-BORROWS element: `Pair` is a record holding a
// class borrow (§9.7's collection bullet), so the destination alone decides and
// no source lookup is consulted — §9.7's "no trust residual" regime.
BEZEL_TEST(collection_construction_store_6468,
           literal_record_element_into_field_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "record Pair(Ref: Item); "
        "class Box "
        "  public owned _kp: Array<Pair>; "
        "  public constructor() _kp := new Array<Pair>(); end constructor; "
        "  public method Stash() "
        "    using var it := new Item(); "
        "    _kp := [new Pair(it)]; "
        "  end Stash; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsHoldsBorrowsConstruction(r, "record"));
}

// The shape-(i) copy, VALUE-TYPE-HOLDS-BORROWS element — the case §9.7 names
// in its own text: "`_keep := xs.ToArray();` … rejected at the call, whatever
// the sources of `xs`'s elements, as a store into a field-held or escaping
// collection is, with no residual."
BEZEL_TEST(collection_construction_store_6468,
           toarray_record_element_into_field_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "record Pair(Ref: Item); "
        "class Box "
        "  public owned _kp: Array<Pair>; "
        "  public constructor() _kp := new Array<Pair>(); end constructor; "
        "  public method Stash() "
        "    using var it := new Item(); "
        "    var xs: Array<Pair> := [new Pair(it)]; "
        "    _kp := xs.ToArray(); "
        "  end Stash; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsHoldsBorrowsConstruction(r, "record"));
}

// RANGE SLICING, the third shape §28.11 names: "`ToArray` (§15.1), range
// slicing (§15.6) and a direct `FromCollectionLiteral` call are shape (i)'s
// copies". #6468 could not probe this spelling (its author reported the
// `xs[0..1]` attempt died at run time and suspected a probe artifact); the
// §15.6 form is `xs[0..2]` over a two-element source, which reaches the same
// rule.
BEZEL_TEST(collection_construction_store_6468,
           range_slice_record_element_into_field_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "record Pair(Ref: Item); "
        "class Box "
        "  public owned _kp: Array<Pair>; "
        "  public constructor() _kp := new Array<Pair>(); end constructor; "
        "  public method Stash() "
        "    using var it := new Item(); "
        "    var xs: Array<Pair> := [new Pair(it), new Pair(it)]; "
        "    _kp := xs[0..2]; "
        "  end Stash; "
        "end Box;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsHoldsBorrowsConstruction(r, "record"));
}

// §9.7's second named spelling: "`var ys := xs.ToArray();` over a local
// `Array<Pair>` is admitted exactly as `ys.Add(xs[0])` is". The destination is
// a LOCAL at the same depth as the source — it dies no later — so this is the
// admitted twin of the row above and must not flip with it.
BEZEL_TEST(collection_construction_store_6468,
           local_toarray_record_element_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "record Pair(Ref: Item); "
        "class C public static method Main(): int "
        "  using var it := new Item(); "
        "  var xs: Array<Pair> := [new Pair(it)]; "
        "  var ys := xs.ToArray(); "
        "  return ys.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// POSITIVE — no over-rejection. Each row is a program the spec ADMITS.
// =============================================================================

// §28.11's store rule clause 1: "parameter-sourced … borrows are admitted."
// The element is a borrow of a PARAMETER, whose source is the caller's frame —
// it outlives everything here, so the literal into a field is admitted under
// the residual. This is the single most important negative control: keying the
// check on the DESTINATION alone (the §9.7 holds-borrows rule) rather than on
// destination × source (the §28.11 class rule) would wrongly reject it.
BEZEL_TEST(collection_construction_store_6468,
           literal_parameter_element_into_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "class Box "
        "  public owned _keep: Array<Item>; "
        "  public constructor() _keep := new Array<Item>(); end constructor; "
        "  public method Stash(p: Item) _keep := [p]; end Stash; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same admission through the shape-(i) copy: the SOURCE collection is a
// parameter, so its elements' borrows are the caller's.
BEZEL_TEST(collection_construction_store_6468,
           toarray_parameter_element_into_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "class Box "
        "  public owned _keep: Array<Item>; "
        "  public constructor() _keep := new Array<Item>(); end constructor; "
        "  public method Stash(src: Array<Item>) _keep := src.ToArray(); end Stash; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.11's store rule clause 1 again, for a FIELD source: `_src.ToArray()`
// where `_src` is a field. §28.11 names this spelling as admitted — "`return
// _items.ToArray()` over a field are admitted under the residual".
BEZEL_TEST(collection_construction_store_6468,
           toarray_field_source_into_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "class Box "
        "  public owned _keep: Array<Item>; "
        "  private owned _src: Array<Item>; "
        "  public constructor(f: Item) "
        "    _keep := new Array<Item>(); _src := new Array<Item>(); _src := [f]; "
        "  end constructor; "
        "  public method Stash() _keep := _src.ToArray(); end Stash; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.11: "A copy whose elements carry no borrow — a value type that holds
// none, a null-source value, a `string`, a sendable element — is a copy and
// nothing more." An `int` element carries no borrow at all, so the literal
// into a field is admitted. The element-type gate must run BEFORE the
// destination gate, or every value-typed field assignment breaks.
BEZEL_TEST(collection_construction_store_6468,
           value_element_literal_into_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public owned _ints: Array<int>; "
        "  public constructor() _ints := new Array<int>(); end constructor; "
        "  public method Fill() _ints := [1, 2, 3]; end Fill; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A LOCAL destination at the same depth as the source. §28.11's store rule
// rejects only a collection that "provably outlives" the source's scope; a
// same-depth local dies no later, so this is admitted. This is the row that
// separates the `localDepth_` comparison from a blanket destination rejection.
BEZEL_TEST(collection_construction_store_6468,
           local_destination_same_depth_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "class C public static method Main(): int "
        "  using var it := new Item(); "
        "  var dest: Array<Item> := [it]; "
        "  return dest.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A fresh producer in element position is §15.4's OWN rejection (a fresh
// unbound owned temporary of non-trivial-dispose class type is rejected at the
// literal site), and it is a LOCAL-destination rejection — MEASURED on the
// pre-fix tree, where the local form already rejects and the FIELD form of the
// same literal does not. That field-destination gap is PRE-EXISTING and is
// NOT this check's: §15.4's rule keys on the fresh producer, and the borrow
// the store rule reads is not a borrow of any scope-bound source (a `new`
// expression carries none), so neither §28.11 nor §9.7 has anything to say
// about it. Pinned here so the distinction is recorded rather than assumed:
// this row asserts §15.4's rule fires where it is implemented, and asserts
// that §28.11's CONSTRUCTION diagnostic does NOT — which is the property this
// change could plausibly have broken by over-firing on a fresh element.
BEZEL_TEST(collection_construction_store_6468,
           fresh_producer_literal_is_a_sec_15_4_local_destination_rule) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "class C public static method Main(): int "
        "  var d: Array<Item> := [new Item()]; "
        "  return d.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // §15.4's own literal-site rule, not §28.11's store rule: a fresh producer
    // is not a borrow of a scope-bound binding, so `rejectsConstruction-
    // StoreEscape` must NOT match here.
    BEZEL_EXPECT_FALSE(rejectsConstructionStoreEscape(r, "new Item"));
}

// The field-destination spelling of the same fresh-producer literal is
// ACCEPTED on the pre-fix tree and STILL accepted after this change — measured
// on both arms. §15.4's rule does not reach it and §28.11's store rule has no
// source to read. Pinned as the current behaviour so a future change to
// either rule shows up here as a flip rather than silently.
BEZEL_TEST(collection_construction_store_6468,
           fresh_producer_literal_field_destination_unaffected_by_this_check) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Item public constructor() end constructor; "
        "  destructor end destructor; end Item; "
        "class Box "
        "  public owned _keep: Array<Item>; "
        "  public constructor() _keep := new Array<Item>(); end constructor; "
        "  public method Stash() _keep := [new Item()]; end Stash; "
        "end Box;");
    BEZEL_EXPECT_FALSE(rejectsConstructionStoreEscape(r, "new Item"));
}
