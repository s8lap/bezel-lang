// Codegen — issue #7940 (§31.2 / §30.9 / §31.1, v0.357): the supersede paths
// END THE HANDLING of the caught exception; they never dispose an instance its
// task still owns.
//
//     var t := BoomAsync();
//     try
//       try await t; catch var e: MyEx => throw new ArgumentException("inner"); end;
//     catch var e: ArgumentException => ... end;
//     await t;                       // the SAME live instance again (§30.4)
//
// §31.2, v0.357, verbatim: "a superseded task-owned fault instance (§30.9) —
// the `await` or `Result` re-raise of a faulted task that owns its fault — is
// dropped from the in-flight slot **undisposed**, its owner's disposal rules
// unchanged — supersession ends the instance's handling, never its life".
// §30.9, v0.357: the task-owned instance "is disposed exactly once, by the
// task that owns it". §31.1, v0.357: a handler-owned exception is disposed
// after its arm completes "normally, or by a `return`, `break` or `continue`
// that leaves it, or by a fresh `throw` from it".
//
// Every supersede site (`throw <new>` from an arm, the destructor-parked
// supersede, the cleanup-pass supersede) disposed the superseded instance
// unconditionally, so a bound task's instance was freed while the task still
// owned it: the next `await t` re-raised freed memory (MEASURED on r30-c1
// 4a6919f1: every "_then_reawait" row below is an ASan heap-use-after-free).
// And the site-specific test for "this throw leaves the arm" only recognised a
// direct `throw` statement whose unwind target was the arm's own: an exception
// leaving the arm through an inner `try` whose arms do not match, or raised by
// an awaited task, never ended the handler-owned instance's handling at all
// (MEASURED: `xgone=0`, a LeakSanitizer report).
//
// Now each handled region (an arm body, a `when` guard, a finally body entered
// on an exception edge) has ONE leave block that every exception edge out of
// the region passes through; it ends the caught instance's handling exactly
// once: a task-owned instance whose owner is alive is dropped undisposed, one
// whose owner's disposal was deferred (§30.9) is disposed there, a
// handler-owned one is disposed (§31.1).
//
// The leave block runs AFTER the region's inner scopes have unwound, and an
// `await using` binding among them awaits its DisposeAsync on that edge
// (§28.12, v0.357): a suspension. The slot the leave block reads is therefore
// frame-backed in every async state machine, not only in a region that
// contains an `await` expression (the "await_using" rows at the end).
//
// Oracle: `MyEx` counts constructions (`xmade`) and destructions (`xgone`).
// Every row loops 100x under generated-code ASan (runAotChecked) and asserts
// the observable catches plus `xmade == xgone == 100`: disposed EXACTLY once.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr const char* kPrelude = R"BZ(module M;
public class XC
  public static made: int := 0;
  public static gone: int := 0;
  public constructor() end constructor;
end XC;
public class MyEx: Exception
  public constructor(m: string) base(m); XC.made := XC.made + 1; end constructor;
  destructor XC.gone := XC.gone + 1; end destructor;
end MyEx;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class K
  public static oda: int := 0;
  public static odt: int := 0;
  public static boom: int := 0;
  public static fboom: int := 0;
  public static inner: int := 0;
  public static other: int := 0;
  public static mark: int := 0;
  public constructor() end constructor;
end K;
public class NOK: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(1);
    K.oda := K.oda + 1;
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NOK;
public class NFAULT: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(1);
    K.oda := K.oda + 1;
    throw new ArgumentException("fboom");
  end DisposeAsync;
  destructor K.odt := K.odt + 1; end destructor;
end NFAULT;
public class DThrow
  public constructor() end constructor;
  destructor
    throw new ArgumentException("inner");
  end destructor;
end DThrow;
public class CThrow
  public constructor()
    throw new ArgumentException("inner");
  end constructor;
end CThrow;
public class H
  public owned t: Task;
  public constructor(owned tk: Task) t := tk; end constructor;
