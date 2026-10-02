// Spec-derived tests for the #1007 fix — returning a BORROW through an
// owning-collection / owning-wrapper return type (§28.3 / §28.6 / §5.11).
//
// An owning-collection / owning-wrapper return type (`OwningArray<T>`,
// `OwningDictionary<K,V>`, `OwningTask<T>`, `Shared<T>`, …) names ELEMENT
// ownership (rationale §5.11); the CONTAINER-ownership axis is the separate
// `owned` qualifier. A method declared `: OwningArray<T>` WITHOUT `owned` hands
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

// §28.3 / §5.11 (#1007 repro): the OwningArray double-free. `a` is a non-owned
// param (a borrow); returning it through `: OwningArray<Cell>` (no `owned`)
// launders it into an owned alias the caller disposes — a double-free.
BEZEL_TEST(borrow_return_1007, owned_array_param_exprbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Identity(a: OwningArray<Cell>): OwningArray<Cell> "
        "    => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The same laundering through a BLOCK body `return a;` is rejected too.
BEZEL_TEST(borrow_return_1007, owned_array_param_blockbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Identity(a: OwningArray<Cell>): OwningArray<Cell> "
        "    return a; "
        "  end Identity; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §5.11 sibling: OwningDictionary borrow-return is the same hazard.
BEZEL_TEST(borrow_return_1007, owned_dictionary_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(d: OwningDictionary<int, Cell>): "
        "      OwningDictionary<int, Cell> => d; "
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

// §30.5 sibling: a borrowed OwningTask<T> param returned through OwningTask<T>.
BEZEL_TEST(borrow_return_1007, owned_task_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(t: OwningTask<Cell>): OwningTask<Cell> => t; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A borrowed LOCAL (a plain `var` bound to a borrowed param) returned through
// the owning type is the same laundering — rejected.
BEZEL_TEST(borrow_return_1007, owned_array_borrowed_local_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: OwningArray<Cell>): OwningArray<Cell> "
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
        "  public _items: OwningArray<Cell>; "
        "  public constructor(a: OwningArray<Cell>) this._items := a; end constructor; "
        "  public method Get(): OwningArray<Cell> => this._items; "
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
        "  public static method Make(): OwningArray<Cell> => new OwningArray<Cell>(); "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The same fresh value returned through an `owned` return type is clean — the
// transfer is explicit and the caller disposes it once.
BEZEL_TEST(borrow_return_1007, fresh_new_owned_array_owned_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Make(): owned OwningArray<Cell> => new OwningArray<Cell>(); "
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
        "  public static method Pass(owned a: OwningArray<Cell>): OwningArray<Cell> "
        "    => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.6 (#1008 Part C, restored by #2269 over #1760): a bare read of an
// `owned` owning-collection field of THE RECEIVER (`this`) returned through
// a non-`owned` owning-collection return is the §28.6 admission (worked
// example `return _transactions;` from `method GetTransactions():
// OwningArray<Transaction>`, spec §28.6 L7000–L7003). The carve-out is
// `exprIsReceiverOwnedFieldBorrow(e)` — this-restricted (a bare identifier
// resolving to a field, or an explicit `this.f`); a field of another
// object / a borrowed parameter / a borrowed local is NOT admitted and
// still falls to the rejection below. The indirect-dispatch unsoundness
// the carve-out reintroduces is filed as a separate spec-amendment follow-
// up to #2269 (not a definition-site rejection).
BEZEL_TEST(borrow_return_1007, owned_field_read_this_borrow_accepted_2269) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor() this._items := new OwningArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningArray<Cell> => this._items; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// UPDATED for issue #3993: this row originally asserted that an explicit
// `owned` return type EXEMPTS the return from the #1007 rule ("the
// caller-tracks-as-owned was the declared intent") — but declaring `owned`
// only ANNOUNCES the contract; #1007's own `checkBorrowReturnedThroughOwning
// Type` never verified the returned expression actually satisfies it
// (`if (returnOwned_) return;` at its own top), so `a` — a plain borrowed
// parameter — flowed out labeled "owned" untouched. Measured via ASan on the
// concrete analogue (an `OwningArray<Cell>` with real elements, borrowed
// through an identical `Id`, returned `owned`, and disposed at both the
// caller's binding and the parameter's real `using var` owner): SEGV in
// `bz_array_free` — a genuine cascading double-free of the array's elements,
// not merely a theoretical laundering. The new `checkOwnedReturnDirectBorrow`
// (§28.3 *Borrow lifetime rules*) closes this — the declared-`owned`-return
// case is REJECTED exactly like the unmarked case above, not exempted.
BEZEL_TEST(borrow_return_1007, owned_return_type_borrow_rejected_spec_3993) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: OwningArray<Cell>): owned OwningArray<Cell> "
        "    => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The genuine owned producer through the SAME owned-declared return type is
// NOT over-rejected — `new` is a fresh production, not a borrow.
BEZEL_TEST(borrow_return_1007, owned_return_type_fresh_producer_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Make(): owned OwningArray<Cell> "
        "    => new OwningArray<Cell>(); "
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

// §28.3 announcement table row 4 (spec v0.313, #5123): a plain static return
// of `Array<int>` names no responsible party ("a trivial-dispose class such as
// `Array<T>` included ... its static return names no responsible party
// either") — rejected at the declaration, exactly as this file's user-class
// `Pick(a, b): Res` is. Was `array_int_borrow_return_clean` (ACCEPT, a
// documented gap) until the v0.309 ruling made row 4 explicit.
BEZEL_TEST(borrow_return_1007, array_int_static_return_rejected_spec_28_3_row4) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Id(a: Array<int>): Array<int> => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returns class/interface type `Array` without `owned`"));
}

// §28.11 / #1264: a plain `var b := move a` of an OWNING COLLECTION must flag `b`
// for affine auto-drop — the move transfers the CONTAINER ownership `a` held, so
// `b` owns the collection and auto-drops at scope end (cascading to its elements,
// §28.11). Before the fix, producerState classified `move <owning-collection>` as
// Borrowed (the owning-collection name-head fallback denotes ELEMENT ownership for
// method RETURNS), so `b` got no auto-drop flag and was never disposed → its
// elements leaked. The runtime cascade is verified AOT + ASan-clean; here we
// assert the sema flag: the auto-drop warning fires for `b` (checkOwnership is
// omitted by the codegen_support runJit harness, so this is tested at the
// ownership-pass level).
BEZEL_TEST(borrow_return_1007, move_owning_collection_local_auto_dropped_1264) {
    auto r = parseAndCheckOwnership(
        "module M; class Cell end Cell; "
        "class P public static method Main(): int "
        "  do "
        "    var a := new OwningArray<Cell>(); a.Add(new Cell()); "
        "    var b := move a; "
        "  end; "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "b"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
