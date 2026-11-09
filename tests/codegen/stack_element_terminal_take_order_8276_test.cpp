// Regression tests for issue #8276 — the stdlib §5 element-access terminals
// (`First()` / `Last()` / `ElementAt(i)`, and their `*OrNull` siblings) over a
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
// stdlib §5 (v0.355, the `extension IEnumerable<T>` block, lines 3668-3682)
// declares `First(): T`, `Last(): T`, `ElementAt(index: int): T` and the
// `*OrNull` siblings over `IEnumerable<T>` — i.e. over the receiver's
// ITERATION order. ⇒ the terminals must answer in the SAME order the
// iteration of the same value visits it.
//
// THE DEFECT (MEASURED on origin/main a30bb11d).  A `Stack<T>` is stored
// bottom-to-top (index 0 = the bottom, index Count-1 = the top — the index
// `Pop()` itself reads), and the terminals read the backing array BY ELEMENT
// INDEX, so `First()` returned the BOTTOM element and `Last()` the TOP:
//
//   using var s := new Stack<int>(); s.Push(10); s.Push(20); s.Push(30);
//   for var x in s do Console.WriteLine(x); end for;   // 30 20 10 (take)
//   Console.WriteLine(s.First());                      // 10  (push — WRONG)
//   Console.WriteLine(s.Last());                       // 30  (push — WRONG)
//   Console.WriteLine(s.ElementAt(0));                 // 10  (push — WRONG)
//
// AFTER THE FIX: `First()` is 30 (the top), `Last()` is 10 (the bottom), and
// `ElementAt(i)` indexes in the SAME order the iteration visits — so
// `ElementAt(0)` is the top (30) and `ElementAt(2)` is the bottom (10).
//
// WHAT IS *NOT* CHANGED (the control rows below).  `Array<T>` states no
// take-order contract distinct from storage order — its `First()` IS its
// element 0 and its `Last()` IS its element N-1 — and `Queue<T>` has its own
// front-to-back (dequeue) order, which coincides with its storage order.  Both
// are measured on the base and on the fix, and must be byte-identical.
//
// WHERE THE FIX LIVES.  `src/codegen/LlvmBackend_Calls3.cpp`, the §5 dispatch
// (`tryLowerLazyLinqCall`), which intercepts the element-access tier for the
// stack family before the ordinary array-receiver lowering sees it.  The
// terminals' faults (InvalidOperationException on an empty source,
// IndexOutOfRangeException on an out-of-range `ElementAt`), their nullable
// form, the §28.11 string-element escape rule and the §28.3
// unbound-owned-temporary reclaim are unchanged; only the ORDER moved.

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

// The three-element stack the issue's own repro uses.  The pushes are 10, 20,
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

// The class-element shape: a class instance is a REFERENCE element, a
// different representation from a value element (§28.11's element-slot table).
const char* kBoxDecl =
    "class Box public Tag: string; "
    "  public constructor(tag: string) this.Tag := tag; end constructor; "
    "end Box; ";

// The record-element shape: a record is a VALUE element (§9.1/§9.2).
const char* kPointDecl =
    "record Point public X: int; public Y: int; end Point; ";

}  // namespace

// ===========================================================================
// THE FIX ROWS — red on the base (which answered 10 / 30 / 10 / 30), green
// with the fix (30 / 10 / 30 / 10).
// ===========================================================================

// stdlib §4.11 / §5: `First()` over a concrete `Stack<T>` is the FIRST
// element of the receiver's ITERATION order — the TOP (the last pushed).
BEZEL_TEST(stack_element_terminal_take_order_8276,
           first_is_the_top_element_stdlib_4_11) {
    BEZEL_EXPECT_EQ(out(stack3("  Console.WriteLine(s.First()); ")), "30\n");
}

// stdlib §4.11 / §5: `Last()` is the LAST element of the iteration order —
// the BOTTOM (the first pushed).
BEZEL_TEST(stack_element_terminal_take_order_8276,
           last_is_the_bottom_element_stdlib_4_11) {
    BEZEL_EXPECT_EQ(out(stack3("  Console.WriteLine(s.Last()); ")), "10\n");
}

// stdlib §4.11 / §5: `ElementAt(i)` indexes in the SAME order the iteration
// visits, so `ElementAt(0)` is the top and `ElementAt(2)` is the bottom.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           element_at_indexes_in_the_iteration_order_stdlib_4_11) {
    BEZEL_EXPECT_EQ(out(stack3("  Console.WriteLine(s.ElementAt(0)); "
                                "  Console.WriteLine(s.ElementAt(1)); "
                                "  Console.WriteLine(s.ElementAt(2)); ")),
                       "30\n20\n10\n");
}

