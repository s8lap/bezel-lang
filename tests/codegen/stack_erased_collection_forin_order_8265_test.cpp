// Regression tests for issue #8265 — a `for ... in` over a `Stack<T>` bound
// to an ERASED `IReadOnlyCollection<T>` receiver iterated in PUSH order
// (bottom-to-top) instead of the take order stdlib §4.11 (v0.355) requires.
//
// THE AUTHORITY. stdlib §4.11 `Stack<T>`, `OwningStack<T>`, `Queue<T>`,
// `OwningQueue<T>`, "Iteration order" (v0.355) — quoted verbatim:
//
//   "All four types iterate (via `IReadOnlyCollection<T>`) in take order — a
//    stack top-to-bottom (pop order), a queue front-to-back (dequeue order);
//    the owning forms iterate identically, yielding borrows."
//
// THE DEFECT. The CONCRETE receiver is fine: #2472 gave `for x in stack`
// (LlvmBackend_Stmts.cpp `lowerArrayFor`'s `isStackFamily` reversal) the
// required top-to-bottom order, and #2486 gave the `IEnumerable<T>`
// coercion the same. The ERASED receiver was not covered by either: an
// `IReadOnlyCollection<T>`-typed local holds the collection's `bz_array_t*`
// and NOTHING ELSE — the interface slot erased which concrete collection it
// is — so the for-in lowering took the ARRAY-BACKED scan
// (`lowerIEnumerableBuiltinFor`'s `bz_array_len` / `bz_array_ptr_at` walk)
// and visited the stack's PHYSICAL order, bottom-to-top.
//
// MEASURED on the base (a939f913), the issue's own repro:
//
//   using var s := new Stack<int>();
//   s.Push(1); s.Push(2); s.Push(3);
//   var r: IReadOnlyCollection<int> := s;
//   for var x in r do Console.WriteLine($"r{x}"); end for;
//
// printed `r1 r2 r3`; stdlib §4.11 requires `r3 r2 r1` (top-to-bottom).
// Silently wrong, no diagnostic.
//
// THE FIX. The erased scan's ORDER is decided by the BACKING, and the backing
// is only known at RUN TIME — so it is DISPATCHED ON: the runtime's own
// per-instantiation descriptor (`bz_array_t` word 1, stamped at every
// collection construction site by `stampBuiltinTypeDesc` and already read
// back by `bz_array_inst_desc` for `o is Stack<int>`) is compared against
// the `Stack<T>` / `OwningStack<T>` descriptors, and the scan's start index
// and step are selected from that answer: top-first for a stack backing,
// unchanged for every other backing.
//
// COVERAGE (each row is a distinct spelling or a distinct backing):
//   (A) the issue's repro — `IReadOnlyCollection<int>` over a `Stack<int>`;
//   (B) the same, compared element-for-element against `Pop()` (the
//       unambiguous definition of "top", independent of this fix);
//   (C) `OwningStack<Res>` — §4.11's "the owning forms iterate identically";
//   (D) the `Queue<int>` negative — front-to-back must be UNCHANGED;
//   (E) the `Array<int>` negative — forward order must be UNCHANGED;
//   (F) the CONCRETE `Stack<int>` negative — #2472's order must be UNCHANGED;
//   (G) the `IEnumerable<int>` negative — #2486's order must be UNCHANGED;
//   (H) the empty stack — zero iterations (the reversed scan starts at
//       `Count - 1`, i.e. -1, when the stack is empty);
//   (I) a PARAMETER typed `IReadOnlyCollection<int>` (a different erased
//       spelling of the same slot);
//   (J) a FIELD typed `IReadOnlyCollection<int>` (a third);
//   (K) `break` after the first element — the first element visited must be
//       the TOP;
//   (L) `return` from inside the loop body — same;
//   (M) a stack MUTATED after the view was taken — the erased view is a live
//       borrow of the collection, so the loop must see the new top first;
//   (N) the erased scan must NOT drain the stack — the elements are still
//       there afterwards (the scan is a BORROW, §4.11 "yielding borrows");
//   (O) a second `for` over the same erased view REPLAYS (the scan is
//       re-entrant, not a one-shot drain);
//   (P) a GENERIC erased body (`TakeG<T>(r: IReadOnlyCollection<T>)`) over a
//       `Stack<int>` — the erased receiver's element type is a type PARAMETER
//       at the scan site, so the backing's descriptor is only known through
//       the ACTIVE substitution;
//   (Q) the SAME generic body at a SECOND instantiation (`Stack<bool>`) —
//       the substitution must resolve per instantiation, not once for the
//       whole generic;
//   (R) the COVARIANTLY-WIDENED shape (`IReadOnlyCollection<Animal>` over a
//       `Stack<Dog>`) — was a **KNOWN-GAP** row pinned for #8361 (it stated
//       the CURRENT, wrong push order, so it was written to FAIL when #8361
//       was fixed). Its `erased=` expectation was FLIPPED to `3;2;1;` in the
//       commit that fixes #8361, so the row is now GREEN on the fixed tip and
//       RED on the unfixed base.
//
// EVERY ROW PRINTS ITS VISIT ORDER ON ONE LINE (the loop accumulates into a
// string and prints it once), because `Console.WriteLine` appends the
// platform newline and a per-element print would not pin the ORDER.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously.

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The stack: 1 pushed first (bottom), 3 last (top). §4.11's take order for a
// stack is top-to-bottom, so the required visit order is 3, 2, 1.
const char* const kStackPrelude =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var s := new Stack<int>(); "
    "    s.Push(1); s.Push(2); s.Push(3); ";

}  // namespace

