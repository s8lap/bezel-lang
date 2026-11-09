// Regression tests for issues #8369 and #8370 — the stdlib §5 PREDICATE
// element terminals (`First(pred)` / `Last(pred)` / `FirstOrNull(pred)` /
// `LastOrNull(pred)`) and the stdlib §5 LAZY operator surface (`Where` /
// `Select` / `Take` / `Skip` / … and every terminal over the result) over a
// CONCRETE `Stack<T>` answered in PUSH order (bottom first), while the
// `for ... in` over the very same stack — and stdlib §4.11 — iterate in TAKE
// order (top first).
//
// THE SPEC (stdlib §4.11, v0.355, line 2489 — "Iteration order"):
//
//   "All four types iterate (via `IReadOnlyCollection<T>`) in take order — a
//    stack top-to-bottom (pop order), a queue front-to-back (dequeue order);
//    the owning forms iterate identically, yielding borrows."
//
// stdlib §5 (v0.355, line 3760) defines the predicate overloads of the
// element terminals as "equivalent to the corresponding no-argument method
// applied after `Where(pred)` — `xs.First(pred)` is `xs.Where(pred).First()`",
// and `First` / `Last` are defined over the receiver's ITERATION order.
// ⇒ over a stack, `First(pred)` is the first match in TAKE order (the TOP
// first) and `Last(pred)` the last one (the BOTTOM-most match); and every
// §5 lazy operator over the stack sees the same take order.
//
// THE DEFECTS (MEASURED on the base, r30-c1 @ 1926ccf7).  A `Stack<T>` is
// bottom-to-top (index 0 is the bottom, index Count-1 is the top — the index
// `Pop()` itself reads), and BOTH families read that backing array BY
// ELEMENT INDEX:
//
//   using var s := new Stack<int>(); s.Push(10); s.Push(20); s.Push(30);
//   for var x in s do Console.WriteLine(x); end for;   // 30 20 10 (take)
//   Console.WriteLine(s.First(lambda(x) => x > 15));  // 20  (push — WRONG)
//   Console.WriteLine(s.Last(lambda(x) => x > 15));   // 30  (push — WRONG)
//   using var w := s.Where(lambda(x) => x > 15).ToArray();
//   for var x in w do Console.WriteLine(x); end for;   // 20 30 (push — WRONG)
//
// AFTER THE FIX: the predicate terminals answer 30 / 20, and the lazy
// pipeline materializes 30, 20 — the same order the iteration of the same
// value visits.
//
// WHAT IS *NOT* CHANGED (the control rows below).  `Array<T>` states no
// take-order contract distinct from storage order — its `First()` IS its
// element 0 — and `Queue<T>` has its own front-to-back (dequeue) order,
// which coincides with its storage order.  Both are measured on the base and
// on the fix, and must be byte-identical.  The stack's NO-ARGUMENT terminals
// (#8276's tier) and its `for ... in` iteration are also unchanged.
//
// WHERE THE FIX LIVES.  `src/codegen/LlvmBackend_Calls3.cpp` only:
//   * `lowerToLazyEnumerator` — the concrete-collection wrap builds the lazy
//     source in TAKE order for the stack family (the source is the stack's
//     array read backwards), so the WHOLE lazy surface and every terminal
//     over it see the take order;
//   * `tryLowerLazyLinqCall` — the §5 dispatch intercepts the PREDICATE tier
//     for the stack family, scanning the backing array from the TOP down.
// The predicate arms themselves live in `LlvmBackend_Calls.cpp` (another
// lane's file); the terminals' faults (InvalidOperationException on no
// match), their nullable form, the §28.11 string-element escape rule and
// the §28.3 unbound-owned-temporary reclaim are unchanged; only the ORDER
// moved.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The program shell every row uses: a module, any extra declarations, and a
// `Main` that returns 0.
std::string prog(const std::string& decls, const std::string& body) {
    return std::string("module M; ") + decls +
           "class C public static method Main(): int " + body +
           "  return 0; end Main; end C;";
}

