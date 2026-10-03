// Regression tests for issue #5297 — §22.6 "out _" and §28.3 *Unbound owned
// temporaries* rule 1 (v0.313): against an `out owned T` formal the discarded
// value "is an unbound owned temporary disposed at the end of the full
// statement" — AFTER the remaining operands of the statement ran.
//
// Pre-fix the discarded value was disposed immediately after the call that
// produced it (lowerCall's discardOwnedOutSlots_ loop for a user formal,
// disposeDiscardedOwnedOut for the builtin Try* forms), i.e. EARLY: before a
// later operand evaluated. The fix queues the scratch slot on the K8 boundary
// (deferUnboundOwnedTempToK8); an `unwindOnly dynamicDispose` entry keeps the
// exception edge covered, so a later operand that throws still disposes it
// exactly once.
//
// Rows loop 120 times under runAotChecked (ASan + LSan). `R.n` counts
// destructor runs; `C.Seen()` reads it, so a later operand observes whether the
// discarded value was already disposed.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kLib5297 = R"BZL(
class R
  public static n: int := 0;
  public constructor() end constructor;
  destructor R.n := R.n + 1; end destructor;
end R;
class PR
  public constructor() end constructor;
  destructor Console.WriteLine("D"); end destructor;
end PR;
class C
  public static hits: int := 0;
  public static method Hit() C.hits := C.hits + 1; end Hit;
  public static method Seen(): int return R.n; end Seen;
  public static method Mark(): int  Console.WriteLine("M"); return 1;  end Mark;
  public static method Boom(): int
    throw new InvalidOperationException("b");
  end Boom;
  public static method Take(b: bool, n: int): int return n; end Take;
  public static method Take2(b: bool, c: bool, n: int): int return n; end Take2;
  public static method Add(a: int, c: int): int return a + c; end Add;
  public static method Pick(b: bool): int return 7; end Pick;
  public static method Make(out owned r: R?): bool  r := new R(); return true;  end Make;
  public static method MakeP(out owned r: PR?): bool  r := new PR(); return true;  end MakeP;
  public static method MakeNone(out owned r: R?): bool  r := null; return false;  end MakeNone;
  public static method PlainOut(out r: int): bool  r := 5; return true;  end PlainOut;
  public static async method SeenA(): Task<int>
    await Task.Delay(1);
    return R.n;
  end SeenA;
  public static method Ret(): int
    using var a := new OwningArray<R>();
    a.Add(move (new R()));
    return Take(a.TryPopFirst(out _), Seen());
  end Ret;
  public static method RetUser(): int
    return Take(Make(out _), Seen());
  end RetUser;
end C;
)BZL";