// ---------------------------------------------------------------------------
// (A) The #8265 pin — `IReadOnlyCollection<int>` over a `Stack<int>`.
//   stdlib §4.11 "Iteration order" (v0.355): a stack top-to-bottom (pop order)
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_stack_visits_top_to_bottom_4_11) {
    const std::string src =
        std::string(kStackPrelude) +
        "    var r: IReadOnlyCollection<int> := s; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"r{x} \"; end for; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on a939f913: "r1 r2 r3" (physical / push order). §4.11 requires
    // "r3 r2 r1" (top-to-bottom, pop order).
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "r3 r2 r1"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "r1 r2 r3"));
}

// ---------------------------------------------------------------------------
// (B) The strongest available evidence: the erased scan's sequence must be
//     element-for-element the sequence `Pop()` produces — `Pop()` reads index
//     Count-1, so it IS the definition of "top", and it is independent of
//     this fix's own scan.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_stack_matches_pop_order_elementwise_4_11) {
    const std::string src =
        std::string(kStackPrelude) +
        "    var r: IReadOnlyCollection<int> := s; "
        "    using var viaIter := new Array<int>(0); "
        "    for var x in r do viaIter.Add(x); end for; "
        "    using var viaPop := new Array<int>(0); "
        "    while s.Count > 0 do viaPop.Add(s.Pop()); end; "
        "    if viaIter.Count <> viaPop.Count then return 1; end; "
        "    var i := 0; "
        "    while i < viaIter.Count do "
        "      if viaIter[i] <> viaPop[i] then return 2; end; "
        "      i := i + 1; "
        "    end; "
        "    return 42; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix: the iter sequence was 1,2,3 and the pop sequence 3,2,1 → the
    // elementwise compare returned 2.
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ---------------------------------------------------------------------------
// (C) `OwningStack<Res>` — §4.11: "the owning forms iterate identically,
//     yielding borrows" (the elements are BORROWED: the scan must not
//     consume them). An owning collection needs a CLASS element type
//     (stdlib §4.2 / §14.4, #1327), hence `Res`.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_owningstack_visits_top_to_bottom_4_11) {
    const std::string src =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new OwningStack<Res>(); "
        "    s.Push(new Res(1)); s.Push(new Res(2)); s.Push(new Res(3)); "
        "    var r: IReadOnlyCollection<Res> := s; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x.Tag} \"; end for; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "3 2 1"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "1 2 3"));
}

