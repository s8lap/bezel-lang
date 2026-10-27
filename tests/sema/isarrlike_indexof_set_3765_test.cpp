// Spec-derived regression test for issue #3765 (stdlib §4.6/§4.7/§4.10/§4.11,
// v0.287).
//
// `IndexOf(item)` is declared only by `Array<T>` (§4.4) and `OwningArray<T>`
// (§4.5); every OTHER isArrLike family — `Stack`/`OwningStack`/`Queue`/
// `OwningQueue`/`Set`/`OwningSet`/`LinkedList`/`OwningLinkedList`/`SortedSet`
// (§§4.6/4.7/4.10/4.11/4.18) — does NOT declare it. The isArrLike
// IndexOf-typing arm in ExprTypecheck_Calls.cpp (the `m == "IndexOf" -> int`
// arm at the head of the `if (isArrLike)` block) previously typed EVERY
// isArrLike head as `int`, so a `Set<int>.IndexOf(1)` call acquired a type
// sema and reached codegen, where there is no dispatch arm for any of those
// eight receivers — the call's result slot was zero-filled via the #2434
// degrade (the "loud -> quiet" defect class §6c rejects). The fix gates the
// typing to `Array`/`OwningArray` and emits a precise "no member `IndexOf`
// on `<Type><T>`" diagnostic on the call line for every other head,
// matching the SortedSet reject wording at ExprTypecheck_Calls.cpp's
// sortedSetMemberUndeclared arm. This file pins that behavior:
//
//   - Row 1 (the issue's own repro): `Set<int>.IndexOf(1)` is rejected with
//     the new "no member `IndexOf` on `Set<T>`" diagnostic.
//   - Row 2 (control — the array family that DOES declare IndexOf): no
//     regression on `Array<int>.IndexOf(1)` (still types, runs, returns the
//     matched index) — proves the fix is NARROW, not a blanket IndexOf
//     reject that would silently break every array program.
//   - Row 3-5 (the OTHER non-array isArrLike families): `Stack<int>/
//     Queue<int>/LinkedList<int>.IndexOf(1)` are rejected with the same
//     "no member `IndexOf`" wording — the precise head name surfaces so a
//     user greps "no member `IndexOf`" and lands all five.
//   - Row 6 (the owning twin): `OwningSet<R>.IndexOf(r)` is rejected with
//     the same wording — proves the fix covers the OWNING side of the
//     isArrLike family too, not just the borrowing side.
//
// Citations verified verbatim against docs/Bezel_Standard_Library.md:
//   - stdlib §4.4 `Array<T>`: `public method IndexOf(item: T, startIndex: int := 0): int;`
//   - stdlib §4.5 `OwningArray<T>`: `public method IndexOf(item: T): int;`
//   - stdlib §4.6 `LinkedList<T>`: no `IndexOf` declared;
//     declared members are `AddFirst/AddLast/AddBefore/AddAfter/RemoveFirst/
//     RemoveLast/RemoveNode/Find` plus the §4.1 `ICollection<T>` operations.
//   - stdlib §4.7 `OwningLinkedList<T>`: no `IndexOf` declared.
//   - stdlib §4.10 `Set<T>`/`OwningSet<T>`: no `IndexOf`
//     declared; declared members are `Add/TryAdd/UnionWith/IntersectWith/
//     ExceptWith/IsSubsetOf/IsSupersetOf` (Set) plus `Pop/TryPop/Drain` and
//     the §4.2 `IOwningCollection<T>` operations (OwningSet).
//   - stdlib §4.11 `Stack<T>`/`OwningStack<T>`/`Queue<T>`/`OwningQueue<T>`:
//     no `IndexOf` declared on any of the four.
//   - stdlib §4.18 `SortedSet<T>`: no `IndexOf` declared
//     (already pinned by `sortedset_isarrlike_3730_test.cpp`).
//
// Mechanical verification (per [[regression-test-pins-the-bug]]): the REJECT
// rows return zero at runtime with PRE-FIX compiler and exit 1 with
// POST-FIX compiler; the ACCEPT row runs identically both ways. The
// diagnostic-text assertions are the load-bearing discriminator (per §3
// of the lane rules: assert diagnostic TEXT, not hasErrors() alone).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// True iff some reported ERROR diagnostic carries `needle`. We grep the
// message text, not the diagnostic category — the precise wording is the
// regression-pin.
bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ===========================================================================
// Row 1 — the issue's own repro: `Set<int>.IndexOf(1)`.
// PRE-FIX: typed as int via the unconditional isArrLike arm, codegen has
// no dispatch arm, slot zero-fills via #2434 (silent zero). POST-FIX: a
// precise "no member `IndexOf` on `Set<T>`" diagnostic on the call line.
// ===========================================================================
BEZEL_TEST(isarrlike_indexof_set_3765, set_indexof_rejected_not_declared) {
    const std::string src =
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var s := new Set<int>();\n"
        "    s.Add(1);\n"
        "    var i := s.IndexOf(1);\n"   // no member on Set<T> (§4.10)
        "    Console.WriteLine(i.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "no member `IndexOf` on `Set<T>`"));
}