end H;
public class Program
  public static method Boom(): int
    throw new ArgumentException("inner");
  end Boom;
  public static method GuardBoom(): bool
    throw new ArgumentException("inner");
  end GuardBoom;
  public static async method BoomAsync(): Task
    await Task.Delay(1);
    throw new MyEx("boom");
  end BoomAsync;
  public static async method InnerAsync(): Task
    await Task.Delay(1);
    throw new ArgumentException("inner");
  end InnerAsync;
  public static async method BoomIntAsync(): Task<int>
    await Task.Delay(1);
    throw new MyEx("boom");
  end BoomIntAsync;
  public static async method BoomNowAsync(): Task<int>
    throw new MyEx("boom");
  end BoomNowAsync;
  public static method SyncRead(t: Task<int>): int
    return t.Result;
  end SyncRead;
  public static method SyncOwn(): int
    var t := Program.BoomNowAsync();
    return t.Result;
  end SyncOwn;
  public static method SyncNested(): int
    var t := Program.BoomNowAsync();
    try
      var x := t.Result;
    catch var e: MyEx =>
      do
        try
          var y := t.Result;
        catch var e2: MyEx =>
          K.mark := K.mark + 1;
        end;
        Program.Tally(e.Message);
      end;
    end;
    return 0;
  end SyncNested;
  public static method Tally(m: string)
    if m = "boom" then K.boom := K.boom + 1;
    elif m = "fboom" then K.fboom := K.fboom + 1;
    elif m = "inner" then K.inner := K.inner + 1;
    else K.other := K.other + 1; end;
  end Tally;
)BZ";

void expectOut(const char* programTail, const char* want) {
    bezel::test::JitResult r =
        runAotChecked(std::string(kPrelude) + programTail);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

}  // namespace

// The issue's repro (q33): a fresh `throw` from the arm supersedes the
// task-owned instance; `t` still owns it and `await t` re-raises it live.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_q33_fresh_throw_from_arm_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      catch var e: MyEx =>
        do
          throw new ArgumentException("inner");
        end;
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The superseding exception comes from a propagating call, not a `throw`
// statement.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_call_in_arm_throws_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      catch var e: MyEx =>
        do
          var k := Program.Boom();
        end;
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... from a constructor that throws.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_constructor_in_arm_throws_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      catch var e: MyEx =>
        do
          using var c := new CThrow();
        end;
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// A `throw` in a finally body entered on the exception edge supersedes the
// in-flight task-owned instance.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_finally_throw_on_exception_edge_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      finally
        throw new ArgumentException("inner");
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... a propagating call in that finally body.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_finally_call_throws_on_exception_edge_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      finally
        var k := Program.Boom();
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The destructor-parked supersede: a `using var` destructor throws while
// the task-owned instance unwinds.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_destructor_throws_during_unwind_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      using var d := new DThrow();
      await t;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// §31.2.1: a `when` guard that throws supersedes the in-flight instance.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_1_when_guard_throws_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      catch var e: MyEx when Program.GuardBoom() =>
        K.other := K.other + 1;
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// §28.12: an `await using` head whose DisposeAsync faults on the exception
// edge supersedes the task-owned instance (clean on base; pinned so the
// rewritten unwind tail keeps it).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_await_using_faulting_dispose_async_supersedes_borrowed_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await using var o := new NFAULT();
      await t;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// §30.9 refinement: the owner was disposed while its fault was being
// handled, so the END of that handling — here by a fresh throw — performs
// the deferred disposal, exactly once.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_30_9_deferred_owner_disposal_runs_when_arm_throws_new) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      try
        var t := Program.BoomAsync();
        await t;
      catch var e: MyEx =>
        do
          throw new ArgumentException("inner");
        end;
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The issue's "also unmeasured" case, MEASURED a leak on base: an awaited
// task's fault leaves the arm of a HANDLER-OWNED instance (§31.1: disposed
// after the arm completes "by a fresh `throw` from it").
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_1_awaited_fault_leaving_arm_disposes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      await Program.BoomAsync();
    catch var e: MyEx =>
      do
        await Program.InnerAsync();
      end;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=100 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The same edge with a task-owned caught instance: dropped undisposed, the