// ---------------------------------------------------------------------------
// (D) The `Queue<int>` negative — §4.11: "a queue front-to-back (dequeue
//     order)". A queue's front-to-back order IS its physical order, so the
//     stack divert must leave it alone.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_queue_still_front_to_back_4_11) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var q := new Queue<int>(); "
        "    q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "    var r: IReadOnlyCollection<int> := q; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x} \"; end for; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "1 2 3"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "3 2 1"));
}

// ---------------------------------------------------------------------------
// (E) The `Array<int>` negative — an array states no take-order contract
//     distinct from storage order, so it must keep visiting forward.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_array_still_forward_order) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    var r: IReadOnlyCollection<int> := a; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x} \"; end for; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "1 2 3"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "3 2 1"));
}

// ---------------------------------------------------------------------------
// (F) The CONCRETE `Stack<int>` negative — #2472's order (the same §4.11
//     contract, reached through the concrete receiver) must be UNCHANGED.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           concrete_stack_forin_keeps_pop_order_4_11) {
    const std::string src =
        std::string(kStackPrelude) +
        "    var acc := \"\"; "
        "    for var x in s do acc := acc + $\"{x} \"; end for; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "3 2 1"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "1 2 3"));
}

// ---------------------------------------------------------------------------
// (G) The `IEnumerable<int>` negative — #2486's coercion order must be
//     UNCHANGED (that path wraps the stack in a take-order enumerator, a
//     different mechanism from this fix's runtime dispatch).
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           ienumerable_over_stack_keeps_pop_order_4_11) {
    const std::string src =
        std::string(kStackPrelude) +
        "    var e: IEnumerable<int> := s; "
        "    var acc := \"\"; "
        "    for var x in e do acc := acc + $\"{x} \"; end for; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "3 2 1"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "1 2 3"));
}

// ---------------------------------------------------------------------------
// (H) The empty stack — zero iterations either way. Guards the reversed
//     scan's initial index (`Count - 1` == -1 when the stack is empty)
//     against an off-by-one that would run one spurious iteration.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_empty_stack_zero_iterations) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new Stack<int>(); "
        "    var r: IReadOnlyCollection<int> := s; "
        "    var n := 0; "
        "    for var x in r do n := n + 1; end for; "
        "    Console.WriteLine($\"count{n}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "count0"));
}

// ---------------------------------------------------------------------------
// (I) A PARAMETER typed `IReadOnlyCollection<int>` — a second spelling of
//     the erased receiver (the argument coercion, not the local-decl one).
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_parameter_over_stack_visits_top_to_bottom_4_11) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Walk(r: IReadOnlyCollection<int>): int "
        "    var n := 0; "
        "    for var x in r do n := n * 5 + x; end for; "
        "    return n; "
        "  end Walk; "
        "  public static method Main(): int "
        "    using var s := new Stack<int>(); "
        "    s.Push(1); s.Push(2); s.Push(3); "
        "    return Walk(s); "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Base-5 encoding of the visit order: pop order (3,2,1) is 3*25+2*5+1 =
    // 86; push order (1,2,3) is 1*25+2*5+3 = 38. Kept under the WEXITSTATUS
    // 8-bit exit-code mask on purpose (tests/README.md).
    BEZEL_EXPECT_EQ(r.exitCode, 86);
}

// ---------------------------------------------------------------------------
// (J) A FIELD typed `IReadOnlyCollection<int>` — a third spelling.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_field_over_stack_visits_top_to_bottom_4_11) {
    const std::string src =
        "module M; "
        "class C "
        "  private _r: IReadOnlyCollection<int>; "
        "  public constructor(s: IReadOnlyCollection<int>) _r := s; end constructor; "
        "  public method Walk(): int "
        "    var n := 0; "
        "    for var x in _r do n := n * 5 + x; end for; "
        "    return n; "
        "  end Walk; "
        "  public static method Main(): int "
        "    using var s := new Stack<int>(); "
        "    s.Push(1); s.Push(2); s.Push(3); "
        "    using var c := new C(s); "
        "    return c.Walk(); "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 86);
}

