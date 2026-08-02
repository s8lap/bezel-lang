// Regression tests for issue #2487 — the nested covariant view factory
// `Emitter::ensureIEnumViewFactory` (src/codegen/LlvmBackend_Assign.cpp) was
// the FOURTH distinct codegen path still iterating a `Stack<T>`/`OwningStack<T>`
// in physical (bottom-to-top / insertion) order instead of stdlib §4.11
// "Iteration order" (v0.195)'s required top-to-bottom (pop) order. #2486 fixed
// only the LEAF (non-nested) branch of `wrapAsIEnumerableForTarget`; the
// nested-composition family (`wrapNestedArrayAsIEnumerable` →
// `ensureIEnumViewFactory`) was Stack-order-agnostic in TWO independent ways,
// both confirmed live by direct probe (base-5 digit accumulation,
// `n := n*5+x`) before this fix:
//
//   * Defect A — a Stack<T> OUTER container's traversal stayed forward. The
//     nested MoveNext/GetEnumerator hardcoded a forward cursor (idx = -1,
//     idx++, idx < len) regardless of whether `storedTy` (the outer static
//     type) was Stack/OwningStack. `Stack<Array<int>>` with Push(a1=[1]);
//     Push(a2=[2]) iterated a1 then a2 (forward -> n = 1*5+2 = 7) instead of
//     the §4.11 pop order a2 then a1 (n = 2*5+1 = 11).
//
//   * Defect B — a Stack<T> ELEMENT's per-item leaf wrap stayed forward. The
//     leaf branch (~bz$viewfac$P$...) always built via
//     `ensureArrayEnumMachinery` (forward-only), regardless of whether the
//     per-element backing (`storedElem`, recursed in as the leaf's storedTy)
//     was a `Stack<T>`. `Array<Stack<int>>` with one element s = Push(1);
//     Push(2); Push(3) iterated 1,2,3 (forward -> n = 1*25+2*5+3 = 38) instead
//     of the §4.11 pop order 3,2,1 (n = 3*25+2*5+1 = 86).
//
// This fix threads `storedTy`'s head name (Stack/OwningStack) through BOTH
// recursion levels of `ensureIEnumViewFactory`, selecting #2471's existing
// reverse machinery (`ensureStackEnumMachinery`) at the leaf and a
// reverse-outer nested cursor (idx = len, idx--, idx >= 0) at the nested
// level — no third implementation; the struct shape and Dispose/curInner
// contract are unchanged, only the cursor direction and the runtime
// before-top sentinel (bz_array_len) differ, exactly mirroring
// `wrapStackAsIEnumerator`. Per-level cache keys carry an "S" tag so a
// Stack-backed and an Array-backed level (e.g. Stack<Array<int>> vs
// Array<Array<int>>) get DISTINCT factories/vtables even when they share the
// same inner leaf factory.
//
// Ownership: this nested wrap is a plain BORROW of the backing collection
// (mirrors `wrapArrayAsIEnumerable` / `wrapNestedArrayAsIEnumerable`'s
// existing contract, unchanged by this fix). The Dispose path still calls the
// live inner view's slot-1 Dispose then frees the wrapper struct; only the
// outer cursor direction changed, not the disposal wiring. A pre-existing,
// unrelated leak (the borrowed wrapper STRUCT this factory `calloc`s is never
// freed on the argument/local-decl positions — tracked by #2298, identical for
// a plain `Array<Array<int>>` control both before and after this fix) is NOT
// this issue's concern and is not asserted against here, matching the
// convention in tests/codegen/set_as_ienumerable_1069_test.cpp and the #2486
// suite (`run()`, not a leak-checking variant, for the borrowing-argument
// shape). The bug is ORDER, verified element-by-element below.
//
// Test shapes: the issue's canonical repros are used directly — Defect A as a
// Stack<Array<int>> outer (Array elements are BORROWED cleanly by Stack<T>.Push,
// no ownership friction), Defect B as a single-element Array<Stack<int>> (the
// issue's own shape). Shapes that put MULTIPLE owned `Stack<int>` handles in a
// single `Array<Stack<int>>` (e.g. a two-element Array<Stack<int>>, or a
// 3-level Stack<Array<Stack<int>>>) are EXCLUDED here: they hit a separate,
// pre-existing ownership bug in `Array<Stack<int>>` itself (a direct for-in
// over the SAME collection, on a path this fix does not touch, reproduces the
// same garbage value), which is orthogonal to this ORDER fix and is filed
// separately. The 3-level case below uses Stack<Array<Array<int>>> (Array
// elements throughout the inner levels) to exercise the nested-reverse
// machinery at recursion depth without that ownership friction.
//
// Spec: stdlib §4.11 "Iteration order" (v0.195); language spec §29.1 /
// §29.11, v0.236; issue #2487.

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

