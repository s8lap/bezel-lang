// Codegen + runtime — issue #7954 (§30.9 / §30.4 / §31.2, v0.357): the handlings
// of ONE task-owned fault instance are COUNTED. Each raise of the instance
// takes a hold, each end of handling releases one, and the instance's owner
// defers its disposal while any hold is out.
//
//     var t := BoomAsync();
//     try
//       await t;
//     catch var e: Exception =>
//       do
//         try await t; catch var e2: Exception => ... end;   // same instance
//         Tally(e.Message);              // e is still being handled here
//       end;
//     end;
//
// §30.9, v0.357, verbatim: "Awaiting a faulted task (or reading a faulted
// `Result`) re-raises the task-owned instance; the catching arm's binding is a
// borrow of it"; "a task disposed while its own fault exception is in flight
// defers that disposal until the exception's handling completes"; and, for a
// fault that escapes into a task, "the faulted task carries it as a borrow
// [...] The retained task is disposed with the faulted task, and the instance
// with it, transitively, exactly once; every awaiter of a multi-awaited
// faulted task re-raises the same live instance." §30.4, v0.357: a task may be
// awaited more than once, by more than one awaiter.
//
// The "this instance is being handled" mark was a SET: the first handling to
// end erased it for every other handling of the same instance. So a nested arm
// that re-awaited the same bound task, a second concurrent awaiter, or the
// first awaiter of a faulted task that itself only borrows the instance, ended
// the handling for all of them — the owner's deferred disposal ran while an
// outer arm, another awaiter, or the faulted task still referred to the
// instance (MEASURED on r30-c1 4a6919f1: the issue's repros n02 and n16 are
// ASan heap-use-after-free).
//
// Now the mark is a count of holds (bz_mark_exc_borrowed / bz_exc_reraise take
// one, bz_exc_end_handling / bz_exc_leave_handling release one), the release
// of the LAST hold performs a deferred disposal, and a fault that escapes a
// frame moves the frame's hold into the faulted task, which releases it at its
// teardown (bz_task_destroy) — so the instance outlives every awaiter of that
// task. A bare `throw;` (§31.2.4) is a raise and takes a hold of its own.
//
// Oracle: `MyEx` counts constructions (`xmade`) and destructions (`xgone`).
// Every row loops 100x under generated-code ASan (runAotChecked) and asserts
// the observable catches plus `xmade == xgone`: disposed EXACTLY once.

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

