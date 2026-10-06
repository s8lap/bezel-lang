// Regression tests for issue #2472 — the `for x in stack do ... end`
// desugaring's direct-indexed-loop optimization (LlvmBackend_Stmts.cpp
// `lowerArrayFor`'s `isCollectionTypeName` block) visited a `Stack<T>` in
// physical storage order (index 0 upward — bottom-to-top / insertion order)
// instead of the order stdlib §4.11 "Iteration order" (v0.195) requires:
//
//   "All four types iterate (via IReadOnlyCollection<T>) in take order — a
//    stack top-to-bottom (pop order), a queue front-to-back (dequeue
//    order); the owning forms iterate identically, yielding borrows."
//
// Confirmed live on origin/main before this fix:
//
//   var s := new Stack<int>(); s.Push(1); s.Push(2); s.Push(3);
//   var n := 0;
//   for var x in s do n := n * 10 + x; end;
//   return n;
//
// returned 123 (bottom-to-top, insertion order) instead of the required 321
// (top-to-bottom, pop order) — silently wrong, no diagnostic.
//
// Normative order established from stdlib §4.11 (quoted above) and language
// spec §29.1 (`IEnumerable<T>`/`IEnumerator<T>` — "Calling GetEnumerator()
// produces a fresh enumerator positioned just before the first element")
// plus §29.2's desugaring (`while __enumerator.MoveNext() do var x :=
// __enumerator.Current; ... end`) and its "Compiler optimization" clause:
// eliding the enumerator construction is sanctioned ONLY "provided the
// observable behavior is identical" to that desugared form — i.e. the
// optimized direct-indexed loop must visit elements in exactly the order a
// real GetEnumerator()/MoveNext()/Current walk would, which for `Stack<T>`
// is §4.11's stated top-to-bottom (pop) order, not physical storage order.
//
// `Queue<T>` was VERIFIED, not assumed, to already be correct: its required
// front-to-back (dequeue) order coincides with physical/insertion order
// (front = index 0, the same index `Dequeue()` itself reads,
// LlvmBackend_Calls.cpp ~4566-4588) — a direct probe against origin/main
// confirmed `Enqueue(1);Enqueue(2);Enqueue(3); for x in q ...` already
// visited 1,2,3 (correct) both before and after this fix, so `Queue<T>` is
// untouched by this change.
//
// Fix scope: `for x in coll do ... end` (LlvmBackend_Stmts.cpp
// `lowerArrayFor`'s `isCollectionTypeName` block) is the ONLY codegen path
// affected — a SEPARATE path (explicit `.GetEnumerator()` calls,
// LlvmBackend_Calls2.cpp ~4618) does not yet functionally implement
// GetEnumerator for Stack<T> at all (filed as #2471, not fixed here); once
// #2471 lands, that path's element order must be made to AGREE with this
// fix's for-loop order (both derive from the same §4.11 contract) rather
// than independently reproducing physical-storage order — noted in #2471.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── The core repro, SEQUENCE-asserting (not sum/membership) ────────────

// Push(1);Push(2);Push(3) → physical [1,2,3] (index0=bottom=1,
// index2=top=3). §4.11 top-to-bottom (pop) order visits 3, 2, 1. Encoding
// via `n := n*5+x` (base 5, safely below the WEXITSTATUS 8-bit exit-code
// mask — per tests/README.md, runAot's exitCode must never be asserted
// above 255, so a base-10 three-digit encoding like `n*10+x` would silently
// wrap: 321 truncates to 65) makes the assertion order-sensitive: forward
// (bottom-to-top) order gives base-5 digits [1,2,3] = 1*25+2*5+3 = 38,
// pop order gives digits [3,2,1] = 3*25+2*5+1 = 86 — a test that merely
// summed or counted elements would pass under EITHER order and would not
// have caught the original bug.
BEZEL_TEST(stack_iteration_order_2472, stack_visits_top_to_bottom_pop_order_stdlib_4_11) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var n := 0; "
        "  for var x in s do n := n * 5 + x; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// Two-element variant, avoiding any risk of the 8-bit exit-code mask
// (WEXITSTATUS) hiding a wraparound coincidence: Push(3);Push(7) → pop
// order is 7,3 → n = 73. A bottom-to-top bug would give 37.
BEZEL_TEST(stack_iteration_order_2472, stack_two_elements_pop_order_sequence) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(3); s.Push(7); "
        "  var n := 0; "
        "  for var x in s do n := n * 10 + x; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 73);
}

