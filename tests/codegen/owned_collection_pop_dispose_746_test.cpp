// Spec-derived tests for fix #746 (R41 cluster F) — OwnedStack<T>.Pop() and
// OwnedQueue<T>.Dequeue() transfer the owned element to the caller, so a
// `using var` binding of the result disposes it at scope end.
//
// Authority: stdlib §4.11 — OwnedStack<T>.Pop(): owned T, OwnedQueue<T>.
// Dequeue(): owned T. OwnedArray.PopLast / OwnedLinkedList.PopFirst already
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

// §4.11: a `using var` of OwnedStack.Pop() disposes the popped element.
BEZEL_TEST(owned_collection_pop_746, owned_stack_pop_disposes) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var s := new OwnedStack<R>(); "
        "    s.Push(move (new R())); "
        "    using var top := s.Pop(); "
        "  end; "
        "  return R._n; "  // 1
        "end Main; end C;"), 1);
}

// §4.11: a `using var` of OwnedQueue.Dequeue() disposes the dequeued element.
BEZEL_TEST(owned_collection_pop_746, owned_queue_dequeue_disposes) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var q := new OwnedQueue<R>(); "
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
        "    using var q := new OwnedQueue<R>(); "
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