// owner disposes it once.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_other_bound_fault_leaving_arm_drops_borrowed_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    var t2 := Program.InnerAsync();
    try
      try
        await t;
      catch var e: MyEx =>
        do
          await t2;
        end;
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    try await t2; catch var e: ArgumentException => K.mark := K.mark + 1; end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=200 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Control: a handler-owned instance superseded by a fresh `throw` is still
// disposed exactly once.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_1_control_handler_owned_instance_arm_throws_new) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      try
        await Program.BoomAsync();
      catch var e: MyEx =>
        do
          throw new ArgumentException("inner");
        end;
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// A fresh throw that leaves the arm THROUGH an inner `try` whose arms do
// not match (MEASURED a leak on base: the per-throw-site test saw the inner
// dispatch as the throw's target).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_1_throw_through_nonmatching_inner_try_disposes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      try
        await Program.BoomAsync();
      catch var e: MyEx =>
        do
          try
            throw new ArgumentException("inner");
          catch var z: InvalidOperationException =>
            K.other := K.other + 1;
          end;
        end;
      end;
    catch var e9: ArgumentException =>
      K.mark := K.mark + 1;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Its task-owned twin: dropped undisposed, disposed once by the owner.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_throw_through_nonmatching_inner_try_drops_borrowed_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      catch var e: MyEx =>
        do
          try
            throw new ArgumentException("inner");
          catch var z: InvalidOperationException =>
            K.other := K.other + 1;
          end;
        end;
      end;
    catch var e9: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... through an inner try/finally whose finally throws (a leak on base).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_1_throw_through_inner_finally_disposes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      try
        await Program.BoomAsync();
      catch var e: MyEx =>
        do
          try
            throw new ArgumentException("inner");
          finally
            K.fboom := K.fboom + 1;
          end;
        end;
      end;
    catch var e9: ArgumentException =>
      K.mark := K.mark + 1;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=100 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Its task-owned twin.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_throw_through_inner_finally_drops_borrowed_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      catch var e: MyEx =>
        do
          try
            throw new ArgumentException("inner");
          finally
            K.fboom := K.fboom + 1;
          end;
        end;
      end;
    catch var e9: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=100 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... whose finally makes a propagating call (a leak on base).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_1_call_through_inner_finally_disposes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      try
        await Program.BoomAsync();
      catch var e: MyEx =>
        do
          try
            var k := Program.Boom();
          finally
            K.fboom := K.fboom + 1;
          end;
        end;
      end;
    catch var e9: ArgumentException =>
      K.mark := K.mark + 1;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=100 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Its task-owned twin.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_call_through_inner_finally_drops_borrowed_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      catch var e: MyEx =>
        do
          try
            var k := Program.Boom();
          finally
            K.fboom := K.fboom + 1;
          end;
        end;
      end;
    catch var e9: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=100 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// A `using var` local inside the arm is discharged at the throw site,
// before the leave block ends the caught instance's handling.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_1_arm_throw_with_using_local_disposes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      try
        await Program.BoomAsync();
      catch var e: MyEx =>
        do
          using var c := new Cell();
          throw new ArgumentException("inner");
        end;
      end;
    catch var e9: ArgumentException =>
      K.mark := K.mark + 1;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Its task-owned twin, then a re-await (a UAF on base).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_arm_throw_with_using_local_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      catch var e: MyEx =>
        do
          using var c := new Cell();
          throw new ArgumentException("inner");
        end;
      end;
    catch var e9: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The arm's try has its own finally: the leave block continues to it (a
// UAF on base).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_inner_finally_in_arm_and_finally_on_outer_try_drops_borrowed_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      catch var e: MyEx =>
        do
          throw new ArgumentException("inner");
        end;
      finally
        K.fboom := K.fboom + 1;
      end;
    catch var e9: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=100 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Control: a handler-owned instance, arm throw, finally on the same try.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_1_arm_throw_with_outer_finally_disposes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      try
        await Program.BoomAsync();
      catch var e: MyEx =>
        do
          throw new ArgumentException("inner");
        end;
      finally
        K.fboom := K.fboom + 1;
      end;
    catch var e9: ArgumentException =>
      K.mark := K.mark + 1;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=100 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// An `await using` binding inside the arm: the throw's unwind awaits its
