// Binding, storing or returning the BORROW RESULT of an instance call (or an
// indexer get) whose RECEIVER is an unbound owned temporary is a COMPILE
// ERROR (issue #8033; spec K8.4, §28.3 announcement table rows 4-5, §28.3.11,
// stdlib §4.11, v0.355).
//
// THE GOVERNING TEXT.
//
//   K8.4 (v0.355): "A temporary is an admissible borrow source only within its
//   boundary; a borrow or frame that would escape it is **rejected, never
//   lifetime-extended** … The escapes rejected are every escape K2.3 can
//   compare — among them a binding, store, capture, `return` or `yield
//   return` of the borrow in the producing body". Its worked example:
//   `var t := new Shelf().Top();  // error (K8.4): `t` would borrow from a
//   temporary disposed at this statement's end`.
//
//   §28.3 *Producers*, the call row: "A plain concrete class or interface
//   type → **Borrowed** (row 4: instance — source the receiver …). A **bare
//   type parameter** → the source-set classification of rows 5–6"; the
//   indexer row bounds a borrowing-collection element by the collection.
//
//   stdlib §4.11: "For class T on the borrowing forms, `Pop`, `Peek`, and
//   `Dequeue` produce borrows whose **compile-time source is the
//   collection** (… the result is statically usable while the receiver is
//   live)".
//
// THE BUG (#8033). `var x := Mk().Pop();` over `Mk(): owned Stack<Cell>` was
// ACCEPTED. The issue's own repro is a heap-use-after-free under ASan, but
// its mechanism is NOT the temporary `Stack` dying too early (that temporary
// is in fact never reclaimed, #6775): the `Cell` is freed inside `Mk`, by
// `s.Push(new Cell())`, a §15.4 fresh store that v0.346 made a compile error
// and the compiler still only warns about (#3047). The program has two
// spec errors; this file pins the K8.4 one, the binding at the call site,
// which is #8033's shape. The rejection removes the program.
//
// Same predicate, same rows below: the K8 example `new Shelf().Top()` (#7445)
// and the user-class `new W().Self()` form of #7627. NOT covered: a field
// read of a temporary bound to a plain `var` (#7402), a builtin class result
// such as `new StringBuilder().Append("x")` (#7627's other form), and the
// argument-sourced bare-type-parameter launder (already rejected, #1801).
//
// THE ORACLE. Each reject row pins the specific diagnostic. The control rows
// run under ASan (`runAotChecked`, or `runAotCheckedLenient` where the
// admitted program still leaks the temporary receiver — #6775, measured on
// the base) and loop 100 times, asserting the printed sum.
//
// per K8.4, §28.3, §28.3.11, stdlib §4.11, v0.355

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// Every `Mk*` takes its element as a plain parameter, so each collection holds
// a borrow of `c`, a `using var` of `Main` that outlives every use: the only
// temporary in play is the collection receiver itself.
const char* kDecls =
    "public class Cell "
    "  public v: int := 3; "
    "  public method Self(): Cell => this; "
    "end Cell; "
    "public class Shelf "
    "  private owned _top: Cell := new Cell(); "
    "  public method Top(): Cell => _top; "
    "end Shelf; "
    "public class Mk "
    "  public static method S(c: Cell): owned Stack<Cell> "
    "    var s := new Stack<Cell>(); s.Push(c); return s; "
    "  end S; "
    "  public static method Q(c: Cell): owned Queue<Cell> "
    "    var s := new Queue<Cell>(); s.Enqueue(c); return s; "
    "  end Q; "
    "  public static method A(c: Cell): owned Array<Cell> "
    "    var s := new Array<Cell>(); s.Add(c); return s; "
    "  end A; "
    "  public static method D(c: Cell): owned Dictionary<int, Cell> "
    "    var s := new Dictionary<int, Cell>(); s.Add(1, c); return s; "
    "  end D; "
    "  public static method OS(): owned OwningStack<Cell> "
    "    var s := new OwningStack<Cell>(); s.Push(new Cell()); return s; "
    "  end OS; "
    "end Mk; ";

std::string mainProg(const std::string& loopBody,
                     const std::string& extra = "") {
    return std::string("module M; import System; "
                       "import System.Collections.Generic; ") +
           kDecls + extra +
           "public class Program "
           "  public static method Main(): int "
           "    var t := 0; "
           "    using var c := new Cell(); "
           "    for var i in 0..<100 do " + loopBody + " end for; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

void expectRejectedTemporaryReceiver(const JitResult& r) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "over an unbound owned temporary receiver is a borrow whose "
           "source is that temporary"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "(K8.4: rejected, never "
                                                   "lifetime-extended)"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "bind the receiver to a name first"));
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- REJECT: the issue's repro, verbatim -------------------------------------

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_k8_4_issue_repro_bound_pop_of_an_owned_stack_temporary_is_rejected) {
    // #8033's program as filed (its `Push(new Cell())` is the separate §15.4
    // fresh store, still a warning — #3047). Before the fix it compiled and
    // ran into a heap-use-after-free under ASan.
    const JitResult r = runAotChecked(
        "module M; import System; import System.Collections.Generic;\n"
        "class Cell\n"
        "  public v: int := 3;\n"
        "end Cell;\n"
        "class C\n"
        "  public static method Mk(): owned Stack<Cell>\n"
        "    var s := new Stack<Cell>();\n"
        "    s.Push(new Cell());\n"
        "    return s;\n"
        "  end Mk;\n"
        "  public static method Main(): int\n"
        "    var t := 0;\n"
        "    for var i in 0..<100 do\n"
        "      var x := Mk().Pop();\n"
        "      t += x.v;\n"
        "    end for;\n"
        "    Console.WriteLine(t); return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectRejectedTemporaryReceiver(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`.Pop(...)`"));
}

