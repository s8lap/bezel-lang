// Spec-derived tests for the #1007 fix — returning a BORROW through an
// owning-collection / owning-wrapper return type (§28.3 / §28.6 / §5.11).
//
// An owning-collection / owning-wrapper return type (`OwnedArray<T>`,
// `OwnedDictionary<K,V>`, `OwnedTask<T>`, `Shared<T>`, …) names ELEMENT
// ownership (rationale §5.11); the CONTAINER-ownership axis is the separate
// `owned` qualifier. A method declared `: OwnedArray<T>` WITHOUT `owned` hands
// the caller a value the caller tracks as OWNED (it must discharge it — a
// `using var` binding disposes it). If the returned value is a BORROW (a
// non-`owned` parameter, a borrowed local, or a plain field read), that borrow
// is laundered into a second owner: both the original owner and the caller
// dispose the same value — a double-free (#1007). Such a return must be
// rejected at its definition site, exactly as a borrowed `Array<T>` return is
// rejected by the §28.3 borrow-lifetime rule.
//
// A genuine owned producer (`new`, `move`, a call returning `owned`, an owned
// local/param transferred out) is NOT a borrow and still compiles; an `owned`
// return type makes the container-ownership transfer explicit and is exempt.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    return std::string("module M; class Cell end Cell; ") + body;
}

} // namespace

// --- REJECT: a borrowed param returned through an owning-collection type. ---

// §28.3 / §5.11 (#1007 repro): the OwnedArray double-free. `a` is a non-owned
// param (a borrow); returning it through `: OwnedArray<Cell>` (no `owned`)
// launders it into an owned alias the caller disposes — a double-free.
BEZEL_TEST(borrow_return_1007, owned_array_param_exprbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Identity(a: OwnedArray<Cell>): OwnedArray<Cell> "
        "    => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The same laundering through a BLOCK body `return a;` is rejected too.
BEZEL_TEST(borrow_return_1007, owned_array_param_blockbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Identity(a: OwnedArray<Cell>): OwnedArray<Cell> "
        "    return a; "
        "  end Identity; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §5.11 sibling: OwnedDictionary borrow-return is the same hazard.
BEZEL_TEST(borrow_return_1007, owned_dictionary_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(d: OwnedDictionary<int, Cell>): "
        "      OwnedDictionary<int, Cell> => d; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.13 sibling: a borrowed Shared<T> param returned through Shared<T>.
BEZEL_TEST(borrow_return_1007, shared_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(s: Shared<Cell>): Shared<Cell> => s; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §30.5 sibling: a borrowed OwnedTask<T> param returned through OwnedTask<T>.
BEZEL_TEST(borrow_return_1007, owned_task_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(t: OwnedTask<Cell>): OwnedTask<Cell> => t; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A borrowed LOCAL (a plain `var` bound to a borrowed param) returned through
// the owning type is the same laundering — rejected.
BEZEL_TEST(borrow_return_1007, owned_array_borrowed_local_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: OwnedArray<Cell>): OwnedArray<Cell> "
        "    var b := a; "
        "    return b; "
        "  end Id; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A plain (non-`owned`) field read returned through the owning type is a borrow
// of `this`'s field — laundering it is the same double-free hazard.
BEZEL_TEST(borrow_return_1007, owned_array_field_read_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public _items: OwnedArray<Cell>; "
        "  public constructor(a: OwnedArray<Cell>) this._items := a; end constructor; "
        "  public method Get(): OwnedArray<Cell> => this._items; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- ACCEPT: genuine owned producers and explicit-`owned` transfers. ---

// §28.6 (#1008 Part C tightening): a FRESH owning collection returned through a
// NON-`owned` return is now REJECTED — the caller cannot distinguish fresh from
// borrow by the return signature, so a non-`owned` owning-collection return
// always means borrow; a fresh value must declare `owned`.
BEZEL_TEST(borrow_return_1007, fresh_new_owned_array_requires_owned) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Make(): OwnedArray<Cell> => new OwnedArray<Cell>(); "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The same fresh value returned through an `owned` return type is clean — the
// transfer is explicit and the caller disposes it once.
BEZEL_TEST(borrow_return_1007, fresh_new_owned_array_owned_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Make(): owned OwnedArray<Cell> => new OwnedArray<Cell>(); "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.6 (#1008 Part C): an OWNED value (here an owned parameter) returned through
// a NON-`owned` owning return is now rejected — under the borrow reading a
// non-`owned` return never disposes, so returning an owned value that way leaks;
// declare the return `owned` to transfer.
BEZEL_TEST(borrow_return_1007, owned_param_through_nonowned_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Pass(owned a: OwnedArray<Cell>): OwnedArray<Cell> "
        "    => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.6 (#1008 Part C): a bare read of an `owned` owning-collection field of the
// RECEIVER (`this`) returned through a non-`owned` return is a legal receiver-
// borrow (the caller may read it but not dispose it; the receiver disposes it).
BEZEL_TEST(borrow_return_1007, owned_field_read_this_borrow_allowed) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public owned _items: OwnedArray<Cell>; "
        "  public constructor() this._items := new OwnedArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwnedArray<Cell> => this._items; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An explicit `owned` return type makes the container-ownership transfer
// explicit; the rule does not fire (the caller-tracks-as-owned was the
// declared intent). Must compile.
BEZEL_TEST(borrow_return_1007, owned_return_type_exempt_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: OwnedArray<Cell>): owned OwnedArray<Cell> "
        "    => a; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A call returning an owned value (Shared<T>.Clone() → `owned Shared<T>`) is a
// genuine owned producer, not a borrow — must compile.
BEZEL_TEST(borrow_return_1007, shared_clone_owned_producer_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Dup(s: Shared<Cell>): Shared<Cell> => s.Clone(); "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (the §28.3 asymmetry the issue notes): a borrowed `Array<int>` param
// returned through `: Array<int>` is a sound borrow return — Array<int> is not
// an owning type, so the #1007 rule does not fire. Must compile.
BEZEL_TEST(borrow_return_1007, array_int_borrow_return_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: Array<int>): Array<int> => a; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