// DisposeAsync (§28.12) BEFORE the arm's leave block ends the caught
// instance's handling, so the leave block reads its slot after a suspension
// no `await` expression announces. The slot is frame-backed in every async
// state machine. (Clean on base, whose supersede ran at the throw site; a
// SEGV in the Resume function with a stack-only slot.)
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_28_12_arm_await_using_then_fresh_throw_disposes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      throw new MyEx("boom");
    catch var e: MyEx =>
      do
        await using var o := new NOK();
        throw new ArgumentException("inner");
      end;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=100 other=0 mark=0 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... a bare `throw;` leaves the arm: the same instance re-raised, live.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_28_12_arm_await_using_then_bare_rethrow_keeps_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      throw new MyEx("boom");
    catch var e: MyEx =>
      do
        await using var o := new NOK();
        throw;
      end;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... a propagating call leaves the arm.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_28_12_arm_await_using_then_call_throws_disposes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      throw new MyEx("boom");
    catch var e: MyEx =>
      do
        await using var o := new NOK();
        var k := Program.Boom();
      end;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=100 other=0 mark=0 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... the binding sits in an inner finally body the arm ran to completion
// before its fresh throw.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_28_12_arm_inner_finally_await_using_then_fresh_throw_disposes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      throw new MyEx("boom");
    catch var e: MyEx =>
      do
        try
          K.mark := K.mark + 1;
        finally
          await using var o := new NOK();
        end;
        throw new ArgumentException("inner");
      end;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=100 other=0 mark=100 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... the binding's DisposeAsync itself faults on that edge and supersedes
// the arm's fresh throw (§28.12, §31.2).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_28_12_arm_await_using_faulting_dispose_async_supersedes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      throw new MyEx("boom");
    catch var e: MyEx =>
      do
        await using var o := new NFAULT();
        throw new ArgumentException("inner");
      end;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=100 inner=0 other=0 mark=0 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The task-owned twin, then a re-await (a UAF on base).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_arm_await_using_then_fresh_throw_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      catch var e: MyEx =>
        do
          await using var o := new NOK();
          throw new ArgumentException("inner");
        end;
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The arm re-awaits the bound task whose fault it is handling, with an
// `await using` binding in scope: the same instance leaves the arm live.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_30_4_arm_await_using_then_reawait_same_bound_task) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    catch var e: MyEx =>
      do
        await using var o := new NOK();
        await t;
      end;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The arm completes normally: its fall-through disposal reads the same
// slot after the binding's scope-end DisposeAsync await (MEASURED a UAF on
// base: the slot was frame-backed only for an arm with an `await`
// expression).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_1_arm_await_using_normal_exit_disposes_handler_owned_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      throw new MyEx("boom");
    catch var e: MyEx =>
      do
        await using var o := new NOK();
        K.mark := K.mark + 1;
      end;
    end;
    await Task.Delay(0);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=0 other=0 mark=100 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Its task-owned twin, then a re-await (a UAF on base).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_arm_await_using_normal_exit_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    catch var e: MyEx =>
      do
        await using var o := new NOK();
        K.mark := K.mark + 1;
      end;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The same binding in a finally body entered on the exception edge: the
// finally's fresh throw supersedes the in-flight instance (§31.2) after the
// binding's DisposeAsync await.
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_28_12_finally_await_using_then_fresh_throw_on_exception_edge) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      throw new MyEx("boom");
    finally
      await using var o := new NOK();
      throw new ArgumentException("inner");
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=100 other=0 mark=0 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... the finally completes normally and the in-flight instance resumes
// propagating (MEASURED a UAF on base: the captured in-flight slot of a
// finally with no `await` expression was stack-only).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_28_12_finally_await_using_normal_completion_resumes_in_flight_instance) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      throw new MyEx("boom");
    finally
      await using var o := new NOK();
      K.mark := K.mark + 1;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... a task-owned in-flight instance superseded there, then a re-await
// (a UAF on base).
BEZEL_TEST(superseded_task_owned_fault_7940,
           spec_31_2_finally_await_using_then_fresh_throw_drops_borrowed_instance_then_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      finally
        await using var o := new NOK();
        throw new ArgumentException("inner");
      end;
    catch var e: ArgumentException =>
      K.mark := K.mark + 1;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: Exception =>
        Program.Tally(e.Message);
      end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}
