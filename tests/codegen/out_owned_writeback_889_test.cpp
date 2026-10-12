// fix #889 / §28.11 / §28.2: a `Try*(out owned item)` call (e.g.
// `s.TryPop(out owned item)`) removed the element from the owning collection
// and wrote it into the caller's local, but the canonical discharge
// `item?.Dispose()` emitted NO code — `Dispose()` is compiler-synthesized and
// has no MethodSymbol, so the null-conditional call lowering bailed silently.
// The element therefore leaked (its destructor never ran). Beyond that, even a
// plain `item.Dispose()` only DISPATCHED the destructor without reclaiming the
// heap block, because an out-owned recipient local was not recognized as an
// owned slot; and the `out using var item` form registered a `bz_shared_release`
// scope-end disposal (correct only for Weak<T>.TryUpgrade), corrupting the heap
// and leaking when the out value is an owned class element.
//
// Now the moved-out owned element is disposed exactly once by the explicit
// `item?.Dispose()` / `item.Dispose()` (dispatch + reclaim + slot-null), or by
// the `out using var` scope-end dynamic-dispose, or transferred onward by a
// `move`. The borrowing Try* (`out item`, no `owned`) is unchanged — it neither
// reclaims nor double-frees.
//
// Value tests — runJit returns the raw int (kept in 0..255). Each program
// reports a destructor counter (`Cell._n` / a `_log` digit fold) so a leak
// (counter too high) and a double-free (counter negative / a wrong fold) are
// both observable through the return value.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A class with a static destructor counter `_n` (constructor +1, destructor -1).
// A correct program ends with _n == 0; a leak leaves _n > 0; a double-free drives
// _n < 0 (which, returned as an exit code, surfaces as a large value, never 0).
const char* kCellHdr =
    "module M; class Cell public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; end Cell; ";
}  // namespace

// Form A — `out owned var item` inline declaration + `item?.Dispose()` (the
// #889 repro): the single element is disposed exactly once; Cell._n returns to
// 0 (was 1 — the dispose was a no-op and the element leaked).
BEZEL_TEST(out_owned_writeback_889, trypop_out_owned_null_cond_dispose) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var s := new OwningStack<Cell>(); "
        "    s.Push(move (new Cell())); "
        "    var ok := s.TryPop(out var item); "
        "    item?.Dispose(); "
        "  end; return Cell._n; end Main; end C;"), 0);
}

// Form A — the plain (narrowed) `item.Dispose()` form likewise disposes exactly
// once (this form ran the destructor before the fix but leaked the heap block;
// the value-level result was already 0 and stays 0).
BEZEL_TEST(out_owned_writeback_889, trypop_out_owned_narrowed_dispose) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var s := new OwningStack<Cell>(); "
        "    s.Push(move (new Cell())); "
        "    var ok := s.TryPop(out var item); "
        "    if item <> null then item.Dispose(); end; "
        "  end; return Cell._n; end Main; end C;"), 0);
}

// Two elements: TryPop the top and dispose it; the other is disposed by the
// stack at scope end. A `_log := _log*10 + _id` fold confirms BOTH ran exactly
// once and in the expected order (popped id=2 first, then id=1) — 21. A double
// free would corrupt the fold; a missed dispose would drop a digit.
BEZEL_TEST(out_owned_writeback_889, trypop_two_elements_each_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Cell public static _log: int := 0; public _id: int; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  destructor Cell._log := Cell._log * 10 + _id; end destructor; end Cell; "
        "class C public static method Main(): int "
        "  do using var s := new OwningStack<Cell>(); "
        "    s.Push(move (new Cell(1))); s.Push(move (new Cell(2))); "
        "    var ok := s.TryPop(out var item); "  // pops id=2 (top)
        "    item?.Dispose(); "                          // disposes 2 -> log=2
        "  end; return Cell._log; end Main; end C;"), 21);
}

// `out using var item` form: the moved-out owned element is disposed exactly
// once at scope end via dynamic dispatch (was: bz_shared_release on a raw Cell
// pointer — heap corruption + leak, _n stayed 1).
BEZEL_TEST(out_owned_writeback_889, trypop_out_using_var_disposed_once) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var s := new OwningStack<Cell>(); "
        "    s.Push(move (new Cell())); "
        "    var ok := s.TryPop(out using var item); "
        "  end; return Cell._n; end Main; end C;"), 0);
}