// `body` runs 120 times, `c0` = R.n at the start of the iteration.
std::string app5297(const std::string& body, bool isAsync = false) {
    return std::string("module M5297;\n") + kLib5297 +
           "public class Program\n"
           "  public static " + (isAsync ? "async " : "") + "method Main(): " +
           (isAsync ? "Task<int>" : "int") + "\n"
           "    var s := 0;\n"
           "    var bad := 0;\n"
           "    for var i in 0..<120 do\n"
           "      const c0 := R.n;\n"
           "      " + body + "\n"
           "    end;\n"
           "    Console.WriteLine(\"bad=\" + bad.ToString() + \" n=\" + "
           "R.n.ToString() + \" s=\" + s.ToString() + \" h=\" + "
           "C.hits.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

void expectRow(const std::string& body, const std::string& want,
               bool isAsync = false) {
    auto r = runAotChecked(app5297(body, isAsync));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// The issue's builtin repro verbatim: exact order M, T, D, end.
BEZEL_TEST(full_expr_out_discard_dispose_5297, issue_repro_builtin_exact_order) {
    auto r = runAotChecked(R"BZL(module M;
class R
  destructor Console.WriteLine("D"); end destructor;
end R;
class C
  public static method Mark(): int  Console.WriteLine("M"); return 1;  end Mark;
  public static method Take(b: bool, n: int)  Console.WriteLine("T");  end Take;
  public static method Main(): int
    using var a := new OwningArray<R>();
    a.Add(move (new R()));
    Take(a.TryPopFirst(out _), Mark());
    Console.WriteLine("end");
    return 0;
  end Main;
end C;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("M\nT\nD\nend\n"));
}

// The issue's user-method variant: exact order M, T, D, end.
BEZEL_TEST(full_expr_out_discard_dispose_5297, issue_repro_user_method_exact_order) {
    auto r = runAotChecked(R"BZL(module M;
class R
  destructor Console.WriteLine("D"); end destructor;
end R;
class C
  public static method Mark(): int  Console.WriteLine("M"); return 1;  end Mark;
  public static method Take(b: bool, n: int)  Console.WriteLine("T");  end Take;
  public static method Make(out owned r: R?): bool  r := new R(); return true;  end Make;
  public static method Main(): int
    Take(Make(out _), Mark());
    Console.WriteLine("end");
    return 0;
  end Main;
end C;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("M\nT\nD\nend\n"));
}

// User `out owned` formal, looped: a later operand sees the value alive; it is
// gone when the statement ends.
BEZEL_TEST(full_expr_out_discard_dispose_5297, user_formal_later_operand_loop_120) {
    expectRow("const seen := C.Take(C.Make(out _), C.Seen());\n"
              "      if seen <> c0 then bad := bad + 1; end;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=0\n");
}

// Builtin OwningArray.TryPopFirst.
BEZEL_TEST(full_expr_out_discard_dispose_5297, builtin_try_pop_first_loop_120) {
    expectRow("using var a := new OwningArray<R>();\n"
              "      a.Add(move (new R()));\n"
              "      const seen := C.Take(a.TryPopFirst(out _), C.Seen());\n"
              "      if seen <> c0 then bad := bad + 1; end;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=0\n");
}

// Builtin OwningLinkedList.TryPopLast.
BEZEL_TEST(full_expr_out_discard_dispose_5297, builtin_linked_list_try_pop_last_loop_120) {
    expectRow("using var a := new OwningLinkedList<R>();\n"
              "      a.Add(move (new R()));\n"
              "      const seen := C.Take(a.TryPopLast(out _), C.Seen());\n"
              "      if seen <> c0 then bad := bad + 1; end;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=0\n");
}

// Builtin OwningDictionary.TryPopValue.
BEZEL_TEST(full_expr_out_discard_dispose_5297, builtin_dict_try_pop_value_loop_120) {
    expectRow("using var d := new OwningDictionary<int, R>();\n"
              "      d.Add(1, move (new R()));\n"
              "      const seen := C.Take(d.TryPopValue(1, out _), C.Seen());\n"
              "      if seen <> c0 then bad := bad + 1; end;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=0\n");
}

// (OwningQueue.TryDequeue / OwningStack.TryPop `out _` leak independently of
// this ordering rule: issue #5336, a different arm with no discard disposal.)

// Two discards in one statement: both outlive the later operand.
BEZEL_TEST(full_expr_out_discard_dispose_5297, two_discards_one_statement_loop_120) {
    expectRow("const seen := C.Take2(C.Make(out _), C.Make(out _), C.Seen());\n"
              "      if seen <> c0 then bad := bad + 1; end;\n"
              "      if R.n <> c0 + 2 then bad := bad + 1; end;",
              "bad=0 n=240 s=0 h=0\n");
}

// Nested: the discard sits inside an inner call; disposed at the end of the
// whole statement, after the outer call's later operand.
BEZEL_TEST(full_expr_out_discard_dispose_5297, nested_calls_loop_120) {
    expectRow("const seen := C.Add(C.Pick(C.Make(out _)), C.Seen());\n"
              "      if seen <> 7 + c0 then bad := bad + 1; end;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=0\n");
}

// Condition: the discard is disposed at the end of the condition (its own K8
// boundary), before the `then` body.
BEZEL_TEST(full_expr_out_discard_dispose_5297, if_condition_loop_120) {
    expectRow("if C.Make(out _) and C.Seen() = c0 then\n"
              "        if R.n <> c0 + 1 then bad := bad + 1; end;\n"
              "      else\n"
              "        bad := bad + 1;\n"
              "      end;",
              "bad=0 n=120 s=0 h=0\n");
}

// `return` operand, builtin and user forms.
BEZEL_TEST(full_expr_out_discard_dispose_5297, return_expression_builtin_loop_120) {
    expectRow("const seen := C.Ret();\n"
              "      if seen <> c0 then bad := bad + 1; end;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=0\n");
}
BEZEL_TEST(full_expr_out_discard_dispose_5297, return_expression_user_loop_120) {
    expectRow("const seen := C.RetUser();\n"
              "      if seen <> c0 then bad := bad + 1; end;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=0\n");
}

// Async body: the discarded value is alive ACROSS an await in a later operand
// (frame-spilled slot, #5292), and disposed after the statement.
BEZEL_TEST(full_expr_out_discard_dispose_5297, async_user_formal_across_await_loop_120) {
    expectRow("const seen := C.Take(C.Make(out _), await C.SeenA());\n"
              "      if seen <> c0 then bad := bad + 1; end;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=0\n", /*isAsync=*/true);
}
BEZEL_TEST(full_expr_out_discard_dispose_5297, async_builtin_across_await_loop_120) {
    expectRow("using var a := new OwningArray<R>();\n"
              "      a.Add(move (new R()));\n"
              "      const seen := C.Take(a.TryPopFirst(out _), await C.SeenA());\n"
              "      if seen <> c0 then bad := bad + 1; end;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=0\n", /*isAsync=*/true);
}

// A LATER operand throws: the discarded value is disposed exactly once.
BEZEL_TEST(full_expr_out_discard_dispose_5297, later_operand_throws_user_disposed_once_loop_120) {
    expectRow("try\n"
              "        s := s + C.Take(C.Make(out _), C.Boom());\n"
              "      catch\n"
              "        InvalidOperationException => C.Hit(),\n"
              "      end try;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=120\n");
}
BEZEL_TEST(full_expr_out_discard_dispose_5297, later_operand_throws_builtin_disposed_once_loop_120) {
    expectRow("using var a := new OwningArray<R>();\n"
              "      a.Add(move (new R()));\n"
              "      try\n"
              "        s := s + C.Take(a.TryPopFirst(out _), C.Boom());\n"
              "      catch\n"
              "        InvalidOperationException => C.Hit(),\n"
              "      end try;\n"
              "      if R.n <> c0 + 1 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=120\n");
}

// Control: a false-returning Try* leaves nothing to dispose.
BEZEL_TEST(full_expr_out_discard_dispose_5297, control_false_try_nothing_disposed_loop_120) {
    expectRow("using var a := new OwningArray<R>();\n"
              "      const ok := a.TryPopFirst(out _);\n"
              "      if ok or R.n <> c0 then bad := bad + 1; end;\n"
              "      const ok2 := C.MakeNone(out _);\n"
              "      if ok2 or R.n <> c0 then bad := bad + 1; end;",
              "bad=0 n=0 s=0 h=0\n");
}

// Control: `out using var x` is a real binding (scope-bound, not a discard):
// disposed at scope end, not at the statement's end.
BEZEL_TEST(full_expr_out_discard_dispose_5297, control_out_using_var_binding_scope_end) {
    expectRow("const ok := C.Make(out using var x);\n"
              "      if not ok or R.n <> c0 then bad := bad + 1; end;",
              "bad=0 n=120 s=0 h=0\n");
}

// Control: `out _` against a plain (non-owned) `out` formal owns nothing.
BEZEL_TEST(full_expr_out_discard_dispose_5297, control_plain_out_discard_loop_120) {
    expectRow("const ok := C.PlainOut(out _);\n"
              "      if not ok or R.n <> c0 then bad := bad + 1; end;",
              "bad=0 n=0 s=0 h=0\n");
}
