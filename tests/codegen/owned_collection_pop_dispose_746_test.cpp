// Spec-derived tests for fix #746 (R41 cluster F) — OwningStack<T>.Pop() and
// OwningQueue<T>.Dequeue() transfer the owned element to the caller, so a
// `using var` binding of the result disposes it at scope end.
//
// Authority: stdlib §4.11 — OwningStack<T>.Pop(): owned T, OwningQueue<T>.
// Dequeue(): owned T. OwningArray.PopLast / OwningLinkedList.PopFirst already
// demonstrate the pattern.
//
// Pre-fix, Pop/Dequeue were absent from the owned-return disposal
// classification (only PopLast/PopFirst/PopAt/PopValue were listed), so the
// popped element's `using var` disposal was never emitted — a leak. The fix is
// gated on the OWNING receiver: a borrowing Stack/Queue returns a borrow, which
// must NOT be disposed (double-free).
//
// Each test counts a static destructor-increment; the popped element's
// destructor running (and not running twice) is the observable check.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
const char* kR =
    "class R public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; end R; ";
}  // namespace

// §4.11: a `using var` of OwningStack.Pop() disposes the popped element.
BEZEL_TEST(owned_collection_pop_746, owned_stack_pop_disposes) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var s := new OwningStack<R>(); "
        "    s.Push(move (new R())); "
        "    using var top := s.Pop(); "
        "  end; "
        "  return R._n; "  // 1
        "end Main; end C;"), 1);
}

// §4.11: a `using var` of OwningQueue.Dequeue() disposes the dequeued element.
BEZEL_TEST(owned_collection_pop_746, owned_queue_dequeue_disposes) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var q := new OwningQueue<R>(); "
        "    q.Enqueue(move (new R())); "
        "    using var front := q.Dequeue(); "
        "  end; "
        "  return R._n; "  // 1
        "end Main; end C;"), 1);
}

// §4.11: the dequeued element AND the one left in the queue are both disposed
// exactly once (2 total).
BEZEL_TEST(owned_collection_pop_746, dequeue_one_keep_one_disposes_both) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var q := new OwningQueue<R>(); "
        "    q.Enqueue(move (new R())); q.Enqueue(move (new R())); "
        "    using var front := q.Dequeue(); "
        "  end; "
        "  return R._n; "  // 2
        "end Main; end C;"), 2);
}

// §4.11 / safety: a BORROWING Stack returns a BORROW on Pop — the element is
// owned by its using-var owner and disposed exactly once (no double-free).
BEZEL_TEST(owned_collection_pop_746, borrowing_stack_pop_no_double_free) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var owner := new R(); "
        "    using var s := new Stack<R>(); "
        "    s.Push(owner); "
        "    var top := s.Pop(); "
        "  end; "
        "  return R._n; "  // 1 — disposed once by `owner`, NOT by Pop
        "end Main; end C;"), 1);
}

// ===========================================================================
// #1199 — OwningArray<T>.PopAt(index) bounds-checks `index` and raises
// IndexOutOfRangeException when out of range, mirroring the indexer / ElementAt
// and the stdlib §4.5 reference (PopAt calls BoundsCheck(index) first:
// `if i < 0 or i >= _count then throw new IndexOutOfRangeException()`).
// Before the fix the OOB path emitted no index guard and the runtime left the
// owned-out slot unwritten, so the caller read an uninitialized owned T and
// faulted (SIGSEGV) instead of throwing.
// ===========================================================================

namespace {
const char* kCell =
    "class Cell public v: int := 0; "
    "  public constructor(x: int) v := x; end constructor; end Cell; ";
}  // namespace

// §4.5 (#1199): PopAt(10) on a 1-element array is out of bounds → throws a
// CATCHABLE IndexOutOfRangeException (was SIGSEGV before the fix).
BEZEL_TEST(owned_array_popat_1199, ownedarray_popat_oob_throws_1199) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Collections.Generic; ") + kCell +
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Cell>(); "
        "  a.Add(new Cell(5)); "
        "  try "
        "    using var c := a.PopAt(10); "
        "    return c.v; "
        "  catch IndexOutOfRangeException => return 1; "  // caught → pass
        "  end; "
        "end Main; end C;"), 1);
}

// §4.5 (#1199): a negative index is likewise out of bounds → IndexOutOfRange.
BEZEL_TEST(owned_array_popat_1199, ownedarray_popat_negative_throws_1199) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Collections.Generic; ") + kCell +
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Cell>(); "
        "  a.Add(new Cell(5)); "
        "  try "
        "    using var c := a.PopAt(-1); "
        "    return c.v; "
        "  catch IndexOutOfRangeException => return 1; "  // caught → pass
        "  end; "
        "end Main; end C;"), 1);
}

// §4.5 (#1199): PopAt on an EMPTY array (Count 0) is out of bounds for every
// index → BoundsCheck raises IndexOutOfRangeException (not InvalidOperation).
BEZEL_TEST(owned_array_popat_1199, ownedarray_popat_empty_throws_1199) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Collections.Generic; ") + kCell +
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Cell>(); "
        "  try "
        "    using var c := a.PopAt(0); "
        "    return c.v; "
        "  catch IndexOutOfRangeException => return 1; "  // caught → pass
        "  end; "
        "end Main; end C;"), 1);
}

// §4.5 (#1199): the in-bounds control still works — PopAt(1) on [5,7,9] removes
// and returns the owned element at slot 1 (value 7). Guards against an
// over-eager bounds guard rejecting a valid index.
BEZEL_TEST(owned_array_popat_1199, ownedarray_popat_inbounds_ok_1199) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Collections.Generic; ") + kCell +
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Cell>(); "
        "  a.Add(new Cell(5)); a.Add(new Cell(7)); a.Add(new Cell(9)); "
        "  using var c := a.PopAt(1); "
        "  return c.v; "  // 7
        "end Main; end C;"), 7);
}
