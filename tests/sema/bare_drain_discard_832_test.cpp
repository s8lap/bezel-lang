// fix #832 / §21.9: a bare expression statement that DISCARDS the owned element
// returned by an owning-collection DRAIN (OwningArray.PopFirst/PopLast/PopAt,
// OwningStack.Pop, OwningQueue.Dequeue, OwningLinkedList.PopFirst/PopLast,
// OwningDictionary.PopValue) silently leaked the popped resource. §21.9 makes
// discarding a non-trivial-dispose owned value a hard error. The discard check
// previously missed these because the drains are synthetic builtins with no user
// MethodDecl (so the existing returnOwned gate never fired). Get/Peek BORROW the
// element and must remain accepted.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class R public static n: int := 0; "
        "  destructor R.n := R.n + 1; end destructor; end R; "
        "class C ") + body + " end C;";
}

// True iff some diagnostic mentions the §21.9 owned-discard rule.
bool has219Error(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("§21.9") != std::string::npos ||
            d.message.find("discarding it as a statement leaks") !=
                std::string::npos)
            return true;
    return false;
}

}  // namespace

// OwningArray.PopFirst() as a bare statement leaks → §21.9 error.
BEZEL_TEST(bare_drain_discard_832, owned_array_pop_first_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public static method Main(): int "
        "  using var a := new OwningArray<R>(); a.Add(move (new R())); "
        "  a.PopFirst(); return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(has219Error(r));
}

// OwningStack.Pop() as a bare statement leaks → §21.9 error.
BEZEL_TEST(bare_drain_discard_832, owned_stack_pop_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public static method Main(): int "
        "  using var s := new OwningStack<R>(); s.Push(move (new R())); "
        "  s.Pop(); return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(has219Error(r));
}

// OwningQueue.Dequeue() as a bare statement leaks → §21.9 error.
BEZEL_TEST(bare_drain_discard_832, owned_queue_dequeue_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public static method Main(): int "
        "  using var q := new OwningQueue<R>(); q.Enqueue(move (new R())); "
        "  q.Dequeue(); return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(has219Error(r));
}

// OwningLinkedList.PopLast() as a bare statement leaks → §21.9 error.
BEZEL_TEST(bare_drain_discard_832, owned_linkedlist_pop_last_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "public static method Main(): int "
        "  using var l := new OwningLinkedList<R>(); l.AddLast(move (new R())); "
        "  l.PopLast(); return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(has219Error(r));
}

// Binding the drain result via `using var _` discharges the obligation → OK.
BEZEL_TEST(bare_drain_discard_832, using_var_discard_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public static method Main(): int "
        "  using var a := new OwningArray<R>(); a.Add(move (new R())); "
        "  using var _ := a.PopFirst(); return 0; "
        "end Main;"));
    BEZEL_EXPECT_FALSE(has219Error(r));
}

// Binding the drain result to a named using-var → OK.
BEZEL_TEST(bare_drain_discard_832, bind_named_using_var_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public static method Main(): int "
        "  using var a := new OwningArray<R>(); a.Add(move (new R())); "
        "  using var x := a.PopFirst(); return 0; "
        "end Main;"));
    BEZEL_EXPECT_FALSE(has219Error(r));
}

// A borrowing access (Get) returns a borrow, not an owned value → not rejected.
BEZEL_TEST(bare_drain_discard_832, borrowing_get_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "public static method Main(): int "
        "  using var a := new OwningArray<R>(); a.Add(move (new R())); "
        "  a.Get(0); return 0; "
        "end Main;"));
    BEZEL_EXPECT_FALSE(has219Error(r));
}
