// Spec-derived tests for #1020 / §28.6 (v0.153): a freshly-produced owned value
// stored into a NON-`owned` (borrow) field is rejected — the field does not own
// it, so the value would be left with no owner and leak. This is the storing-side
// counterpart of the existing non-`owned`-RETURN rule, and the inverse of #402
// (a borrow stored INTO an `owned` field).
//
// §28.6 (v0.153): "A freshly-produced owned value — a `new` expression, a call
// returning `owned T`, a `move` expression, or an owned operator result — stored
// into a non-`owned` (borrow) field is an error: the field does not own its
// referent, so the fresh value would be left with no owner and leak. Declare the
// field `owned` … or hand the value to a method that takes it `owned`."
//
// `producerState` classifies `new` / owned-returning call / `move` / owned
// operator as Owned; a bare borrow read (a back-reference parameter, a borrowed
// local) is Borrowed and stays ACCEPTED — back-reference fields are the
// legitimate non-`owned` class-field use (§28.5 / Guide 1 "point back at
// something that outlives you").

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// `Wrap` has a BORROW field `_b: Inner` (no `owned`), an `owned` control field
// `_o`, an owned-returning factory, and a value-typed field `_v` (a record) for
// the value-field negative-control.
std::string mod(const std::string& member) {
    return std::string("module M; "
        "class Inner end Inner; "
        "record Pt(X: int, Y: int); "
        "class Wrap "
        "  private _b: Inner?; "                         // BORROW field (no owned; nullable so members without a ctor satisfy DA)
        "  private owned _o: Inner := new Inner(); "       // owned control field
        "  private _v: Pt := new Pt(0, 0); "               // value-typed field
        "  public static method MakeOwned(): owned Inner "
        "    return new Inner(); "
        "  end MakeOwned; "
        ) + member + " end Wrap;";
}

} // namespace

// ---- NEGATIVE: a fresh owned producer into a non-`owned` field → REJECT ----

// §28.6 v0.153: the headline constructor case. `new Inner()` is a fresh owned
// producer; storing it into the borrow field `_b` leaves it with no owner. REJECT.
BEZEL_TEST(fresh_owned_into_borrow_field_1020, ctor_new_into_borrow_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public constructor() "
        "  this._b := new Inner(); "
        "end constructor;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An owned-returning CALL result is a fresh owned value (§28.3 producers table);
// into a borrow field it leaks. REJECT.
BEZEL_TEST(fresh_owned_into_borrow_field_1020, owned_call_into_borrow_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method Set() "
        "  this._b := MakeOwned(); "
        "end Set;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A `move` of an owned local transfers ownership out of the local; storing the
// moved value into a borrow field leaves it with no owner. REJECT.
BEZEL_TEST(fresh_owned_into_borrow_field_1020, move_owned_local_into_borrow_field_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public method Set() "
        "  var x := new Inner(); "
        "  this._b := move x; "
        "end Set;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- POSITIVE: must stay ACCEPTED (no over-rejection) ----

// The canonical back-reference: a bare BORROW parameter stored into a non-`owned`
// field. `b` is a borrow (source: caller), not a fresh producer — the legitimate
// non-`owned` class-field use. ACCEPT.
BEZEL_TEST(fresh_owned_into_borrow_field_1020, borrow_param_back_reference_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public constructor(b: Inner) "
        "  this._b := b; "
        "end constructor;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A bare borrowed LOCAL (aliasing a borrow parameter) into the borrow field is a
// borrow store, not a fresh producer. ACCEPT.
BEZEL_TEST(fresh_owned_into_borrow_field_1020, borrow_local_into_borrow_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method Set(b: Inner) "
        "  var t := b; "
        "  this._b := t; "
        "end Set;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A fresh owned producer into an `owned` field is the legal ownership transfer
// (handled by the §28.5 owned-field path); it must NOT be caught by the #1020
// rule. ACCEPT.
BEZEL_TEST(fresh_owned_into_borrow_field_1020, new_into_owned_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method Set() "
        "  this._o.Dispose(); "
        "  this._o := new Inner(); "
        "end Set;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A fresh VALUE (`new Pt(...)`, a record) into a value-typed field carries no
// disposal obligation, so it is not a leak and must stay accepted. ACCEPT.
BEZEL_TEST(fresh_owned_into_borrow_field_1020, new_value_record_into_value_field_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public method Set() "
        "  this._v := new Pt(1, 2); "
        "end Set;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