// The issue's repro (n02): the nested arm's handling ends first; the outer
// arm still reads the instance.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_n02_nested_arm_reawaits_same_bound_task) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    catch var e: Exception =>
      do
        try
          await t;
        catch var e2: Exception =>
          K.mark := K.mark + 1;
        end;
        Program.Tally(e.Message);
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
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The issue's second repro (n16): two concurrent awaiters of one faulted
// task, their arms suspended across each other.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_4_n16_two_concurrent_borrowers_of_one_task) {
    expectOut(R"BZ(  public static async method W(t: Task, d: int): Task
    try
      await t;
    catch var e: Exception =>
      do
        await Task.Delay(d);
        Program.Tally(e.Message);
      end;
    end;
  end W;
  public static async method R(i: int): Task
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t := Program.BoomAsync();
      var a := Program.W(t, 1);
      var b := Program.W(t, 3);
      await a;
      await b;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Three nested handlings of the one instance.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_three_level_nested_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    catch var e: Exception =>
      do
        try
          await t;
        catch var e2: Exception =>
          do
            try
              await t;
            catch var e3: Exception =>
              K.mark := K.mark + 1;
            end;
            Program.Tally(e2.Message);
          end;
        end;
        Program.Tally(e.Message);
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
              "boom=200 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The nested arm is left by a fresh throw (§31.2: the supersession ends
// THAT handling); the outer arm goes on, and the task is awaited again.
BEZEL_TEST(counted_exception_holds_7954,
           spec_31_2_nested_reawait_inner_arm_throws_new) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    catch var e: Exception =>
      do
        try
          try
            await t;
          catch var e2: MyEx =>
            do
              throw new ArgumentException("inner");
            end;
          end;
        catch var e3: ArgumentException =>
          K.mark := K.mark + 1;
        end;
        Program.Tally(e.Message);
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
              "boom=200 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// A finally body entered on the instance's own exception edge awaits the
// task again and handles it; the parked instance then resumes its flight.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_finally_reawaits_same_task_on_exception_edge) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      try
        await t;
      finally
        try
          await t;
        catch var e: Exception =>
          K.mark := K.mark + 1;
        end;
      end;
    catch var e2: Exception =>
      Program.Tally(e2.Message);
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
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Three concurrent awaiters; one leaves its arm by a bare `throw;`, which
// escapes into its task.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_4_three_concurrent_borrowers_one_rethrows) {
    expectOut(R"BZ(  public static async method W(t: Task, d: int): Task
    try
      await t;
    catch var e: Exception =>
      do
        await Task.Delay(d);
        Program.Tally(e.Message);
      end;
    end;
  end W;
  public static async method W2(t: Task): Task
    try
      await t;
    catch var e: Exception =>
      do
        await Task.Delay(2);
        throw;
      end;
    end;
  end W2;
  public static async method R(i: int): Task
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t := Program.BoomAsync();
      var a := Program.W(t, 3);
      var b := Program.W(t, 1);
      var c := Program.W2(t);
      await a;
      await b;
      try await c; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=300 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The nested arm is left by `continue` and by `break` while the outer arm
// holds the instance.
BEZEL_TEST(counted_exception_holds_7954,
           spec_31_1_nested_arm_left_by_continue_and_break) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    catch var e: Exception =>
      do
        for var j in 0..<3 do
          try
            await t;
          catch var e2: Exception =>
            do
              K.mark := K.mark + 1;
              if j = 1 then continue; end;
              if j = 2 then break; end;
            end;
          end;
        end for;
        Program.Tally(e.Message);
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
              "boom=100 fboom=0 inner=0 other=0 mark=300 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// A bare `throw;` caught by a nested arm INSIDE the arm that rethrew: two
// handlings of one instance, the inner ends first.
BEZEL_TEST(counted_exception_holds_7954,
           spec_31_2_4_bare_rethrow_caught_inside_arm_task_owned) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    catch var e: MyEx =>
      do
        try
          throw;
        catch var e2: MyEx =>
          K.mark := K.mark + 1;
        end;
        Program.Tally(e.Message);
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
              "boom=200 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The same with a handler-owned instance (§31.1): it is disposed when the
// LAST of its two handlings ends, not the first.
BEZEL_TEST(counted_exception_holds_7954,
           spec_31_2_4_bare_rethrow_caught_inside_arm_handler_owned) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      await Program.BoomAsync();
    catch var e: MyEx =>
      do
        try
          throw;
        catch var e2: MyEx =>
          K.mark := K.mark + 1;
        end;
        Program.Tally(e.Message);
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
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... and the outer arm is then left by a fresh throw.
BEZEL_TEST(counted_exception_holds_7954,
           spec_31_2_4_bare_rethrow_handler_owned_caught_inside_then_arm_throws_new) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    try
      try
        await Program.BoomAsync();
      catch var e: MyEx =>
        do
          try
            throw;
          catch var e2: MyEx =>
            K.mark := K.mark + 1;
          end;
          Program.Tally(e.Message);
          throw new ArgumentException("x");
        end;
      end;
    catch var e3: ArgumentException =>
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
              "boom=100 fboom=0 inner=0 other=0 mark=200 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// A `when` guard that reads the instance and declines; the next arm
// re-awaits the task inside its body.
BEZEL_TEST(counted_exception_holds_7954,
           spec_31_2_1_guard_false_then_next_arm_reawaits) {
    expectOut(R"BZ(  public static method Never(s: string): bool
    return s.Length > 1000;
  end Never;
  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    catch var e: MyEx when Program.Never(e.Message) =>
      K.mark := K.mark + 1000;
    catch var e2: Exception =>
      do
        try await t; catch var e3: MyEx => K.mark := K.mark + 1; end;
        Program.Tally(e2.Message);
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
              "boom=200 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The frame's own arm awaits another awaiter of the same task, then
// suspends, then reads its binding.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_4_three_awaiters_arms_suspend_inside_each_other) {
    expectOut(R"BZ(  public static async method W(t: Task, d: int): Task
    try
      await t;
    catch var e: Exception =>
      do
        await Task.Delay(d);
        Program.Tally(e.Message);
      end;
    end;
  end W;
  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    var a := Program.W(t, 2);
    var b := Program.W(t, 0);
    var c := Program.W(t, 1);
    try
      await t;
    catch var e: MyEx =>
      do
        await a;
        await Task.Delay(1);
        Program.Tally(e.Message);
      end;
    end;
    await b;
    await c;
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
              "boom=400 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The owning task is handed to Task.FireAndForget from inside the arm that
// is handling its fault: the owner's disposal defers to the arm's end.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_owner_fire_and_forgotten_inside_arm_handling_its_fault) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    catch var e: MyEx =>
      do
        Task.FireAndForget(move t);
        await Task.Delay(0);
        Program.Tally(e.Message);
        return;
      end;
    end;
    Task.FireAndForget(move t);
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
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// A fault that escaped a frame, its operand a plain local disposed at the
// frame's exit: the faulted task is awaited twice — the same live instance
// both times, disposed with the faulted task.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_escaped_fault_bound_frame_task_awaited_twice) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R(i);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The escape repeated through a second frame ("transitively").
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_two_level_escape_chain_awaited_twice) {
    expectOut(R"BZ(  public static async method R2(i: int): Task
    var u := Program.R(i);
    await u;
  end R2;
  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R2(i);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The faulted task is never awaited: its teardown releases the last hold
// and the deferred disposal runs there (a leak on base).
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_escaped_fault_frame_task_never_awaited) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R(i);
      await Task.Delay(3);
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=0 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The instance escapes the frame by a bare `throw;` from the arm.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_escape_by_bare_rethrow_awaited_twice) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    catch var e: Exception =>
      do
        K.mark := K.mark + 1;
        throw;
      end;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R(i);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The operand is a `using var` local.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_using_var_operand_escape_awaited_twice) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    using var t := Program.BoomAsync();
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R(i);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The operand is an `owned` parameter, disposed AFTER the capture.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_owned_parameter_operand_escape_awaited_twice) {
    expectOut(R"BZ(  public static async method OP(owned t: Task): Task
    await t;
  end OP;
  public static async method R(i: int): Task
    await Task.Delay(0);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.OP(Program.BoomAsync());
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The operand is an owned field of a `using var` holder.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_owned_field_operand_escape_awaited_twice) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    using var h := new H(Program.BoomAsync());
    await h.t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R(i);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// `return await t;` in a Task<int> frame.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_return_await_operand_escape_awaited_twice) {
    expectOut(R"BZ(  public static async method RI(i: int): Task<int>
    var t := Program.BoomIntAsync();
    return await t;
  end RI;
  public static async method R(i: int): Task
    await Task.Delay(0);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.RI(i);
      try var x := await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try var y := await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The frame is an async lambda (its own fault boundary).
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_async_lambda_escape_awaited_twice) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    await Task.Delay(0);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var f := async lambda(): Task => do
        var t := Program.BoomAsync();
        await t;
      end;
      var t1 := f();
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Two concurrent awaiters of the task that carries the escaped instance.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_4_escaped_fault_two_concurrent_borrowers) {
    expectOut(R"BZ(  public static async method W(t: Task, d: int): Task
    try
      await t;
    catch var e: Exception =>
      do
        await Task.Delay(d);
        Program.Tally(e.Message);
      end;
    end;
  end W;
  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R(i);
      var a := Program.W(t1, 1);
      var b := Program.W(t1, 3);
      await a;
      await b;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Three frames borrow one faulted task as a parameter, each with an
// `await using` local (§28.12), and each faults with its instance.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_4_borrowed_parameter_escape_three_frames_concurrent) {
    expectOut(R"BZ(  public static async method V(t: Task): Task
    await using var o := new NOK();
    await t;
  end V;
  public static async method R(i: int): Task
    await Task.Delay(0);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t := Program.BoomAsync();
      var a := Program.V(t);
      var b := Program.V(t);
      var c := Program.V(t);
      try await a; catch var e: Exception => Program.Tally(e.Message); end;
      try await b; catch var e: Exception => Program.Tally(e.Message); end;
      try await c; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=300 fboom=0 inner=0 other=0 mark=0 oda=300 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The operand was owned by a synchronous callee that read its `Result` and
// disposed it in its own unwind.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_sync_callee_disposed_operand_escape_awaited_twice) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    await Task.Delay(0);
    var x := Program.SyncOwn();
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R(i);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Nested `Result` re-reads of one faulted task in synchronous arms.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_sync_nested_result_reread) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    await Task.Delay(0);
    var x := Program.SyncNested();
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

// A TaskCompletionSource's task, re-awaited inside its own arm.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_completion_source_task_nested_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    await Task.Delay(0);
    using var tcs := new TaskCompletionSource<int>();
    tcs.SetException(new MyEx("boom"));
    try
      var x := await tcs.Task;
    catch var e: MyEx =>
      do
        try
          var y := await tcs.Task;
        catch var e2: MyEx =>
          K.mark := K.mark + 1;
        end;
        Program.Tally(e.Message);
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
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// ... and escaping the frame that owns the source.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_completion_source_task_escape_awaited_twice) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    await Task.Delay(0);
    using var tcs := new TaskCompletionSource<int>();
    tcs.SetException(new MyEx("boom"));
    var x := await tcs.Task;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R(i);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// §30.10: the WhenAll aggregate borrows an input's instance; the input is
// re-awaited inside the aggregate's arm.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_10_whenall_aggregate_then_inputs_reawait) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var a := Program.BoomAsync();
    var b := Program.BoomAsync();
    using var tasks := new Array<Task>();
    tasks.Add(a);
    tasks.Add(b);
    try
      await Task.WhenAll(tasks);
    catch var e: MyEx =>
      do
        try
          await a;
        catch var e2: MyEx =>
          K.mark := K.mark + 1;
        end;
        Program.Tally(e.Message);
      end;
    end;
    try await b; catch var e3: MyEx => K.mark := K.mark + 1; end;
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
              "boom=100 fboom=0 inner=0 other=0 mark=200 oda=0 odt=0 cells=0 xmade=200 xgone=200\n");
}

// The aggregate's borrowed fault escapes the frame.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_10_whenall_fault_escape_awaited_twice) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var a := Program.BoomAsync();
    var b := Program.BoomAsync();
    using var tasks := new Array<Task>();
    tasks.Add(a);
    tasks.Add(b);
    await Task.WhenAll(tasks);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R(i);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=200 xgone=200\n");
}

// WhenAll over tasks that themselves only carry an escaped instance.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_10_whenall_over_tasks_carrying_escaped_faults_then_inputs) {
    expectOut(R"BZ(  public static async method R2(i: int): Task
    var t := Program.BoomAsync();
    await t;
  end R2;
  public static async method R(i: int): Task
    var a := Program.R2(i);
    var b := Program.R2(i + 1);
    using var tasks := new Array<Task>();
    tasks.Add(a);
    tasks.Add(b);
    try
      await Task.WhenAll(tasks);
    catch var e: MyEx =>
      K.mark := K.mark + 1;
    end;
    try await b; catch var e3: MyEx => K.mark := K.mark + 1; end;
    await a;
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
              "boom=100 fboom=0 inner=0 other=0 mark=200 oda=0 odt=0 cells=0 xmade=200 xgone=200\n");
}