// --- REJECT: the borrowing-family siblings -----------------------------------

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_k8_4_stack_pop_and_peek_over_a_temporary_are_rejected) {
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := Mk.S(c).Pop(); t += x.v;")));
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := Mk.S(c).Peek(); t += x.v;")));
}

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_k8_4_queue_dequeue_and_peek_over_a_temporary_are_rejected) {
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := Mk.Q(c).Dequeue(); t += x.v;")));
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := Mk.Q(c).Peek(); t += x.v;")));
}

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_k8_4_array_and_dictionary_lookups_over_a_temporary_are_rejected) {
    // The indexer row: a borrowing-collection element is bounded by the
    // collection (§28.3 *Producers*); `GetOrNull` is a row-5 call.
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := Mk.A(c)[0]; t += x.v;")));
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := Mk.D(c)[1]; t += x.v;")));
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := Mk.D(c).GetOrNull(1); t += x!.v;")));
}

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_k8_4_owning_stack_peek_over_a_temporary_is_rejected) {
    // `OwningStack<T>.Peek()` is a "borrow of top element" (stdlib §4.11):
    // its source is the temporary stack, which disposes the element with it.
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := Mk.OS().Peek(); t += x.v;")));
}

// --- REJECT: every escape spelling K8.4 names ---------------------------------

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_k8_4_bang_meet_using_and_assignment_spellings_are_rejected) {
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := Mk.S(c).Pop()!; t += x.v;")));
    expectRejectedTemporaryReceiver(runAotChecked(mainProg(
        "var x := (if i > 1 then Mk.S(c).Pop() else c); t += x.v;")));
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("using var x := Mk.S(c).Pop(); t += x.v;")));
    expectRejectedTemporaryReceiver(runAotChecked(
        mainProg("var x: Cell := c; x := Mk.S(c).Pop(); t += x.v;")));
}

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_k8_4_returning_the_borrow_result_is_rejected) {
    expectRejectedTemporaryReceiver(runAotChecked(mainProg(
        "t += new Get().It(c).v;",
        "public class Get "
        "  public method It(c: Cell): Cell "
        "    return Mk.S(c).Pop(); "
        "  end It; "
        "end Get; ")));
}

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_k8_4_worked_example_user_class_receiver_sourced_calls_are_rejected) {
    // K8.4's own example (#7445), and a `this`-returning method (#7627).
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := new Shelf().Top(); t += x.v;")));
    expectRejectedTemporaryReceiver(
        runAotChecked(mainProg("var x := new Cell().Self(); t += x.v;")));
}

// --- controls ------------------------------------------------------------------

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_k8_4_control_bind_the_receiver_first_then_pop) {
    // The diagnostic's remedy and the issue's "safe shape": the named `s` is
    // the borrow's source and outlives it.
    expectRunOutput(
        runAotChecked(mainProg("var s := Mk.S(c); var x := s.Pop(); t += x.v;")),
        "300\n");
    expectRunOutput(
        runAotChecked(
            mainProg("using var s := Mk.S(c); var x := s.Pop(); t += x.v;")),
        "300\n");
}

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_28_11_control_owning_stack_pop_transfers_out_of_a_temporary) {
    // `OwningStack<T>.Pop(): owned T` hands out a fresh owner (row 1), not a
    // borrow, so the temporary stack may die: the binding is admitted.
    expectRunOutput(
        runAotChecked(mainProg("var x := Mk.OS().Pop(); t += x.v;")),
        "300\n");
}

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_k8_4_control_a_use_within_the_full_expression_is_admitted) {
    // K8.4: "allowed (K8.4): a borrow within the boundary". Lenient: the
    // temporary receiver itself still leaks here (#6775, the same on the
    // base); the generated-code ASan net (use-after-free) stays on.
    expectRunOutput(
        runAotCheckedLenient(mainProg(
            "t += Mk.S(c).Pop().v; t += Mk.A(c)[0].v; "
            "t += new Shelf().Top().v;")),
        "900\n");
}

BEZEL_TEST(ownership_temporary_receiver_borrow_result_8033,
           spec_28_3_11_control_deferred_activation_frame_receiver_is_admitted) {
    // `s.Reverse()` is a deferred-activation frame (§28.3.11): an element read
    // through it borrows from `s` (the frame's carried source), not from the
    // frame, so binding it is admitted. Mirrors reverse_set_3763's
    // owningstack/owningqueue rows, which the first cut of the fix over-rejected.
    expectRunOutput(
        runAotChecked(mainProg(
            "using var s := new OwningStack<Cell>(); s.Push(new Cell()); "
            "var x := s.Reverse().FirstOrNull(); t += x!.v;")),
        "300\n");
}