// ---------------------------------------------------------------------------
// (K) `break` after the first element — the first element visited must be
//     the TOP (§4.11's take order), and the loop must stop there.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_stack_break_visits_top_first_4_11) {
    const std::string src =
        std::string(kStackPrelude) +
        "    var r: IReadOnlyCollection<int> := s; "
        "    var acc := \"\"; "
        "    for var x in r do "
        "      acc := acc + $\"{x} \"; "
        "      break; "
        "    end for; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "3 "));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "2 "));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "1 "));
}

// ---------------------------------------------------------------------------
// (L) `return` from inside the loop body — the element visited before the
//     return must be the TOP.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_stack_return_early_visits_top_first_4_11) {
    const std::string src =
        std::string(kStackPrelude) +
        "    var r: IReadOnlyCollection<int> := s; "
        "    for var x in r do "
        "      Console.WriteLine($\"{x}\"); "
        "      return x; "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "3"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "2"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "1"));
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// (M) A stack MUTATED after the erased view was taken — the view is a live
//     borrow of the collection (§4.1's borrowing interfaces), so the loop
//     must see the new TOP first. (A snapshot view would print 3 2 1 here.)
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_stack_pushed_after_the_view_is_live_4_11) {
    const std::string src =
        std::string(kStackPrelude) +
        "    var r: IReadOnlyCollection<int> := s; "
        "    s.Push(4); "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x} \"; end for; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "4 3 2 1"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "1 2 3 4"));
}

// ---------------------------------------------------------------------------
// (N) The erased scan must NOT drain the stack — §4.11's take order is a
//     BORROWING traversal ("yielding borrows"); the elements must still be
//     in the stack afterwards.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_stack_does_not_drain_4_11) {
    const std::string src =
        std::string(kStackPrelude) +
        "    var r: IReadOnlyCollection<int> := s; "
        "    for var x in r do Console.WriteLine($\"{x}\"); end for; "
        "    Console.WriteLine($\"left{s.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "left3"));
}

// ---------------------------------------------------------------------------
// (O) A second `for` over the SAME erased view REPLAYS the same sequence —
//     the scan is a re-entrant borrow, not a one-shot drain.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_over_stack_second_scan_replays_4_11) {
    const std::string src =
        std::string(kStackPrelude) +
        "    var r: IReadOnlyCollection<int> := s; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"a{x} \"; end for; "
        "    for var x in r do acc := acc + $\"b{x} \"; end for; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "a3 a2 a1 b3 b2 b1"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "a1 a2 a3"));
}

// ---------------------------------------------------------------------------
// (P) A GENERIC erased body — `TakeG<T>(r: IReadOnlyCollection<T>)`. The
//     erased receiver's element type is a type PARAMETER at the scan site,
//     so the backing's concrete instantiation (`Stack<int>`) is only known
//     through the ACTIVE substitution. §4.11's take order applies to the
//     erased receiver exactly as it does to the concrete one.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           generic_erased_body_over_stack_visits_top_to_bottom_4_11) {
    const std::string src =
        "module M; "
        "class C "
        "  static method TakeG<T>(r: IReadOnlyCollection<T>): string "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x};\"; end for; "
        "    return acc; "
        "  end TakeG; "
        "  public static method Main(): int "
        "    using var s := new Stack<int>(); "
        "    s.Push(1); s.Push(2); s.Push(3); "
        "    var r: IReadOnlyCollection<int> := s; "
        "    Console.WriteLine($\"int={TakeG(r)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on a939f913: "int=1;2;3;" (push order). §4.11 requires
    // "int=3;2;1;" (top-to-bottom).
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "int=3;2;1;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "int=1;2;3;"));
}