// ===========================================================================
// Row 2 — control: `Array<int>.IndexOf(1)` STILL WORKS. Array<T> (§4.4)
// declares `IndexOf(item: T): int`; OwningArray<T> (§4.5) does too. The
// fix is NARROW — it only restricts the OTHER 8 isArrLike families, not
// the two array families. The row runs and prints the matched index,
// proving no regression on the working surface.
// ===========================================================================
BEZEL_TEST(isarrlike_indexof_set_3765, array_indexof_still_works) {
    const std::string src =
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<int>();\n"
        "    a.Add(10); a.Add(20); a.Add(30);\n"
        "    var i := a.IndexOf(20);\n"
        "    Console.WriteLine(i.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(runAotOut(src), "1");
}

// ===========================================================================
// Row 3 — control: `Stack<int>.IndexOf(1)` is REJECTED. Stack<T> (§4.11)
// declares no `IndexOf` member (its surface is Push/Contains/Pop/Peek/TryPop/
// TryPeek/Clear). The diagnostic carries the precise head name so the user
// sees exactly which collection type rejected the call.
// ===========================================================================
BEZEL_TEST(isarrlike_indexof_set_3765, stack_indexof_rejected_not_declared) {
    const std::string src =
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var s := new Stack<int>();\n"
        "    s.Push(1);\n"
        "    var i := s.IndexOf(1);\n"   // no member on Stack<T> (§4.11)
        "    Console.WriteLine(i.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "no member `IndexOf` on `Stack<T>`"));
}

// ===========================================================================
// Row 4 — control: `Queue<int>.IndexOf(1)` is REJECTED. Queue<T> (§4.11)
// declares no `IndexOf` member (its surface is Enqueue/Dequeue/Peek/
// Contains/TryDequeue/TryPeek/Clear).
// ===========================================================================
BEZEL_TEST(isarrlike_indexof_set_3765, queue_indexof_rejected_not_declared) {
    const std::string src =
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var q := new Queue<int>();\n"
        "    q.Enqueue(1);\n"
        "    var i := q.IndexOf(1);\n"   // no member on Queue<T> (§4.11)
        "    Console.WriteLine(i.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "no member `IndexOf` on `Queue<T>`"));
}

// ===========================================================================
// Row 5 — control: `LinkedList<int>.IndexOf(1)` is REJECTED. LinkedList<T>
// (§4.6) declares no `IndexOf` member (its surface is AddFirst/AddLast/
// AddBefore/AddAfter/RemoveFirst/RemoveLast/RemoveNode/Find plus the
// §4.1 `ICollection<T>` operations).
// ===========================================================================
BEZEL_TEST(isarrlike_indexof_set_3765, linkedlist_indexof_rejected_not_declared) {
    const std::string src =
        "module M;\n"
        "import System;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var l := new LinkedList<int>();\n"
        "    l.AddLast(1);\n"
        "    var i := l.IndexOf(1);\n"   // no member on LinkedList<T> (§4.6)
        "    Console.WriteLine(i.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(
        hasErrorContaining(r, "no member `IndexOf` on `LinkedList<T>`"));
}

// ===========================================================================
// Row 6 — control: `OwningSet<R>.IndexOf(r)` is REJECTED. OwningSet<T>
// (§4.10) declares no `IndexOf` member. The row uses a class element R so
// the OwningSet instantiation is well-formed (OwningSet requires `T:
// class`); the IndexOf argument is a freshly-constructed R so the call has
// no other diagnostic competing for the call line (the unused-R auto-drop
// diagnostic that follows the call site is irrelevant to the IndexOf row —
// what matters is that the IndexOf diagnostic fires first, on the call
// line, with the precise head name `OwningSet<T>`).
// ===========================================================================
BEZEL_TEST(isarrlike_indexof_set_3765,
           owningset_indexof_rejected_not_declared) {
    const std::string src =
        "module M;\n"
        "import System;\n"
        "class R\n"
        "  public X: int := 0;\n"
        "  destructor end destructor;\n"
        "end R;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var s := new OwningSet<R>();\n"
        "    var q := new R();\n"
        "    var i := s.IndexOf(q);\n"   // no member on OwningSet<T> (§4.10)
        "    Console.WriteLine(i.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(
        hasErrorContaining(r, "no member `IndexOf` on `OwningSet<T>`"));
}

}  // namespace