// The three-element stack the issues' own repros use.  The pushes are 10, 20,
// 30, so the backing array is [10, 20, 30] bottom-to-top and the TAKE order
// (top first) is 30, 20, 10.
std::string stack3(const std::string& body) {
    return prog("",
                "  using var s := new Stack<int>(); "
                "  s.Push(10); s.Push(20); s.Push(30); " + body);
}

// The stdout oracle: the program's captured stdout, byte-for-byte.
std::string out(const std::string& program) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    return r.stdoutText;
}

// The REJECT oracle: the program must not compile, and the diagnostic must
// carry `needle`.  Used for the residual rows — the shapes this fix does NOT
// reach, pinned so that a future fix has to move them deliberately.
bool rejectsWith(const std::string& program, const std::string& needle) {
    JitResult r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The class-element shape: a class instance is a REFERENCE element, a
// different representation from a value element (§28.11's element-slot table).
const char* kBoxDecl =
    "class Box public Tag: string; "
    "  public constructor(tag: string) this.Tag := tag; end constructor; "
    "end Box; ";

}  // namespace

// ===========================================================================
// #8369 — THE PREDICATE TERMINALS.  Red on the base (which answered 20 / 30
// for First(pred) / Last(pred)), green with the fix (30 / 20).
// ===========================================================================

// stdlib §4.11 / §5: `First(pred)` over a concrete `Stack<T>` is the FIRST
// match in the receiver's TAKE order — the TOP-first scan, so it answers 30
// (the top) and not 20 (the bottom-most match).
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           first_pred_is_the_first_match_in_take_order) {
    BEZEL_EXPECT_EQ(
        out(stack3("  Console.WriteLine(s.First(lambda(x) => x > 15)); ")),
        "30\n");
}

// stdlib §4.11 / §5: `Last(pred)` is the LAST match in the TAKE order — the
// bottom-most match, 20.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           last_pred_is_the_last_match_in_take_order) {
    BEZEL_EXPECT_EQ(
        out(stack3("  Console.WriteLine(s.Last(lambda(x) => x > 15)); ")),
        "20\n");
}

// The `*OrNull` tier is the nullable SIBLING of the throwing forms on the
// SAME §5 extension block, over the SAME `IEnumerable<T>`.  They must not
// keep the old (push) order while the throwing forms move.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           ornull_predicate_siblings_agree_with_the_throwing_forms) {
    BEZEL_EXPECT_EQ(
        out(stack3("  Console.WriteLine(s.FirstOrNull(lambda(x) => x > 15) ?? -1); "
                   "  Console.WriteLine(s.LastOrNull(lambda(x) => x > 15) ?? -1); ")),
        "30\n20\n");
}

// THE STRONGEST ROW: the predicate terminals and the iteration of the SAME
// value, side by side.  The iteration is 30, 20, 10 (take order) and the
// terminals must answer the same sequence.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           predicate_terminals_agree_with_the_for_in_iteration) {
    BEZEL_EXPECT_EQ(
        out(stack3("  for var x in s do Console.WriteLine(x); end for; "
                   "  Console.WriteLine(s.First(lambda(x) => x > 15)); "
                   "  Console.WriteLine(s.Last(lambda(x) => x > 15)); ")),
        "30\n20\n10\n30\n20\n");
}

// stdlib §4.11: "the owning forms iterate identically".  `OwningStack<T>` is
// the owning twin of `Stack<T>` and must answer in the same take order.  The
// element is a CLASS here (a reference element, §28.11).
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           owning_stack_predicate_terminals_take_order) {
    const std::string body =
        "  using var s := new OwningStack<Box>(); "
        "  s.Push(new Box(\"one\")); s.Push(new Box(\"two\")); "
        "  s.Push(new Box(\"three\")); "
        "  for var x in s do Console.WriteLine(x.Tag); end for; "
        "  Console.WriteLine(s.First(lambda(x) => x.Tag <> \"one\").Tag); "
        "  Console.WriteLine(s.Last(lambda(x) => x.Tag <> \"three\").Tag); "
        "  Console.WriteLine(s.FirstOrNull(lambda(x) => x.Tag = \"nope\") = null); "
        "  Console.WriteLine(s.LastOrNull(lambda(x) => x.Tag = \"nope\") = null); ";
    BEZEL_EXPECT_EQ(out(prog(kBoxDecl, body)),
                    "three\ntwo\none\nthree\none\nTrue\nTrue\n");
}