// Base-5 encoding (n := n*5+x) keeps forward vs pop order unambiguous AND
// safely under the WEXITSTATUS 8-bit mask (tests/README.md): the largest
// value asserted is 86 (< 256).

// ── Defect A: Stack<T> OUTER container's nested traversal ───────────────
//
// `Stack<Array<int>>` -> IEnumerable<IEnumerable<int>>. Outer pop order visits
// a2=[2] then a1=[1] -> n = 2*5+1 = 11. Without the fix (forward outer) the
// value is 7 (a1 then a2 -> 1*5+2). Mirrors the issue's Defect A repro.

BEZEL_TEST(nested_ienumerable_stack_order_2487,
           stack_outer_of_array_elements_pop_order) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<IEnumerable<int>>): int "
        "  var n := 0; "
        "  for var inner in src do "
        "    for var x in inner do n := n * 5 + x; end; "
        "  end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  var outer := new Stack<Array<int>>(); "
        "  using var a1 := new Array<int>(); a1.Add(1); "
        "  using var a2 := new Array<int>(); a2.Add(2); "
        "  outer.Push(a1); outer.Push(a2); "
        "  return C.Sum(outer); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 11);
}

// Three-element outer disambiguates position (not just a pair-reversal):
// Push([1]); Push([2]); Push([3]) -> pop order [3],[2],[1] -> 3*25+2*5+1 = 86.
// Forward order would give 1*25+2*5+3 = 38.

BEZEL_TEST(nested_ienumerable_stack_order_2487,
           stack_outer_three_inner_arrays_pop_order) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<IEnumerable<int>>): int "
        "  var n := 0; "
        "  for var inner in src do "
        "    for var x in inner do n := n * 5 + x; end; "
        "  end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  var outer := new Stack<Array<int>>(); "
        "  using var a1 := new Array<int>(); a1.Add(1); "
        "  using var a2 := new Array<int>(); a2.Add(2); "
        "  using var a3 := new Array<int>(); a3.Add(3); "
        "  outer.Push(a1); outer.Push(a2); outer.Push(a3); "
        "  return C.Sum(outer); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// ── Defect B: Stack<T> ELEMENT's per-item leaf wrap ─────────────────────
//
// `Array<Stack<int>>` -> IEnumerable<IEnumerable<int>>. Outer forward order
// is already correct for Array; the Stack ELEMENT's own leaf wrap must
// iterate pop order. Element s = Push(1);Push(2);Push(3) -> 3,2,1 ->
// n = 3*25+2*5+1 = 86. Without the fix (forward leaf) the value is 38
// (1,2,3 -> 1*25+2*5+3). Mirrors the issue's Defect B repro (single element —
// the canonical shape; see the file header on why multi-Stack-element shapes
// are excluded).

BEZEL_TEST(nested_ienumerable_stack_order_2487,
           array_outer_of_stack_element_pop_order) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<IEnumerable<int>>): int "
        "  var n := 0; "
        "  for var inner in src do "
        "    for var x in inner do n := n * 5 + x; end; "
        "  end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  using var outer := new Array<Stack<int>>(); "
        "  var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  outer.Add(s); "
        "  return C.Sum(outer); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// ── 3-level nesting: Stack outer + nested (non-leaf) inner factory ──────