// WhenEach over such tasks, then one awaited again.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_10_wheneach_over_tasks_carrying_escaped_faults) {
    expectOut(R"BZ(  public static async method R2(i: int): Task
    var t := Program.BoomAsync();
    await t;
  end R2;
  public static async method R(i: int): Task
    var a := Program.R2(i);
    var b := Program.R2(i + 1);
    using var tasks := new Array<Task>();
    tasks.Add(a);
    tasks.Add(b);
    await for var tk in Task.WhenEach(tasks) do
      try
        await tk;
      catch var e: MyEx =>
        K.mark := K.mark + 1;
      end;
    end;
    await a;
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
              "boom=100 fboom=0 inner=0 other=0 mark=200 oda=0 odt=0 cells=0 xmade=200 xgone=200\n");
}

// WhenAllOwned consumes inputs that carry an escaped instance: the
// aggregate takes the input's hold with the fault, and is awaited twice.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_10_whenallowned_aggregate_takes_the_input_hold_awaited_twice) {
    expectOut(R"BZ(  public static async method RO(i: int): OwningTask<Cell>
    var t := Program.BoomAsync();
    await t;
    return new Cell();
  end RO;
  public static async method R(i: int): Task
    var tasks := new OwningArray<OwningTask<Cell>>();
    tasks.Add(Program.RO(i));
    tasks.Add(Program.RO(i + 1));
    using var wa := Task.WhenAllOwned(move tasks);
    try
      using var arr := await wa;
    catch var e: MyEx =>
      K.mark := K.mark + 1;
    end;
    using var arr2 := await wa;
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
              "boom=0 fboom=0 inner=0 other=100 mark=100 oda=0 odt=0 cells=0 xmade=200 xgone=200\n");
}