// THE ORDER IS A CONTRACT — MEASURE THE ELEMENT TYPE AS WELL AS THE ORDER.  A
// `string` element is a runtime-managed REFERENCE slot (§28.11), so a
// mis-typed or mis-indexed read would show up here as the wrong element.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           string_elements_answer_the_right_matching_element) {
    const std::string body =
        "  using var s := new Stack<string>(); "
        "  s.Push(\"alpha\"); s.Push(\"beta\"); s.Push(\"gamma\"); "
        "  Console.WriteLine(s.First(lambda(x) => x <> \"alpha\")); "
        "  Console.WriteLine(s.Last(lambda(x) => x <> \"gamma\")); "
        "  Console.WriteLine(s.FirstOrNull(lambda(x) => x = \"nope\") ?? \"none\"); ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "gamma\nalpha\nnone\n");
}

// The FAULT RULE is unchanged: stdlib §5 (line 3760) — "`First(pred)` /
// `Last(pred)` … throw `InvalidOperationException` when no element matches".
// A fix that turned the fault into a wrong value (or a crash) is a REJECT.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           no_matching_element_keeps_the_documented_fault) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); "
        "  try Console.WriteLine(s.First(lambda(x) => x > 100)); "
        "  catch InvalidOperationException => Console.WriteLine(\"first\"); end; "
        "  try Console.WriteLine(s.Last(lambda(x) => x > 100)); "
        "  catch InvalidOperationException => Console.WriteLine(\"last\"); end; "
        "  Console.WriteLine(s.FirstOrNull(lambda(x) => x > 100) ?? -1); "
        "  Console.WriteLine(s.LastOrNull(lambda(x) => x > 100) ?? -1); ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "first\nlast\n-1\n-1\n");
}

// ===========================================================================
// #8370 — THE LAZY OPERATOR SURFACE.  Red on the base (which materialized
// 20, 30), green with the fix (30, 20).
// ===========================================================================

// stdlib §4.11 / §5: `Where(pred)` over a concrete `Stack<T>` yields the
// stack's elements in TAKE order, so the materialized array is 30, 20 — the
// two elements above 15, top first.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           where_pipeline_materializes_in_take_order) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); "
        "  using var w := s.Where(lambda(x) => x > 15).ToArray(); "
        "  for var x in w do Console.WriteLine(x); end for; ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "30\n20\n");
}

// A TERMINAL over the lazy pipeline answers in the pipeline's order: the
// first element of `s.Where(x => x > 15)` is the TOP (30), not the
// bottom-most match (20).
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           terminal_over_the_where_pipeline_is_the_top) {
    BEZEL_EXPECT_EQ(
        out(stack3("  Console.WriteLine(s.Where(lambda(x) => x > 15).First()); ")),
        "30\n");
}

// The PROJECTION arm: `Select(f).First()` is the first element of the
// projected take-order sequence — 60 (the top, doubled), not 20.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           select_pipeline_first_is_the_top) {
    BEZEL_EXPECT_EQ(
        out(stack3("  Console.WriteLine(s.Select(lambda(x) => x * 2).First()); ")),
        "60\n");
}

// The SLICING arms: `Take(n)` keeps the first n of the TAKE order and
// `Skip(n)` drops them — 30, 20 and 20, 10.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           take_and_skip_slice_the_take_order) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); "
        "  using var t := s.Take(2).ToArray(); "
        "  for var x in t do Console.WriteLine(x); end for; "
        "  using var k := s.Skip(1).ToArray(); "
        "  for var x in k do Console.WriteLine(x); end for; ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "30\n20\n20\n10\n");
}