//
// `Stack<Array<Array<int>>>` -> IEnumerable<IEnumerable<IEnumerable<int>>>:
// exercises Defect A's reverse-outer nested machinery at RECURSION DEPTH —
// the inner factory is itself a NESTED (forward) factory over Array<Array<int>>,
// not a leaf, proving the reverse-outer MoveNext/GetEnumerator cooperate with
// a nested inner Current (the cursor direction reversal is independent of the
// inner factory's kind). Array elements throughout the inner levels avoid the
// unrelated Array<Stack<int>> ownership friction (file header).
//   outer = Push(oa); Push(ob)  -> pop ob then oa (Defect A reversal)
//   ob    = Array of [arr(2)]   -> forward: arr(2)
//   oa    = Array of [arr(1)]   -> forward: arr(1)
//   arr(2)= Array<int> [2]; arr(1) = Array<int> [1]
// Sequence (base-10): 2 then 1 -> n = 2*10+1 = 21.
// Forward outer would visit oa then ob -> 1*10+2 = 12.

BEZEL_TEST(nested_ienumerable_stack_order_2487,
           stack_outer_three_level_nested_inner_forward) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<IEnumerable<IEnumerable<int>>>): int "
        "  var n := 0; "
        "  for var mid in src do "
        "    for var inner in mid do "
        "      for var x in inner do n := n * 10 + x; end; "
        "    end; "
        "  end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  var outer := new Stack<Array<Array<int>>>(); "
        "  using var oa := new Array<Array<int>>(); "
        "  using var ia1 := new Array<int>(); ia1.Add(1); oa.Add(ia1); "
        "  using var ob := new Array<Array<int>>(); "
        "  using var ia2 := new Array<int>(); ia2.Add(2); ob.Add(ia2); "
        "  outer.Push(oa); outer.Push(ob); "
        "  return C.Sum(outer); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 21);
}

// ── Empty nested: zero outer elements -> zero iterations ────────────────

BEZEL_TEST(nested_ienumerable_stack_order_2487,
           empty_stack_outer_zero_iterations) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<IEnumerable<int>>): int "
        "  var n := 0; "
        "  for var inner in src do "
        "    for var x in inner do n := n + 111; end; "
        "  end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  var outer := new Stack<Array<int>>(); "
        "  return C.Sum(outer) + 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Queue<T> outer non-regression: dequeue order coincides with ─────────
// ── forward/physical order, so a Queue<Array<int>> outer must be ─────────
// ── UNCHANGED by this fix (forward, not reversed). ──────────────────────

BEZEL_TEST(nested_ienumerable_stack_order_2487,
           queue_outer_unchanged_forward_order) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<IEnumerable<int>>): int "
        "  var n := 0; "
        "  for var inner in src do "
        "    for var x in inner do n := n * 5 + x; end; "
        "  end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  using var outer := new Queue<Array<int>>(); "
        "  using var a1 := new Array<int>(); a1.Add(1); "
        "  using var a2 := new Array<int>(); a2.Add(2); "
        "  outer.Enqueue(a1); outer.Enqueue(a2); "
        "  return C.Sum(outer); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 7);  // forward: a1 then a2 -> 1*5+2
}

// ── Array<Array<int>> outer non-regression: a plain doubly-nested Array ─
// ── view (no Stack anywhere) must be UNCHANGED (forward at both levels). ─

BEZEL_TEST(nested_ienumerable_stack_order_2487,
           array_of_array_unchanged_forward_order) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<IEnumerable<int>>): int "
        "  var n := 0; "
        "  for var inner in src do "
        "    for var x in inner do n := n * 5 + x; end; "
        "  end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  using var outer := new Array<Array<int>>(); "
        "  using var a1 := new Array<int>(); a1.Add(1); a1.Add(2); "
        "  using var a2 := new Array<int>(); a2.Add(3); "
        "  outer.Add(a1); outer.Add(a2); "
        "  return C.Sum(outer); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 38);  // forward: 1,2 then 3 -> 1*25+2*5+3
}
