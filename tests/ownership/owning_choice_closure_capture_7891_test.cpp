// A closure that captures an OWNING-CHOICE local captures a BORROW of it
// (issue #7891; spec §28.9, §28.3 *Captured owned bindings*, §27.5, §35.4,
// v0.357).
//
// THE GOVERNING TEXT (docs/Bezel_Language_Spec.md, v0.357).
//
//   §28.9 — "Capturing a *move-only* local (an owning inline array, §15.3)
//   follows the same rule: the closure captures a borrow whose source is the
//   local's enclosing scope, and may not outlive it. ... a move-only capture
//   cannot be a copy (copying is forbidden) and cannot be a move (the local is
//   still in scope under its name ...)". §35.4 makes an owning-classified
//   choice a move-only value type, so the rule reaches it.
//
//   §28.3 *Captured owned bindings* — "Inside a closure, a captured owned
//   binding ... is uniformly in **borrow state**: reads borrow ..., and
//   assignment to it, a marked transfer from it, an explicit `Dispose()` or
//   discard of it, and every other owned-consuming use of it are compile
//   errors". For a move-only value the plain binding, argument and `return`
//   ARE owned-consuming uses (§15.3: they move unmarked).
//
// THE BUG (#7891), two halves.
//
//   Codegen. A captured local is boxed into a heap cell and its disposal
//   entry is rebound to the pointer-sized cell holder. The owning-choice
//   scope-end pass and the moved-from zero-store treated that holder as the
//   choice struct itself. MEASURED at r30-c1 46219d9ba under ASan on the row
//   `spec_28_9_borrowing_case_read_of_a_captured_local`:
//   `stack-buffer-overflow ... READ of size 24`, for a closure that only READS.
//
//   Ownership. The closure walk did not descend into a `case` scrutinee, so
//   the capture was invisible to it: a move of the captured local INSIDE the
//   closure was accepted (`var x := o;`), and so were a move of the local
//   while the closure was still live, a returned closure and a closure stored
//   into a field. Each was memory-unsafe at run time on the base.
//
// Every row below fails at r30-c1 46219d9ba: the run rows on the ASan report,
// the reject rows because the program compiled.
//
// per §28.9, §28.3, §27.5, §35.4, §15.3, v0.357

#include <string>

#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains, ...
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kPrelude = R"BZL(module M;
public class Res
  public static Count: int := 0;
  public N: int := 7;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
public choice OC Text(owned r: Res), Empty; end OC;
public choice KC
  Text(owned r: Res), Empty;
  public method Kind(): int => (case this of Text(r) => r.N, _ => 2);
end KC;
public choice Opt<T> Some(owned v: T), None; end Opt;
public class Sink
  public static method Take(v: OC): int => 1;
  public static method Peek(f: () -> int): int => f();
end Sink;
public class Hold
  public F: (() -> int)? := null;
  public constructor() end constructor;
end Hold;
)BZL";

