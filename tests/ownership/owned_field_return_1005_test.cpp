// Spec-derived tests for the #1005 fix — returning an owned field's `move`d
// value through a non-`owned` method return type (§28.6, Bezel_Language_Spec.md
// L5066).
//
// §28.6: "An owned field's value may not be returned from a method unless the
// return type is also declared `owned`." The leak is the `move` form: `move
// this.f` / `move f` reads the owned field's *value* as an owned value (§28.8.1)
// and nulls the field; returned through a NON-`owned` (borrow) return type the
// moved value escapes the body with no owner — a leak (the #1005 repro
// `Take(): Cell? => move v;`).
//
// A BARE read of an owned field (`=> v;` / `return this.v;`) is a legal BORROW
// return, NOT a leak: per §28.6's "Methods returning `this`" rule a non-`owned`
// class return type yields a borrow whose lifetime is bounded by the receiver,
// which the receiver's destructor still disposes (the field's owner is unchanged
// — no second owner, no double-free). The §28.6 `return _transactions` example
// is the OWNING-COLLECTION case (field type is an owning collection), rejected
// separately by #1007. The cross-module `t3` canary (`GetConn(): Connection =>
// Conn`, returns 55 ASan-clean) codifies that a bare class-type owned-field read
// is an accepted borrow. So this fix flags ONLY the `move` form.
//
// An `owned` return type (`: owned T`) is the legal ownership-transfer form
// (§28.8) and is exempt. The check is scoped to owned FIELD `move`s only; owned
// LOCAL returns (`var r := new Cell(); return r;`) are a separate concern
// (#1006a) and are intentionally NOT rejected here. A bare owned-field borrow, a
// borrow of a NON-owned field, and a fresh `new` must all still compile.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A `Cell` class (the owned value) plus a `Foo` value-bearing non-owned field
// type, then the test body. A class with only owned-bearing fields and no native
// resource needs no destructor (§28.6), but `Box` carries an explicit empty one
// matching the issue repro.
std::string mod(const std::string& body) {
    return std::string("module M; class Cell end Cell; class Foo end Foo; ") +
           body;
}

} // namespace

// --- REJECT: owned field returned through a non-`owned` return type. ---

// §28.6 (#1005 repro): `move v` of the owned field `v` through `: Cell?` (no
// `owned`) — the moved value escapes the body with no owner. Expression body.
BEZEL_TEST(owned_field_return_1005, move_owned_field_exprbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Box "
        "  public owned v: Cell?; "
        "  public constructor(owned x: Cell) v := x; end constructor; "
        "  destructor end destructor; "
        "  public method Take(): Cell? => move v; "
        "end Box;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "cannot return an owned field's `move`d value through a non-`owned` return"));
}

// The same `move v` through a non-`owned` return in a BLOCK body is rejected too.
BEZEL_TEST(owned_field_return_1005, move_owned_field_blockbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Box "
        "  public owned v: Cell?; "
        "  public constructor(owned x: Cell) v := x; end constructor; "
        "  destructor end destructor; "
        "  public method Take(): Cell? "
        "    return move v; "
        "  end Take; "
        "end Box;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "cannot return an owned field's `move`d value through a non-`owned` return"));
}

// --- ACCEPT: bare owned-field borrows, the `owned` return type, non-owned. ---

// A BARE read of the owned field (no `move`) through a non-`owned` class return
// is a legal BORROW (§28.6 "Methods returning `this`"): the field's owner is
// unchanged, the receiver's destructor still disposes it. Must compile — this is
// the `t3` canary shape (`GetConn(): Connection => Conn`). Expression body.
BEZEL_TEST(owned_field_return_1005, bare_read_owned_field_borrow_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Box "
        "  public owned v: Cell?; "
        "  public constructor(owned x: Cell) v := x; end constructor; "
        "  destructor end destructor; "
        "  public method Peek(): Cell? => v; "
        "end Box;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Bare read of the owned field via explicit `this.v` (block body) — same legal
// borrow. Must compile.
BEZEL_TEST(owned_field_return_1005, bare_read_this_owned_field_borrow_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Box "
        "  public owned v: Cell?; "
        "  public constructor(owned x: Cell) v := x; end constructor; "
        "  destructor end destructor; "
        "  public method Peek(): Cell? "
        "    return this.v; "
        "  end Peek; "
        "end Box;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- ACCEPT: the `owned` return type and non-owned borrows. ---

// The legal owned-transfer form: `move v` through `: owned Cell?` declares the
// ownership transfer to the caller — must compile (the issue's CORRECT form).
BEZEL_TEST(owned_field_return_1005, move_owned_field_owned_return_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Box "
        "  public owned v: Cell?; "
        "  public constructor(owned x: Cell) v := x; end constructor; "
        "  destructor end destructor; "
        "  public method Take(): owned Cell? => move v; "
        "end Box;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A borrow of a NON-owned field through the matching (non-`owned`) return type
// is a sound borrow return (§28.3) — `f` is not owned, so §28.6 does not fire.
// Must compile.
BEZEL_TEST(owned_field_return_1005, borrow_non_owned_field_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public f: Foo; "
        "  public constructor(f0: Foo) f := f0; end constructor; "
        "  public method Peek(): Foo => f; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A FRESH `new` returned through `: owned Cell` is a genuine owned producer (not
// an owned-field read) — §28.6 does not fire. Must compile.
BEZEL_TEST(owned_field_return_1005, fresh_new_owned_return_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Maker "
        "  public method Make(): owned Cell => new Cell(); "
        "end Maker;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