// ---------------------------------------------------------------------------
// (Q) The SAME generic body at a SECOND instantiation (`Stack<bool>`) — the
//     element type's substitution must resolve per instantiation, not once
//     for the whole generic. (`bool` renders as "True"/"False", so the two
//     orders are unambiguous.)
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           generic_erased_body_at_a_second_instantiation_4_11) {
    const std::string src =
        "module M; "
        "class C "
        "  static method TakeG<T>(r: IReadOnlyCollection<T>): string "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x};\"; end for; "
        "    return acc; "
        "  end TakeG; "
        "  public static method Main(): int "
        "    using var sb := new Stack<bool>(); "
        "    sb.Push(false); sb.Push(true); "
        "    var rb: IReadOnlyCollection<bool> := sb; "
        "    Console.WriteLine($\"bool={TakeG(rb)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on a939f913: "bool=False;True;" (push order). §4.11 requires
    // "bool=True;False;" (top-to-bottom).
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "bool=True;False;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "bool=False;True;"));
}

// ---------------------------------------------------------------------------
// (R) THE COVARIANTLY-WIDENED SHAPE — FLIPPED BY THE #8361 FIX.
//
//     `IReadOnlyCollection<Animal>` over a `Stack<Dog>`. stdlib §4.1 (v0.355)
//     states that "`IReadOnlyCollection<T>` and `IReadOnlyList<T>` are
//     covariant in T (`out T`)", and spec §12.9 (v0.355) admits the
//     assignment, so THIS PROGRAM COMPILES AND RUNS — it is NOT the
//     unrelated-types assignment that §5.1 rejects.
//
//     It was a KNOWN-GAP row (#8361): the erased scan's ORDER was dispatched
//     on the backing's EXACT instantiation (`Stack<Dog>`), while the erased
//     receiver's element type is `Animal` — a DIFFERENT instantiation — so
//     the descriptor compare did not fire and the scan kept the stack's
//     PHYSICAL (push) order. §4.11's "All four types iterate (via
//     `IReadOnlyCollection<T>`) in take order" applies to this shape too, so
//     the erased view and the CONCRETE for-in over the very same stack
//     DISAGREED: `1;2;3;` against `3;2;1;`.
//
//     The fix makes the comparison FAMILY-level ("is the backing a `Stack` /
//     `OwningStack` AT ALL?"), so the covariantly-widened view now agrees
//     with the concrete view. This row's `erased=` expectation is therefore
//     FLIPPED to `3;2;1;` in the same commit that fixes the defect — the row
//     was written to fail when the fix landed, and this is that flip.
// ---------------------------------------------------------------------------
BEZEL_TEST(stack_erased_collection_forin_order_8265,
           erased_iroc_covariantly_widened_over_stack_visits_top_to_bottom_4_11_8361) {
    const std::string src =
        "module M; "
        "class Animal public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Animal; "
        "class Dog: Animal "
        "  public constructor(t: int) base(t); end constructor; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d1 := new Dog(1); "
        "    using var d2 := new Dog(2); "
        "    using var d3 := new Dog(3); "
        "    using var s := new Stack<Dog>(); "
        "    s.Push(d1); s.Push(d2); s.Push(d3); "
        "    var r: IReadOnlyCollection<Animal> := s; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x.Tag};\"; end for; "
        "    Console.WriteLine($\"erased={acc}\"); "
        "    var acc2 := \"\"; "
        "    for var d in s do acc2 := acc2 + $\"{d.Tag};\"; end for; "
        "    Console.WriteLine($\"concrete={acc2}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // After the #8361 fix: the erased, covariantly-widened view visits in TAKE
    // order (§4.11) — the same order the CONCRETE for-in over the same stack
    // visits, which is the whole point of the row.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=3;2;1;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "erased=1;2;3;"));
    // The CONCRETE for-in over the same stack visits in TAKE order (§4.11) —
    // the erased view must AGREE with it.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "concrete=3;2;1;"));
}
