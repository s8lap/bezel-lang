// Sema -- `Remove` on Stack<T>, Queue<T>, OwningStack<T>, OwningQueue<T> is a
// "no member" compile error (#6163).
//
// QUOTED stdlib (docs/Bezel_Standard_Library.md, v0.338, §4.11): the member
// lists of the four classes are
//   Stack<T>:       Push, Contains, Pop, Peek, TryPop, TryPeek, Clear
//   OwningStack<T>: Push, Contains, Pop, Peek, TryPop, TryPeek, Clear, Drain
//   Queue<T>:       Enqueue, Dequeue, Peek, Contains, TryDequeue, TryPeek, Clear
//   OwningQueue<T>: Enqueue, Contains, Dequeue, Peek, TryDequeue, TryPeek,
//                   Clear, Drain
// and all four are `IReadOnlyCollection<T>` (#1354), not `ICollection<T>`:
// none declares `Remove`. Stdlib §4.1 gives `Remove(item: T): bool` to
// ICollection<T> only (and §4.4/§4.5 to Array/OwningArray).
//
// Pre-fix the generous member-surface union admitted the name `Remove`
// (a sibling family has it), so `s.Remove(1)` compiled and was dropped at
// lowering with warning #3079. Post-fix it is the same "no member" error
// `s.Frob()` already draws.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

std::string prog(const std::string& decl, const std::string& stmt) {
    return "module M; "
           "class Cell public V: int := 0; public constructor(v: int) V := v; end constructor; end Cell; "
           "class C public static method Main() " + decl + " " + stmt +
           " end Main; end C;";
}

}  // namespace

BEZEL_TEST(stack_queue_remove_no_member_6163, stack_remove_rejected) {
    auto r = parseSemaAll(prog("using var s := new Stack<int>(); s.Push(1);",
                               "s.Remove(1);"));
    BEZEL_EXPECT_TRUE(hasError(r, "no member `Remove` on `Stack<T>`"));
}

BEZEL_TEST(stack_queue_remove_no_member_6163, stack_remove_var_rejected) {
    auto r = parseSemaAll(prog("using var s := new Stack<int>(); s.Push(1);",
                               "var x := s.Remove(1);"));
    BEZEL_EXPECT_TRUE(hasError(r, "no member `Remove` on `Stack<T>`"));
}

BEZEL_TEST(stack_queue_remove_no_member_6163, queue_remove_rejected) {
    auto r = parseSemaAll(prog("using var q := new Queue<int>(); q.Enqueue(1);",
                               "q.Remove(1);"));
    BEZEL_EXPECT_TRUE(hasError(r, "no member `Remove` on `Queue<T>`"));
}

BEZEL_TEST(stack_queue_remove_no_member_6163, owningstack_remove_rejected) {
    auto r = parseSemaAll(prog("using var s := new OwningStack<Cell>();",
                               "using var c := new Cell(1); s.Remove(c);"));
    BEZEL_EXPECT_TRUE(hasError(r, "no member `Remove` on `OwningStack<T>`"));
}

BEZEL_TEST(stack_queue_remove_no_member_6163, owningqueue_remove_rejected) {
    auto r = parseSemaAll(prog("using var q := new OwningQueue<Cell>();",
                               "using var c := new Cell(1); q.Remove(c);"));
    BEZEL_EXPECT_TRUE(hasError(r, "no member `Remove` on `OwningQueue<T>`"));
}

// Controls: the families that DO declare Remove are unaffected, as are the
// real Stack/Queue members.
BEZEL_TEST(stack_queue_remove_no_member_6163, controls_not_rejected) {
    for (const char* src : {
             "using var s := new Set<int>(); s.Add(1); s.Remove(1);",
             "using var a := new Array<int>(); a.Add(1); a.Remove(1);",
             "using var l := new LinkedList<int>(); l.Add(1); l.Remove(1);",
             "using var s := new Stack<int>(); s.Push(1); s.Pop(); s.Clear();",
             "using var q := new Queue<int>(); q.Enqueue(1); q.Dequeue(); q.Clear();"}) {
        auto r = parseSemaAll(prog("", src));
        BEZEL_EXPECT_FALSE(hasError(r, "no member `Remove`"));
    }
}