// The PROJECTION arm, materialized: `Select(f).ToArray()` is the projected
// TAKE order — 60, 40, 20 — not the projected push order (20, 40, 60).
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           select_pipeline_toarray_is_in_take_order) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); "
        "  using var w := s.Select(lambda(x) => x * 2).ToArray(); "
        "  for var x in w do Console.WriteLine(x); end for; ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "60\n40\n20\n");
}

// The ORDERING arm: stdlib §5's `Reverse()` reverses the receiver's ITERATION
// order, and §4.11 gives a stack its TAKE order (top first) — so the reverse
// of a stack is its PUSH order (bottom first).  This row pins the consequence
// of the fix: `Reverse()` over a stack answers 10, 20, 30.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           the_reverse_operator_reverses_the_take_order) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); "
        "  using var w := s.Reverse().ToArray(); "
        "  for var x in w do Console.WriteLine(x); end for; ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "10\n20\n30\n");
}

// The MATERIALIZER: `ToArray()` over a concrete stack is the §5 operator
// over the receiver's ITERATION order, so it is 30, 20, 10 — the take order.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           toarray_materializes_in_take_order) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); "
        "  using var w := s.ToArray(); "
        "  for var x in w do Console.WriteLine(x); end for; ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "30\n20\n10\n");
}

// A CHAIN of operators stays in the take order at every step.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           a_chained_pipeline_stays_in_take_order) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); "
        "  using var w := s.Where(lambda(x) => x > 15)"
        ".Select(lambda(x) => x * 2).ToArray(); "
        "  for var x in w do Console.WriteLine(x); end for; ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "60\n40\n");
}

// ===========================================================================
// THE CONTROL ROWS — green on the base AND on the fix.  They are the reason a
// wrong-order fix would be loud rather than silent.
// ===========================================================================

// An ARRAY states no take-order contract distinct from storage order: its
// `First(pred)` IS the first match in storage order and its `Last(pred)` the
// last one.  A fix that flipped the array would be a REJECT.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           array_predicate_terminals_keep_storage_order_control) {
    const std::string body =
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  Console.WriteLine(a.First(lambda(x) => x > 15)); "
        "  Console.WriteLine(a.Last(lambda(x) => x > 15)); "
        "  Console.WriteLine(a.FirstOrNull(lambda(x) => x > 15) ?? -1); "
        "  Console.WriteLine(a.LastOrNull(lambda(x) => x > 15) ?? -1); ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "20\n30\n20\n30\n");
}

// A QUEUE has its own front-to-back (dequeue) order, which coincides with its
// storage order — it is already consistent and must not move.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           queue_predicate_terminals_keep_front_first_order_control) {
    const std::string body =
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(10); q.Enqueue(20); q.Enqueue(30); "
        "  Console.WriteLine(q.First(lambda(x) => x > 15)); "
        "  Console.WriteLine(q.Last(lambda(x) => x > 15)); "
        "  Console.WriteLine(q.FirstOrNull(lambda(x) => x > 15) ?? -1); "
        "  Console.WriteLine(q.LastOrNull(lambda(x) => x > 15) ?? -1); ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "20\n30\n20\n30\n");
}

// The ARRAY's LAZY surface keeps its storage order too.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           array_lazy_surface_keeps_storage_order_control) {
    const std::string body =
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  Console.WriteLine(a.Select(lambda(x) => x * 2).First()); "
        "  using var w := a.Where(lambda(x) => x > 15).ToArray(); "
        "  for var x in w do Console.WriteLine(x); end for; "
        "  using var t := a.Take(2).ToArray(); "
        "  for var x in t do Console.WriteLine(x); end for; "
        "  using var k := a.Skip(1).ToArray(); "
        "  for var x in k do Console.WriteLine(x); end for; ";
    BEZEL_EXPECT_EQ(out(prog("", body)),
                    "20\n20\n30\n10\n20\n20\n30\n");
}