// THE STRONGEST ROW: the terminals and the iteration of the SAME value, side
// by side.  The iteration is 30, 20, 10 (take order) and the terminals must
// answer the same sequence: First = 30, Last = 10, ElementAt(0..2) = 30, 20,
// 10.  A terminal that disagreed with the iteration of the same stack could
// not pass this row.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           terminals_agree_with_the_for_in_iteration_of_the_same_stack) {
    BEZEL_EXPECT_EQ(out(stack3("  for var x in s do Console.WriteLine(x); end for; "
                                "  Console.WriteLine(s.First()); "
                                "  Console.WriteLine(s.Last()); "
                                "  Console.WriteLine(s.ElementAt(0)); "
                                "  Console.WriteLine(s.ElementAt(1)); "
                                "  Console.WriteLine(s.ElementAt(2)); ")),
                       "30\n20\n10\n30\n10\n30\n20\n10\n");
}

// stdlib §5 declares the `*OrNull` tier as the nullable SIBLING of the
// throwing forms on the SAME extension block, over the SAME `IEnumerable<T>`.
// They must not keep the old (push) order while the throwing forms move —
// that would leave the tier internally inconsistent.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           ornull_siblings_agree_with_the_throwing_forms_stdlib_5) {
    BEZEL_EXPECT_EQ(out(stack3("  Console.WriteLine(s.FirstOrNull() ?? -1); "
                                "  Console.WriteLine(s.LastOrNull() ?? -1); "
                                "  Console.WriteLine(s.ElementAtOrNull(0) ?? -1); "
                                "  Console.WriteLine(s.ElementAtOrNull(2) ?? -1); ")),
                       "30\n10\n30\n10\n");
}

// stdlib §4.11: "the owning forms iterate identically".  `OwningStack<T>` is
// the owning twin of `Stack<T>` and must answer in the same take order.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           owning_stack_terminals_take_order_stdlib_4_11) {
    const std::string body =
        "  using var s := new OwningStack<Box>(); "
        "  s.Push(new Box(\"one\")); s.Push(new Box(\"two\")); "
        "  s.Push(new Box(\"three\")); "
        "  for var x in s do Console.WriteLine(x.Tag); end for; "
        "  Console.WriteLine(s.First().Tag); "
        "  Console.WriteLine(s.Last().Tag); "
        "  Console.WriteLine(s.ElementAt(0).Tag); "
        "  Console.WriteLine(s.ElementAt(2).Tag); ";
    BEZEL_EXPECT_EQ(out(prog(kBoxDecl, body)),
                    "three\ntwo\none\nthree\none\nthree\none\n");
}

// THE ORDER IS A CONTRACT — MEASURE THE ELEMENT TYPE AS WELL AS THE ORDER.  A
// `string` element is a runtime-managed REFERENCE slot (§28.11), so a
// mis-typed or mis-indexed read would show up here as the wrong element.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           string_elements_return_the_right_element) {
    const std::string body =
        "  using var s := new Stack<string>(); "
        "  s.Push(\"alpha\"); s.Push(\"beta\"); s.Push(\"gamma\"); "
        "  Console.WriteLine(s.First()); "
        "  Console.WriteLine(s.Last()); "
        "  Console.WriteLine(s.ElementAt(0)); "
        "  Console.WriteLine(s.ElementAt(2)); ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "gamma\nalpha\ngamma\nalpha\n");
}

// A CLASS element is a reference element — a different representation from a
// value element.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           class_elements_return_the_right_instance) {
    const std::string body =
        "  using var b1 := new Box(\"one\"); "
        "  using var b2 := new Box(\"two\"); "
        "  using var b3 := new Box(\"three\"); "
        "  using var s := new Stack<Box>(); "
        "  s.Push(b1); s.Push(b2); s.Push(b3); "
        "  Console.WriteLine(s.First().Tag); "
        "  Console.WriteLine(s.Last().Tag); "
        "  Console.WriteLine(s.ElementAt(0).Tag); "
        "  Console.WriteLine(s.ElementAt(2).Tag); ";
    BEZEL_EXPECT_EQ(out(prog(kBoxDecl, body)), "three\none\nthree\none\n");
}

// A RECORD element is a VALUE element (§9.1/§9.2) — its bytes live in the
// slot, so a wrong stride or a mis-typed read would show up here.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           record_elements_return_the_right_value) {
    const std::string body =
        "  using var s := new Stack<Point>(); "
        "  s.Push(new Point(1, 10)); s.Push(new Point(2, 20)); "
        "  s.Push(new Point(3, 30)); "
        "  Console.WriteLine(s.First().X); "
        "  Console.WriteLine(s.Last().X); "
        "  Console.WriteLine(s.ElementAt(0).X); "
        "  Console.WriteLine(s.ElementAt(2).X); ";
    BEZEL_EXPECT_EQ(out(prog(kPointDecl, body)), "3\n1\n3\n1\n");
}

// ===========================================================================
// THE CONTROL ROWS — green on the base AND on the fix.  They are the reason a
// wrong-order fix would be loud rather than silent.
// ===========================================================================

// An ARRAY states no take-order contract distinct from storage order: its
// `First()` IS its element 0 and its `Last()` IS its element N-1.  A fix that
// flipped the array would be a REJECT.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           array_terminals_keep_storage_order_control) {
    const std::string body =
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  for var x in a do Console.WriteLine(x); end for; "
        "  Console.WriteLine(a.First()); "
        "  Console.WriteLine(a.Last()); "
        "  Console.WriteLine(a.ElementAt(0)); "
        "  Console.WriteLine(a.ElementAt(2)); ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "10\n20\n30\n10\n30\n10\n30\n");
}