// Direct proof that for-loop order equals `Pop()` order — the strongest
// available evidence, since `Pop()` is unambiguous (it is the very
// definition of "top", LlvmBackend_Calls.cpp's `m == "Pop"` reads index
// Count-1) and independent of this fix's own for-loop lowering. Iterate
// via `for`, capture the sequence; separately drain via `Pop()`, capture
// that sequence; the two sequences must match element-for-element.
BEZEL_TEST(stack_iteration_order_2472, stack_for_loop_order_matches_pop_order_directly) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); s.Push(40); "
        "  using var viaIter := new Array<int>(0); "
        "  for var x in s do viaIter.Add(x); end; "
        "  using var viaPop := new Array<int>(0); "
        "  while s.Count > 0 do viaPop.Add(s.Pop()); end; "
        "  if viaIter.Count <> viaPop.Count then return 1; end; "
        "  var i := 0; "
        "  while i < viaIter.Count do "
        "    if viaIter[i] <> viaPop[i] then return 2; end; "
        "    i := i + 1; "
        "  end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// `OwningStack<T>`'s default (borrowing) `IReadOnlyCollection<T>`
// conformance "iterate[s] identically" per §4.11 ("the owning forms iterate
// identically, yielding borrows") — same pop-order requirement, VALUE- AND
// SEQUENCE-correct via the Tag field accumulation (base-5 encoding, see the
// comment on the borrowing-Stack case above for why: keeps the result
// safely under the WEXITSTATUS 8-bit exit-code mask).
BEZEL_TEST(stack_iteration_order_2472, owningstack_visits_top_to_bottom_pop_order) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  using var s := new OwningStack<Res>(); "
        "  s.Push(new Res(1)); s.Push(new Res(2)); s.Push(new Res(3)); "
        "  var n := 0; "
        "  for var x in s do n := n * 5 + x.Tag; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// Empty stack — zero iterations either way; guards the reversed loop's
// initial index (`Count - 1` when `Count == 0` is `-1`) against an
// off-by-one that would otherwise run one spurious iteration or fault.
BEZEL_TEST(stack_iteration_order_2472, empty_stack_zero_iterations) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  var n := 0; "
        "  for var x in s do n := n + 1000; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// Single-element stack — the boundary between "empty" and "general case":
// starting index Count-1 == 0 must run exactly one iteration, not zero.
BEZEL_TEST(stack_iteration_order_2472, single_element_stack_one_iteration) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(9); "
        "  var n := 0; "
        "  for var x in s do n := n + x; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 9);
}

// ── `Queue<T>` confirmed UNAFFECTED (verified, not assumed) ─────────────

// Front-to-back (dequeue) order coincides with physical/insertion order for
// a queue, so this fix's Stack-only reversal must not touch it.
BEZEL_TEST(stack_iteration_order_2472, queue_still_visits_front_to_back_dequeue_order) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "  var n := 0; "
        "  for var x in q do n := n * 10 + x; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 123);
}

// `OwningQueue<T>` — same non-regression guard for the owning form.
BEZEL_TEST(stack_iteration_order_2472, owningqueue_still_visits_front_to_back_dequeue_order) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  using var q := new OwningQueue<Res>(); "
        "  q.Enqueue(new Res(1)); q.Enqueue(new Res(2)); q.Enqueue(new Res(3)); "
        "  var n := 0; "
        "  for var x in q do n := n * 10 + x.Tag; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 123);
}

// ── §29.12 mutation-during-iteration guard unaffected by the reversal ───

// The mod-epoch check (LlvmBackend_Stmts.cpp's `getModFn`/`emitGuardThrow`)
// runs identically regardless of scan direction; this guards that the
// reversed loop's cond-block still re-checks the epoch on every visit.
// tests/codegen/collection_concurrent_mod_1232_test.cpp has no Stack case
// at all (checked directly — Array/Dictionary/LinkedList/Set only), so this
// is a genuinely new guard, not a duplicate.
BEZEL_TEST(stack_iteration_order_2472, stack_mutation_during_iteration_still_throws_spec_29_12) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var s := new Stack<int>(); s.Push(1); s.Push(2); s.Push(3); "
        "  try "
        "    for var x in s do s.Push(99); end; "
        "  catch InvalidOperationException => return 42; "
        "  end; "
        "  return 1; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// Normal iteration (no mutation) must NOT spuriously throw after the
// reversal — the epoch is captured once before the (now-reversed) loop and
// must not itself be treated as a mutation.
BEZEL_TEST(stack_iteration_order_2472, stack_normal_iteration_no_spurious_throw) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var n := 0; "
        "  for var x in s do n := n + x; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 6);
}