// The QUEUE's LAZY surface keeps its front-to-back order too.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           queue_lazy_surface_keeps_front_first_order_control) {
    const std::string body =
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(10); q.Enqueue(20); q.Enqueue(30); "
        "  Console.WriteLine(q.Select(lambda(x) => x * 2).First()); "
        "  using var w := q.Where(lambda(x) => x > 15).ToArray(); "
        "  for var x in w do Console.WriteLine(x); end for; "
        "  using var t := q.Take(2).ToArray(); "
        "  for var x in t do Console.WriteLine(x); end for; "
        "  using var k := q.Skip(1).ToArray(); "
        "  for var x in k do Console.WriteLine(x); end for; ";
    BEZEL_EXPECT_EQ(out(prog("", body)),
                    "20\n20\n30\n10\n20\n20\n30\n");
}

// #8276's tier — the NO-ARGUMENT terminals over a concrete stack — is
// unchanged by this fix (they were already take order), and so is the
// `for ... in` iteration they are measured against.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           stack_no_argument_terminals_and_iteration_are_unchanged_control) {
    BEZEL_EXPECT_EQ(
        out(stack3("  for var x in s do Console.WriteLine(x); end for; "
                   "  Console.WriteLine(s.First()); "
                   "  Console.WriteLine(s.Last()); "
                   "  Console.WriteLine(s.ElementAt(0)); "
                   "  Console.WriteLine(s.ElementAt(2)); "
                   "  Console.WriteLine(s.FirstOrNull() ?? -1); "
                   "  Console.WriteLine(s.LastOrNull() ?? -1); ")),
        "30\n20\n10\n30\n10\n30\n10\n30\n10\n");
}

// An EMPTY stack keeps the documented faults (stdlib §5, line 3760) — the
// predicate forms throw `InvalidOperationException` on no match and the
// `*OrNull` forms answer null.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           empty_stack_keeps_the_documented_faults_control) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  try Console.WriteLine(s.First(lambda(x) => x > 0)); "
        "  catch InvalidOperationException => Console.WriteLine(\"first\"); end; "
        "  try Console.WriteLine(s.Last(lambda(x) => x > 0)); "
        "  catch InvalidOperationException => Console.WriteLine(\"last\"); end; "
        "  Console.WriteLine(s.FirstOrNull(lambda(x) => x > 0) ?? -1); "
        "  Console.WriteLine(s.LastOrNull(lambda(x) => x > 0) ?? -1); "
        "  using var w := s.Where(lambda(x) => x > 0).ToArray(); "
        "  Console.WriteLine(w.Count); ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "first\nlast\n-1\n-1\n0\n");
}

// A ONE-ELEMENT stack has the same first and last, so the take order cannot
// be distinguished from the push order there — the row pins that the fix does
// not change the answer.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           one_element_stack_answers_that_element_control) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  s.Push(42); "
        "  for var x in s do Console.WriteLine(x); end for; "
        "  Console.WriteLine(s.First(lambda(x) => x > 0)); "
        "  Console.WriteLine(s.Last(lambda(x) => x > 0)); "
        "  Console.WriteLine(s.FirstOrNull(lambda(x) => x > 0) ?? -1); "
        "  Console.WriteLine(s.LastOrNull(lambda(x) => x > 0) ?? -1); "
        "  using var w := s.Where(lambda(x) => x > 0).ToArray(); "
        "  for var x in w do Console.WriteLine(x); end for; "
        "  Console.WriteLine(s.Select(lambda(x) => x * 2).First()); ";
    BEZEL_EXPECT_EQ(out(prog("", body)),
                    "42\n42\n42\n42\n42\n42\n84\n");
}