// A QUEUE has its own front-to-back (dequeue) order, which coincides with its
// storage order — it is already consistent and must not move.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           queue_terminals_keep_front_first_order_control) {
    const std::string body =
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(10); q.Enqueue(20); q.Enqueue(30); "
        "  for var x in q do Console.WriteLine(x); end for; "
        "  Console.WriteLine(q.First()); "
        "  Console.WriteLine(q.Last()); "
        "  Console.WriteLine(q.ElementAt(0)); "
        "  Console.WriteLine(q.ElementAt(2)); ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "10\n20\n30\n10\n30\n10\n30\n");
}

// The `for ... in` over a concrete stack was ALREADY take order (#2472) and
// must not move: it is the anchor the terminals are measured against.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           for_in_iteration_order_is_unchanged_control) {
    BEZEL_EXPECT_EQ(out(stack3("  for var x in s do Console.WriteLine(x); end for; ")),
                    "30\n20\n10\n");
}

// An EMPTY stack keeps the documented faults (stdlib §5, line 3758:
// "`First`/`Last`/`Single`/`ElementAt` throw `InvalidOperationException` (or
// `IndexOutOfRangeException` for `ElementAt`) on missing-element"), and the
// `*OrNull` forms keep returning null.  A fix that turned the empty fault
// into a crash or a wrong value would be a REJECT.
BEZEL_TEST(stack_element_terminal_take_order_8276,
           empty_stack_keeps_the_documented_faults_control) {
    const std::string body =
        "  using var s := new Stack<int>(); "
        "  try Console.WriteLine(s.First()); "
        "  catch InvalidOperationException => Console.WriteLine(\"first\"); end; "
        "  try Console.WriteLine(s.Last()); "
        "  catch InvalidOperationException => Console.WriteLine(\"last\"); end; "
        "  try Console.WriteLine(s.ElementAt(0)); "
        "  catch IndexOutOfRangeException => Console.WriteLine(\"oor\"); end; "
        "  try Console.WriteLine(s.ElementAt(-1)); "
        "  catch IndexOutOfRangeException => Console.WriteLine(\"neg\"); end; "
        "  Console.WriteLine(s.FirstOrNull() ?? -1); "
        "  Console.WriteLine(s.LastOrNull() ?? -1); "
        "  Console.WriteLine(s.ElementAtOrNull(0) ?? -1); ";
    BEZEL_EXPECT_EQ(out(prog("", body)),
                    "first\nlast\noor\nneg\n-1\n-1\n-1\n");
}

// A ONE-element stack answers that element from every terminal (the take
// order and the push order coincide there, so this row is green on both arms).
BEZEL_TEST(stack_element_terminal_take_order_8276,
           single_element_stack_answers_that_element_control) {
    const std::string body =
        "  using var s := new Stack<int>(); s.Push(42); "
        "  Console.WriteLine(s.First()); "
        "  Console.WriteLine(s.Last()); "
        "  Console.WriteLine(s.ElementAt(0)); "
        "  Console.WriteLine(s.FirstOrNull() ?? -1); "
        "  Console.WriteLine(s.LastOrNull() ?? -1); "
        "  Console.WriteLine(s.ElementAtOrNull(0) ?? -1); ";
    BEZEL_EXPECT_EQ(out(prog("", body)), "42\n42\n42\n42\n42\n42\n");
}

// ===========================================================================
// THE LEAK / SANITIZER ROW.  `runAotChecked` compiles and links the program
// with AddressSanitizer and returns nonzero on ANY sanitizer finding, so a
// leak, a use-after-free and a double free each fail the run.  The loop shape
// is the one the contract asks for (100 iterations over a stack of STRINGS —
// the element kind whose slot is a shared reference, §28.11) so a per-call
// leak cannot hide under the noise of a single evaluation.
// ===========================================================================
BEZEL_TEST(stack_element_terminal_take_order_8276,
           terminals_in_a_loop_over_strings_are_sanitizer_clean) {
    const std::string body =
        "  var acc := 0; "
        "  for var i in 0 .. 100 do "
        "    using var s := new Stack<string>(); "
        "    s.Push(\"alpha\"); s.Push(\"beta\"); s.Push(\"gamma\"); "
        "    acc := acc + s.First().Length; "
        "    acc := acc + s.Last().Length; "
        "    acc := acc + s.ElementAt(0).Length; "
        "    acc := acc + s.ElementAt(2).Length; "
        "  end; "
        "  Console.WriteLine($\"{acc}\"); "
        "  return 0; ";
    JitResult r = runAotChecked(prog("", body));
    BEZEL_EXPECT_TRUE(r.ran);
    // 101 iterations x (5 + 5 + 5 + 5) characters — "gamma" is 5 and "alpha"
    // is 5, and the take order makes First() and ElementAt(0) the "gamma"
    // (the top) and Last() and ElementAt(2) the "alpha" (the bottom).
    BEZEL_EXPECT_TRUE(r.stdoutText.find("2020") != std::string::npos);
}