// TryPop on an EMPTY collection: ok=false, item stays null, `item?.Dispose()` is
// a no-op, no crash, Cell._n correct (0 — nothing was ever constructed).
BEZEL_TEST(out_owned_writeback_889, trypop_empty_null_cond_noop) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var s := new OwningStack<Cell>(); "
        "    var ok := s.TryPop(out var item); "
        "    item?.Dispose(); "
        "    if ok then return 99; end; "
        "  end; return Cell._n; end Main; end C;"), 0);
}

// NOTE: the `move item`-onward-to-an-owned-sink discharge and the #860 dropped-
// item REJECT both depend on the OWNERSHIP pass (transfer / obligation modeling),
// which the codegen JIT harness here intentionally does not run. Those are pinned
// in the sema suite — tests/sema/out_owned_transfer_860_test.cpp
// (trypop_moved_out_accepted, trypop_dropped_rejected, …) — and the move case is
// additionally verified end-to-end through the full `bezelc` pipeline. They are
// therefore not duplicated here (a JIT-harness run without the ownership pass
// would not exercise the same lowering).

// OwningQueue.TryDequeue(out owned var item): the front element is written back
// and disposed exactly once.
BEZEL_TEST(out_owned_writeback_889, trydequeue_out_owned_disposed_once) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var q := new OwningQueue<Cell>(); "
        "    q.Enqueue(move (new Cell())); "
        "    var ok := q.TryDequeue(out var item); "
        "    item?.Dispose(); "
        "  end; return Cell._n; end Main; end C;"), 0);
}

// Regression — a BORROWING Stack<int>.TryPop(out item): the VALUE out param
// receives the popped value (no owned, no disposal). Returns the value 5.
BEZEL_TEST(out_owned_writeback_889, borrow_stack_int_trypop_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  do using var s := new Stack<int>(); s.Push(5); "
        "    var item: int? := null; var ok := s.TryPop(out item); "  // v0.171 (#1336): T?
        "    if ok then return item!; end; return -1; end; "
        "end Main; end C;"), 5);
}

// Regression — a borrowing Stack<Box>.TryPop(out item) where the element is a
// BORROW (no `owned`): item borrows; the real owner disposes the Box exactly
// once at scope end (no reclaim / no double-free at the borrow's site). Returns
// the borrowed value 7, with the owner's destructor running once.
BEZEL_TEST(out_owned_writeback_889, borrow_stack_class_trypop_no_disposal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box public static _n: int := 0; public _v: int; "
        "  public constructor(v: int) _v := v; Box._n := Box._n + 1; end constructor; "
        "  destructor Box._n := Box._n - 1; end destructor; end Box; "
        "class C public static method Main(): int "
        "  do using var owner := new OwningStack<Box>(); "
        "    owner.Push(move (new Box(7))); "
        "    var b := owner.Peek(); "
        "    using var s := new Stack<Box>(); s.Push(b); "
        "    var item: Box? := null; var ok := s.TryPop(out item); "
        "    var r := 0; if item <> null then r := item._v; end; "
        "    if Box._n <> 1 then return 100; end; "   // exactly one Box alive
        "    return r; end; "                          // 7
        "end Main; end C;"), 7);
}

// Regression — Weak<T>.TryUpgrade(out using var h) still binds a Shared<T>
// HANDLE released via bz_shared_release (the sharedHandle path is preserved for
// a Weak receiver). The upgraded handle reads the underlying value (42) and the
// last handle's disposal disposes the Cell exactly once.
BEZEL_TEST(out_owned_writeback_889, weak_tryupgrade_out_using_var_preserved) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var sh := new Shared<Cell>(new Cell()); "
        "    using var w := sh.Downgrade(); "
        "    var ok := false; "
        "    if w.TryUpgrade(out using var h) then ok := true; end; "
        "    if not ok then return 100; end; "
        "  end; return Cell._n; end Main; end C;"), 0);
}