std::string prog(const std::string& body, const std::string& members = "") {
    return std::string(kPrelude) + "public class Program\n" + members +
           "  public static method T(i: int): int\n" + body +
           "\n  end T;\n"
           "  public static method Main(): int\n"
           "    var s := 0;\n"
           "    for var i in 0..<100 do\n"
           "      s := s + T(i);\n"
           "    end;\n"
           "    Console.WriteLine($\"{Res.Count} {s}\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The `async` twin: `T` is an async method and `Main` awaits it.
std::string asyncProg(const std::string& body) {
    return std::string(kPrelude) + "public class Program\n"
           "  public static async method T(i: int): Task<int>\n" + body +
           "\n  end T;\n"
           "  public static async method Main(): Task\n"
           "    var s := 0;\n"
           "    for var i in 0..<100 do\n"
           "      s := s + await T(i);\n"
           "    end;\n"
           "    Console.WriteLine($\"{Res.Count} {s}\");\n"
           "  end Main;\n"
           "end Program;\n";
}

// 100 iterations under the strict AOT + ASan oracle: the program must run to
// completion and print "<destructor count> <sum>".
void expectClean(const std::string& src, const std::string& expected) {
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// A compile-time rejection carrying the named diagnostic.
void expectRejected(const std::string& src, const char* needle) {
    const JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, needle));
}

}  // namespace

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_issue_repro_capture_then_move_after_the_closures_last_use) {
    // The issue's first repro: a borrowing `case o of` read in the closure, then
    // `var x := o!;` once the closure is dead. One payload, disposed once.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(_) => 1, _ => 2);
    var k := f();
    var x := o!;
    return k;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_borrowing_case_read_of_a_captured_local) {
    // The smallest shape: the closure only reads the tag.
    expectClean(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(_) => 1, _ => 2);
    return f();)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_payload_read_through_the_captured_borrow) {
    // The closure reads the payload object through the borrow.
    expectClean(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(r) => r.N, _ => 2);
    return f();)BZL"),
                "100 700\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_capture_of_a_using_var_local) {
    // A `using var` local is captured the same way; its scope end disposes it.
    expectClean(prog(R"BZL(    using var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(r) => r.N, _ => 2);
    return f();)BZL"),
                "100 700\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_capture_of_a_nullable_local_that_is_null_on_one_path) {
    // `OC?`: the cell holds null on the odd iterations.
    expectClean(prog(R"BZL(    var o: OC? := null;
    if i mod 2 = 0 then o := OC.Text(new Res()); end if;
    var f := lambda(): int => (case o of Text(r) => r.N, null => 3, _ => 2);
    return f();)BZL"),
                "50 500\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_reassignment_after_the_closures_last_use) {
    // The store displaces through the cell: the old payload is disposed by the
    // store, the new one at scope end.
    expectClean(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(_) => 1, _ => 2);
    var k := f();
    o := OC.Text(new Res());
    return k;)BZL"),
                "200 100\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_35_5_owning_match_after_the_closures_last_use) {
    // `case move o of` consumes the captured local after the closure is dead.
    expectClean(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(_) => 1, _ => 2);
    var k := f();
    return k + (case move o of Text(r) => 1, _ => 2);)BZL"),
                "100 200\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_two_closures_share_one_captured_local) {
    // Two closures borrow the same cell.
    expectClean(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(_) => 1, _ => 2);
    var g := lambda(): int => (case o of Text(r) => r.N, _ => 2);
    return f() + g();)BZL"),
                "100 800\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_capture_inside_a_loop_body) {
    // A fresh local and a fresh cell per iteration.
    expectClean(prog(R"BZL(    var k := 0;
    for var j in 0..<3 do
      var o := OC.Text(new Res());
      var f := lambda(): int => (case o of Text(_) => 1, _ => 2);
      k := k + f();
    end;
    return k;)BZL"),
                "300 300\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_capturing_lambda_passed_as_an_argument) {
    // The closure is an argument temporary: it does not outlive the call.
    expectClean(prog(R"BZL(    var o := OC.Text(new Res());
    return Sink.Peek(lambda(): int => (case o of Text(r) => r.N, _ => 2));)BZL"),
                "100 700\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_3_exception_path_disposes_the_captured_local_once) {
    // The unwind edge runs the same scope-end pass over the cell.
    expectClean(prog(R"BZL(    var k := 0;
    try
      var o := OC.Text(new Res());
      var f := lambda(): int => (case o of Text(_) => 1, _ => 2);
      k := f();
      if i >= 0 then throw new InvalidOperationException("x"); end if;
    catch var e: InvalidOperationException => k := k + 1;
    end;
    return k;)BZL"),
                "100 200\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_early_return_disposes_the_captured_local_once) {
    // Both return paths dispose through the cell.
    expectClean(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(_) => 1, _ => 2);
    if i mod 2 = 0 then return f(); end if;
    return 5;)BZL"),
                "100 300\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_capture_through_a_move_only_receiver_call) {
    // The closure calls a method of the captured choice value (§9.4: the
    // receiver is lent in place).
    expectClean(prog(R"BZL(    var o := KC.Text(new Res());
    var f := lambda(): int => o.Kind();
    var k := f();
    var x := o;
    return k;)BZL"),
                "100 700\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_capture_of_a_generic_owning_choice_local) {
    // The generic face: `Opt<Res>` classifies owning at this instantiation.
    expectClean(prog(R"BZL(    var o := Opt<Res>.Some(new Res());
    var f := lambda(): int => (case o of Some(r) => r.N, _ => 0);
    return f();)BZL"),
                "100 700\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_capture_in_an_async_frame_called_after_an_await) {
    // The async twin: the local and its cell live in the frame across the
    // `await`.
    expectClean(asyncProg(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(r) => r.N, _ => 0);
    await Task.Delay(1);
    return f();)BZL"),
                "100 700\n");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_3_issue_repro_move_inside_the_closure_is_rejected) {
    // The issue's second half: a plain binding of a move-only value MOVES
    // (§15.3), and a captured owned binding may not be consumed in the closure.
    expectRejected(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var f := lambda(): int => do var x := o; return 1; end;
    return f();)BZL"),
                   "cannot transfer captured owned binding `o` inside a closure");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_17_8_bang_move_inside_the_closure_is_rejected) {
    // §17.8: `o!` classifies exactly as the bare `o` does.
    expectRejected(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var f := lambda(): int => do var x := o!; return 1; end;
    return f();)BZL"),
                   "cannot transfer captured owned binding `o` inside a closure");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_3_argument_move_inside_the_closure_is_rejected) {
    // §15.3 *Parameter passing*: by value-move for a move-only V.
    expectRejected(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => Sink.Take(o);
    return f();)BZL"),
                   "cannot transfer captured owned binding `o` inside a closure");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_3_using_var_adoption_inside_the_closure_is_rejected) {
    // `using var x := o;` over a move-only value adopts (moves) it.
    expectRejected(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => do using var x := o; return 1; end;
    return f();)BZL"),
                   "cannot transfer captured owned binding `o` inside a closure");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_3_return_of_the_captured_local_from_the_closure_is_rejected) {
    // §15.3 *Return*: returning a move-only value transfers it.
    expectRejected(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): OC => o;
    var x := f();
    return 1;)BZL"),
                   "cannot transfer captured owned binding `o` inside a closure");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_20_conditional_arm_move_inside_the_closure_is_rejected) {
    // §20: a branch naming the binding moves at an owned-consuming position.
    expectRejected(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => do var x: OC := (if i > 0 then o else OC.Empty); return 1; end;
    return f();)BZL"),
                   "cannot transfer captured owned binding `o` inside a closure");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_3_move_inside_a_closure_in_an_async_body_is_rejected) {
    // The async twin of the in-closure move.
    expectRejected(asyncProg(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => do var y: OC := o; return 1; end;
    return f();)BZL"),
                   "cannot transfer captured owned binding `o` inside a closure");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_move_of_the_local_while_the_closure_is_live_is_rejected) {
    // The closure holds a borrow of `o`; moving `o` out before the closure's
    // last use would let the call read a moved-from cell.
    expectRejected(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(_) => 1, _ => 2);
    var x := o;
    return f();)BZL"),
                   "cannot consume `o` here: the borrow held by `f` is still live");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_reassignment_while_the_closure_is_live_is_rejected) {
    // A store disposes the value the closure's borrow still reads.
    expectRejected(prog(R"BZL(    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(_) => 1, _ => 2);
    o := OC.Empty;
    return f();)BZL"),
                   "cannot consume `o` here: the borrow held by `f` is still live");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_returned_closure_may_not_outlive_the_captured_local) {
    // §28.9: the closure "may not outlive" the local's enclosing scope.
    expectRejected(prog(R"BZL(    var g := Mk();
    return g();)BZL", R"BZL(  public static method Mk(): () -> int
    var o := OC.Text(new Res());
    return lambda(): int => (case o of Text(_) => 1, _ => 2);
  end Mk;
)BZL"),
                   "returned closure captures `o` whose source is disposed at scope end");
}

BEZEL_TEST(ownership_owning_choice_closure_capture_7891,
           spec_28_9_closure_stored_into_a_field_may_not_outlive_the_local) {
    // The field outlives the local's scope.
    expectRejected(prog(R"BZL(    var o := OC.Text(new Res());
    var h := new Hold();
    h.F := lambda(): int => (case o of Text(_) => 1, _ => 2);
    return 1;)BZL"),
                   "borrow of `o` stored into a field would outlive its source");
}