// THE KNOWN GAP, PINNED AS A CONTROL.  The erased, COVARIANTLY-WIDENED
// `IReadOnlyCollection<T>` view of a stack (`IReadOnlyCollection<Animal>`
// over a `Stack<Dog>`) still visits in PUSH order — that is issue #8361's
// territory, NOT this fix's.  This row states the CURRENT (wrong) push
// order, so it is green today and it will go RED the moment #8361's fix
// lands.  That failure is the point: the row is the in-tree marker for the
// flip that #8361's fix must make in the same commit.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           erased_iroc_covariantly_widened_over_stack_still_push_order_8361) {
    const std::string decls =
        "class Animal public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Animal; "
        "class Dog: Animal "
        "  public constructor(t: int) base(t); end constructor; "
        "end Dog; ";
    const std::string body =
        "  using var d1 := new Dog(1); "
        "  using var d2 := new Dog(2); "
        "  using var d3 := new Dog(3); "
        "  using var s := new Stack<Dog>(); "
        "  s.Push(d1); s.Push(d2); s.Push(d3); "
        "  var r: IReadOnlyCollection<Animal> := s; "
        "  var acc := \"\"; "
        "  for var x in r do acc := acc + $\"{x.Tag};\"; end for; "
        "  Console.WriteLine($\"erased={acc}\"); "
        "  var acc2 := \"\"; "
        "  for var d in s do acc2 := acc2 + $\"{d.Tag};\"; end for; "
        "  Console.WriteLine($\"concrete={acc2}\"); ";
    const std::string stdout = out(prog(decls, body));
    BEZEL_EXPECT_TRUE(stdout.find("erased=1;2;3;") != std::string::npos);
    // The CONCRETE for-in over the same stack visits in TAKE order (§4.11) —
    // the disagreement this row pins.  This half stays true forever.
    BEZEL_EXPECT_TRUE(stdout.find("concrete=3;2;1;") != std::string::npos);
}

// THE ERASED-VIEW TERMINALS ARE STILL UNREACHABLE — THE KNOWN RESIDUAL, AND
// IT IS NOT THIS FIX'S.  `var c: IReadOnlyCollection<int> := s;` erases the
// receiver's concrete type, so the §5 terminals over it are lowered by a
// DIFFERENT site (the borrowing-list-interface bridge), which today has no
// arm at all — the call is REJECTED at compile time (#2457), so it cannot
// answer a wrong value either.  That is issue #8265's territory (the erased
// view) and a different lowering site; this fix does not touch it and does not
// claim it.  The row pins the CURRENT (rejected) shape so that whoever
// implements the erased view has to move this row deliberately.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           erased_iroc_view_terminals_are_still_unreachable_residual_8265) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  s.Push(10); s.Push(20); s.Push(30); "
        "  var c: IReadOnlyCollection<int> := s; "
        "  Console.WriteLine(c.First(lambda(x) => x > 15)); ";
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("", body),
        "`First` on built-in type `IReadOnlyCollection` produced no value"));
}

// ===========================================================================
// THE LEAK / SANITIZER ROW.  `runAotChecked` compiles and links the program
// with AddressSanitizer and returns nonzero on ANY sanitizer finding, so a
// leak, a use-after-free and a double free each fail the run.  The loop shape
// is the one the contract asks for (100 iterations over a stack of STRINGS —
// the element kind whose slot is a shared reference, §28.11) so a per-call
// leak cannot hide under the noise of a single evaluation.
BEZEL_TEST(stack_take_order_predicate_lazy_8369_8370,
           predicate_and_lazy_surface_in_a_loop_over_strings_are_sanitizer_clean) {
    const std::string body =
        "  var acc := 0; "
        "  for var i in 0 .. 100 do "
        "    using var s := new Stack<string>(); "
        "    s.Push(\"alpha\"); s.Push(\"beta\"); s.Push(\"gamma\"); "
        "    acc := acc + s.First(lambda(x) => x <> \"alpha\").Length; "
        "    acc := acc + s.Last(lambda(x) => x <> \"gamma\").Length; "
        "    using var w := s.Where(lambda(x) => x <> \"alpha\").ToArray(); "
        "    acc := acc + w.Count; "
        "  end; "
        "  Console.WriteLine($\"{acc}\"); "
        "  return 0; ";
    JitResult r = runAotChecked(prog("", body));
    BEZEL_EXPECT_TRUE(r.ran);
    // 101 iterations x (5 + 5 + 2) — "gamma" is 5, "alpha" is 5, and the
    // filtered array holds 2 elements ("gamma" and "beta").
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1212") != std::string::npos);
}
