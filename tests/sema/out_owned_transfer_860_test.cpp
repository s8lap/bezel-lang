// fix #860 / §28.3 / §28.11: model the caller-side `out owned` ownership
// transfer. Moving an element out of an owning collection via a
// `Try*(out owned item: T)` call (OwnedStack.TryPop, OwnedQueue.TryDequeue,
// OwnedLinkedList.TryPopFirst/Last, OwnedSet.TryTake, OwnedDictionary.
// TryPopValue, Channel.TryReceive, Weak.TryUpgrade) — or through a USER
// method's `out owned` parameter — transfers ownership of the moved-out value
// to the caller's local. The ownership checker did NOT model this owned-OUT
// direction (it modelled only owned-IN sinks), so dropping the receiving local
// without disposal was wrongly ACCEPTED — a leak.
//
// The fix marks the `out owned` argument local as Owned after the call (the
// owned-OUT mirror of `builtinOwnedSinkArg`'s owned-IN consumption), so the
// existing §28.3 owned-local discharge check rejects an undischarged item. The
// canonical null-safe consumer `item?.Dispose()` discharges it on both the
// success and the (null) Try* false-return path.
//
// These tests pin: the dropped out-owned item REJECT (TryPop / TryDequeue /
// TryPopValue, builtin and user out-owned param); `item?.Dispose()` ACCEPT;
// move / return ACCEPT (discharged by transfer); and — the key guardrails —
// that the BORROW forms `TryPop(out item)` / `TryPeek(out item)` (no `owned`),
// a value-typed `out owned` mismatch (no disposal obligation), and an ordinary
// `out` (non-owned) `TryGetValue(out value)` are NOT over-rejected.
// // per §28.3, §28.11, #860

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// A disposable Cell class plus a Main body that varies per test. A complete,
// valid program so only the ownership facts under test drive the result.
std::string withMain(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public _id: int := 0; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C "
        "  public static method Main(): int "
        "    do ") +
        body +
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// True iff some diagnostic names `<name>` as an owned value dropped without
// disposal (the §28.3 owned-local discharge error the out-owned local triggers
// when undischarged).
bool hasDropError(const bezel::test::OwnershipResult& r,
                  const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("owned value in `" + name + "`") !=
                std::string::npos &&
            d.message.find("without disposal") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §28.3 / §28.11 / #860 (headline): an `out owned var item` inline local moved
// out of an OwnedStack via TryPop and then dropped without disposal must be
// REJECTED. Before the fix this was silently accepted (a leak).
BEZEL_TEST(out_owned_transfer_860, trypop_dropped_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwnedStack<Cell>(); "
        "s.Push(move (new Cell(1))); "
        "var ok := s.TryPop(out owned var item); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDropError(r, "item"));
}

// §28.3 / §28.11 / #860: the canonical null-safe consumer `item?.Dispose()`
// discharges the conservatively-Owned out-owned local on both the success and
// the (null) false-return path. Must be ACCEPTED.
BEZEL_TEST(out_owned_transfer_860, trypop_null_safe_dispose_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwnedStack<Cell>(); "
        "s.Push(move (new Cell(1))); "
        "var ok := s.TryPop(out owned var item); "
        "item?.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.11 / #860: discharge by moving the out-owned local into another
// owning collection (`s2.Push(move item)`) — the move discharges `item`. Must
// be ACCEPTED.
BEZEL_TEST(out_owned_transfer_860, trypop_moved_out_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwnedStack<Cell>(); "
        "using var s2 := new OwnedStack<Cell>(); "
        "s.Push(move (new Cell(1))); "
        "var ok := s.TryPop(out owned var item); "
        "s2.Push(move item); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.11 / #860: discharge by returning the out-owned local from a
// method whose return type is `owned Cell?`. Must be ACCEPTED.
BEZEL_TEST(out_owned_transfer_860, trypop_returned_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public _id: int := 0; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C "
        "  public static method Take(): owned Cell? "
        "    using var s := new OwnedStack<Cell>(); "
        "    s.Push(move (new Cell(1))); "
        "    var ok := s.TryPop(out owned var item); "
        "    return move item; "
        "  end Take; "
        "  public static method Main(): int "
        "    using var c := Take(); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.11 / #860: OwnedQueue.TryDequeue — out-owned at arg 0 — dropped
// must be REJECTED (a second built-in family beyond TryPop).
BEZEL_TEST(out_owned_transfer_860, trydequeue_dropped_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var q := new OwnedQueue<Cell>(); "
        "q.Enqueue(move (new Cell(1))); "
        "var ok := q.TryDequeue(out owned var item); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDropError(r, "item"));
}

// §28.3 / §28.11 / #860: OwnedDictionary.TryPopValue(key, out owned value) —
// out-owned at arg POSITION 1 (not 0) — dropped must be REJECTED. Guards the
// per-method argument-index table.
BEZEL_TEST(out_owned_transfer_860, trypopvalue_pos1_dropped_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var d := new OwnedDictionary<string, Cell>(); "
        "d.Add(\"a\", move (new Cell(1))); "
        "var ok := d.TryPopValue(\"a\", out owned var v); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDropError(r, "v"));
}

// §28.3 / §28.11 / #860: TryPopValue at arg 1 with a null-safe dispose — the
// key (arg 0) is a borrow and only arg 1 is the out-owned transfer. ACCEPT.
BEZEL_TEST(out_owned_transfer_860, trypopvalue_pos1_dispose_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var d := new OwnedDictionary<string, Cell>(); "
        "d.Add(\"a\", move (new Cell(1))); "
        "var ok := d.TryPopValue(\"a\", out owned var v); "
        "v?.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.11 / #860 (user method): a USER method's `out owned r` parameter
// is the owned-OUT direction. The receiving local dropped must be REJECTED.
// Before the fix the `out owned` formal was mis-classified as an owned-IN sink
// (mis-marking the receiving local moved-from); now it correctly receives.
BEZEL_TEST(out_owned_transfer_860, user_out_owned_param_dropped_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public _id: int := 0; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class Box "
        "  private owned _c: Cell; "
        "  public constructor() _c := move (new Cell(1)); end constructor; "
        "  public method TakeIt(out owned r: Cell): bool "
        "    r := move _c; _c := move (new Cell(2)); return true; "
        "  end TakeIt; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var b := new Box(); "
        "      var ok := b.TakeIt(out owned var got); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDropError(r, "got"));
}

// §28.3 / §28.11 / #860 (user method): the same user `out owned` form with a
// null-safe dispose of the receiving local — ACCEPT.
BEZEL_TEST(out_owned_transfer_860, user_out_owned_param_dispose_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public _id: int := 0; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class Box "
        "  private owned _c: Cell; "
        "  public constructor() _c := move (new Cell(1)); end constructor; "
        "  public method TakeIt(out owned r: Cell): bool "
        "    r := move _c; _c := move (new Cell(2)); return true; "
        "  end TakeIt; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var b := new Box(); "
        "      var ok := b.TakeIt(out owned var got); "
        "      got?.Dispose(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.11 / #860 (THE BORROW-FORM GUARDRAIL): `Stack<int>.TryPop(out
// item)` — the borrowing form, no `owned`, value-typed — has NO discharge
// obligation on `item`. Dropping it must be ACCEPTED. An over-broad fix keying
// on the call-site/`TryPop` name alone would wrongly reject this.
BEZEL_TEST(out_owned_transfer_860, borrow_trypop_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new Stack<int>(); "
        "s.Push(7); "
        "var item: int := 0; "
        "var ok := s.TryPop(out item); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.11 / #860 (BORROW-FORM GUARDRAIL): `OwnedStack.TryPeek(out item)`
// produces a BORROW of the top element (stdlib §4.11), not an owned transfer —
// dropping `item` must be ACCEPTED.
BEZEL_TEST(out_owned_transfer_860, borrow_trypeek_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new OwnedStack<Cell>(); "
        "s.Push(move (new Cell(1))); "
        "var item: Cell? := null; "
        "var ok := s.TryPeek(out item); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.11 / #860 (VALUE-TYPED GUARDRAIL): inline `out var item` on a
// value-typed `Stack<int>.TryPop(out item: int)` — `item` is an `int` with no
// disposal obligation — must NOT be over-rejected. The ownership checker must
// not assign an Owned discharge obligation to value-type out-var locals.
BEZEL_TEST(out_owned_transfer_860, value_typed_out_owned_not_rejected) {
    auto r = parseAndCheckOwnership(withMain(
        "using var s := new Stack<int>(); "
        "s.Push(7); "
        "var ok := s.TryPop(out var item); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 / §28.11 / #860 (OUT-NON-OWNED GUARDRAIL): an ordinary `out` (non-
// owned) parameter `Dictionary.TryGetValue(key, out value)` — `out`, not
// `out owned` — carries no discharge obligation. Dropping `value` must be
// ACCEPTED.
BEZEL_TEST(out_owned_transfer_860, out_non_owned_trygetvalue_accepted) {
    auto r = parseAndCheckOwnership(withMain(
        "using var d := new Dictionary<string, int>(); "
        "d.Add(\"a\", 5); "
        "var v: int := 0; "
        "var ok := d.TryGetValue(\"a\", out v); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