// Three awaits and a `Result` read of the task carrying the instance.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_escaped_fault_awaited_three_times_and_result) {
    expectOut(R"BZ(  public static async method RI(i: int): Task<int>
    var t := Program.BoomIntAsync();
    var v := await t;
    return v + 1;
  end RI;
  public static async method R(i: int): Task
    await Task.Delay(0);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.RI(i);
      try var x := await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try var y := await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try var z := Program.SyncRead(t1); catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=300 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// WhenAny over such tasks, then each awaited.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_10_whenany_over_tasks_carrying_escaped_faults) {
    expectOut(R"BZ(  public static async method R2(i: int): Task
    var t := Program.BoomAsync();
    await t;
  end R2;
  public static async method R(i: int): Task
    var a := Program.R2(i);
    var b := Program.R2(i + 1);
    using var tasks := new Array<Task>();
    tasks.Add(a);
    tasks.Add(b);
    var idx := await Task.WhenAny(tasks);
    try await a; catch var e: MyEx => K.mark := K.mark + 1; end;
    try await b; catch var e: MyEx => K.mark := K.mark + 1; end;
    await a;
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
              "boom=100 fboom=0 inner=0 other=0 mark=200 oda=0 odt=0 cells=0 xmade=200 xgone=200\n");
}

// The escaping instance passes a finally body on its way out.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_escape_through_finally_awaited_twice) {
    expectOut(R"BZ(  public static async method RF(i: int): Task
    var t := Program.BoomAsync();
    try
      await t;
    finally
      K.mark := K.mark + 1;
    end;
  end RF;
  public static async method R(i: int): Task
    await Task.Delay(0);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.RF(i);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// A Domain.Spawn entry: a nested arm catches the bare rethrow of a
// handler-owned instance, the outer arm rethrows it again, and the domain's
// fault wrap (§30.14) ends the last handling.
BEZEL_TEST(counted_exception_holds_7954,
           spec_31_2_4_domain_entry_nested_arm_catches_bare_rethrow_then_rethrows) {
    expectOut(R"BZ(  public static method SBoom(): int
    throw new MyEx("boom");
  end SBoom;
  public static method EntryNested(): int
    try
      return Program.SBoom();
    catch var e: MyEx =>
      do
        try
          throw;
        catch var e2: MyEx =>
          Program.Tally(e2.Message);
        end;
        Program.Tally(e.Message);
        throw;
      end;
    end;
  end EntryNested;
  public static async method R(i: int): Task
    using var j := Domain.Spawn(Program.EntryNested);
    try
      var x := await j;
    catch var e: DomainFaultedException =>
      K.mark := K.mark + 1;
    end;
    var t := Program.BoomAsync();
    try await t; catch var e2: MyEx => Program.Tally(e2.Message); end;
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
              "boom=300 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=200 xgone=200\n");
}

// Kept control (review r42-rev-f1-7943 F2; green on the parent). A
// synchronous bare rethrow caught by the caller. Wrong fix it catches: a
// bare `throw;` that takes no hold of its own (the count is released once
// per end-of-handling, so the rethrow's release underflows) -> the
// handler-owned instance is disposed twice or too early.
BEZEL_TEST(counted_exception_holds_7954,
           spec_31_2_4_sync_bare_rethrow_of_handler_owned_instance) {
    expectOut(R"BZ(  public static method SBoom(): int
    throw new MyEx("boom");
  end SBoom;
  public static method SR()
    try
      var x := Program.SBoom();
    catch var e: MyEx =>
      throw;
    end;
  end SR;
  public static async method R(i: int): Task
    await Task.Delay(0);
    try
      Program.SR();
    catch var e: MyEx =>
      Program.Tally(e.Message);
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
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Kept control (review r42-rev-f1-7943 F2; green on the parent). A TaskScope
// child faults with an instance its operand owns; the scope re-raises it.
// Wrong fix it catches: moving the frame's hold into the faulted task
// without releasing it at the task's teardown (bz_task_destroy) -> the
// count never reaches zero, the deferred disposal never runs (xgone < xmade).
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_taskscope_child_escaped_fault) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    await using var scope := new TaskScope();
    scope.Spawn(async lambda() => do
      var t := Program.BoomAsync();
      await t;
    end);
    await scope.Join();
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
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// §30.5 kind 2 chained return whose anonymous source itself carries a borrow
// (its own operand is a plain local): the hold travels through both tasks.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_chained_return_source_carries_a_borrow_awaited_twice) {
    expectOut(R"BZ(  public static async method BoomCellAsync(): Task<Cell>
    await Task.Delay(1);
    throw new MyEx("boom");
  end BoomCellAsync;
  public static async method BoomCellNowAsync(): Task<Cell>
    throw new MyEx("boom");
  end BoomCellNowAsync;
  public static method Use(c: Cell)
    K.mark := K.mark + 1;
  end Use;
  public static async method Mid(): Task<Cell>
    var p := Program.BoomCellAsync();
    return await p;
  end Mid;
  public static async method Q(): Task<Cell>
    return await Program.Mid();
  end Q;
  public static async method R(i: int): Task
    var t := Program.Q();
    try
      var a := await t;
    catch var e: Exception =>
      K.mark := K.mark + 1;
    end;
    var b := await t;
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

// A nested catch arm awaits a chained faulted task again: the two handlings
// each hold the instance the faulted task owns.
BEZEL_TEST(counted_exception_holds_7954,
           spec_30_9_chained_return_bound_nested_arm_reawait) {
    expectOut(R"BZ(  public static async method BoomCellAsync(): Task<Cell>
    await Task.Delay(1);
    throw new MyEx("boom");
  end BoomCellAsync;
  public static async method BoomCellNowAsync(): Task<Cell>
    throw new MyEx("boom");
  end BoomCellNowAsync;
  public static method Use(c: Cell)
    K.mark := K.mark + 1;
  end Use;
  public static async method Q(): Task<Cell>
    return await Program.BoomCellNowAsync();
  end Q;
  public static async method R(i: int): Task
    var t := Program.Q();
    try
      var a := await t;
    catch var e: Exception =>
      do
        try
          var b := await t;
        catch var e2: Exception =>
          K.mark := K.mark + 1;
        end;
        Program.Tally(e.Message);
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
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}
