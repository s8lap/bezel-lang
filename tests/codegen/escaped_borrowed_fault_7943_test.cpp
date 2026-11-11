// Codegen — issues #7943 / #7886 (§30.9, v0.357): a fault that ESCAPES a frame
// into the frame's task is carried by that task as a BORROW of the instance the
// operand task owns; the faulted task never becomes its owner.
//
//     async method R(i: int): Task
//       var t := BoomAsync();
//       await using var o := new NOK();
//       await t;                       // t's own fault escapes R into R's task
//     end R;
//     ... var t1 := R(1); var t2 := R(2); ...
//     try await t1; catch var e: Exception => ... end;
//
// §30.9, v0.357, *A fault that escapes into a task is retained through its
// operand task*, verbatim: "the instance stays owned by the task that owned
// it, the faulted task carries it as a borrow"; "The capture ends the frame's
// handling of the instance"; "The retained task is disposed with the faulted
// task, and the instance with it, transitively, exactly once; every awaiter of
// a multi-awaited faulted task re-raises the same live instance."
//
// The fault boundary stored the escaping instance in the frame's task as if
// the task OWNED it. The operand `t`, disposed at the frame's exit while its
// instance was in flight, had deferred its disposal (§30.9) "until the
// exception's handling completes" — and the first handler of the faulted task
// then drained that deferral while the faulted task still pointed at the
// instance. MEASURED on r30-c1 4a6919f1: the issues' repros r25 / r26 / r27
// are ASan heap-use-after-free, and so is the one-task minimum below
// (`single_bound_operand`): the defect is the escape, not the concurrency the
// titles name. #7886 names the same repro files as #7943.
//
// Now the three fault boundaries (async method, async lambda, async iterator)
// record an escaping task-owned instance as borrowed by the faulted task
// (bz_task_complete_escaped_fault).
//
// The escaping instance is recognized by the hold its re-raise took, so every
// await that CONSUMES its operand must raise the instance unheld. A chained
// return's anonymous source (`return await Producer()`, §30.5 Pattern A kind
// 2) is such an operand — "An operand the frame owns — an unbound temporary
// (`await Inner()`) [...] moves into the faulted task's state as a kind-2
// chained source does" — but its await marked it unconditionally while
// reclaiming the source on the fault edge (#2750): the instance read as owned
// by a task that no longer existed, and with the borrow rule above nothing
// disposed it (MEASURED: `chained_return_*` rows below leak without that part
// of the fix, and #2750's own tests with them). Now the chained source's
// await marks the instance only when the source itself carried it as a borrow.
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

// The issues' repro (r25, #7943 and #7886): four concurrent frames, each
// with a bound faulted operand and an `await using` local, fault into their
// tasks; Main awaits all four.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_r25_four_concurrent_bound_faults_await_using_escape) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    await using var o := new NOK();
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<25 do
      var t1 := Program.R(i);
      var t2 := Program.R(i + 1);
      var t3 := Program.R(i + 2);
      var t4 := Program.R(i + 3);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t2; catch var e: Exception => Program.Tally(e.Message); end;
      try await t3; catch var e: Exception => Program.Tally(e.Message); end;
      try await t4; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// r26: the operand's fault is superseded by a faulting DisposeAsync (§28.12),
// caught, and the operand awaited again — that second raise escapes.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_r26_four_concurrent_supersede_then_reawait_escape) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    try
      await using var o := new NFAULT();
      await t;
    catch var e: ArgumentException => do
      K.mark := K.mark + 1;
      end;
    end;
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<25 do
      var t1 := Program.R(i);
      var t2 := Program.R(i + 1);
      var t3 := Program.R(i + 2);
      var t4 := Program.R(i + 3);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t2; catch var e: Exception => Program.Tally(e.Message); end;
      try await t3; catch var e: Exception => Program.Tally(e.Message); end;
      try await t4; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=100 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// r27: the operand is a block local, disposed in the unwind before the
// fault reaches the frame's boundary (the deferred disposal, §30.9).
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_r27_four_concurrent_operand_disposed_before_escape) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    await using var o := new NOK();
    do
      var t := Program.BoomAsync();
      await t;
    end;
  end R;
  public static async method Main(): Task
    for var i in 0..<25 do
      var t1 := Program.R(i);
      var t2 := Program.R(i + 1);
      var t3 := Program.R(i + 2);
      var t4 := Program.R(i + 3);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t2; catch var e: Exception => Program.Tally(e.Message); end;
      try await t3; catch var e: Exception => Program.Tally(e.Message); end;
      try await t4; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=100 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The minimum: ONE frame, no `await using`, no concurrency.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_single_bound_operand_escape_bound_awaiter) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
    await t;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t1 := Program.R(i);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// The operand is a borrowed parameter: its owner (Main's local) outlives
// the faulted task and re-raises the same live instance afterwards.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_borrowed_parameter_operand_escape_then_owner_reawait) {
    expectOut(R"BZ(  public static async method V(t: Task): Task
    await t;
  end V;
  public static async method R(i: int): Task
    await Task.Delay(0);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t := Program.BoomAsync();
      var a := Program.V(t);
      try await a; catch var e: Exception => Program.Tally(e.Message); end;
      try await a; catch var e: Exception => Program.Tally(e.Message); end;
      try await t; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=300 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Control (clean on base): the faulted task is awaited anonymously.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_borrowed_parameter_operand_escape_anonymous_awaiter) {
    expectOut(R"BZ(  public static async method V(t: Task): Task
    await t;
  end V;
  public static async method R(i: int): Task
    await Task.Delay(0);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      var t := Program.BoomAsync();
      try await Program.V(t); catch var e: Exception => Program.Tally(e.Message); end;
      try await t; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=200 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Control (clean on base): an anonymous operand — the await consumed it,
// so the faulted task retains it (§30.9).
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_anonymous_operand_escape_anonymous_awaiter) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    await Program.BoomAsync();
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

// Control (clean on base): bound operand, anonymous awaiter of the frame.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_bound_operand_escape_anonymous_awaiter) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    var t := Program.BoomAsync();
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
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Control (clean on base): anonymous operands, four concurrent frames.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_anonymous_operand_escape_four_bound_awaiters) {
    expectOut(R"BZ(  public static async method R(i: int): Task
    await Program.BoomAsync();
  end R;
  public static async method Main(): Task
    for var i in 0..<25 do
      var t1 := Program.R(i);
      var t2 := Program.R(i + 1);
      var t3 := Program.R(i + 2);
      var t4 := Program.R(i + 3);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t2; catch var e: Exception => Program.Tally(e.Message); end;
      try await t3; catch var e: Exception => Program.Tally(e.Message); end;
      try await t4; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// §30.5 kind 2 chained return, `return await Producer()`: the source is
// reclaimed on the fault edge, its instance escapes into the frame's task.
// MEASURED on r30-c1 4a6919f1: every instance leaked (xgone=0).
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_chained_return_anonymous_source_anonymous_awaiters) {
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
    return await Program.BoomCellAsync();
  end Q;
  public static async method R(i: int): Task
    Program.Use(await Program.Q());
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

// Control (clean on base): #2750's own shape — a synchronous fault in the
// chained source, the frame's task `using var`-bound and awaited in a try.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_chained_return_issue_2750_shape_using_var_awaiter) {
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
    await Task.Delay(1);
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        using var t := Program.Q();
        var c := await t;
      catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Control (clean on base): the chained frame's task is awaited twice (§30.4).
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_chained_return_bound_frame_task_awaited_twice) {
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
    return await Program.BoomCellAsync();
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

// Control (clean on base): four concurrent frames each awaiting a chained
// faulted task anonymously.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_chained_return_four_concurrent_bound_frames) {
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
    return await Program.BoomCellAsync();
  end Q;
  public static async method R(i: int): Task
    Program.Use(await Program.Q());
  end R;
  public static async method Main(): Task
    for var i in 0..<25 do
      var t1 := Program.R(i);
      var t2 := Program.R(i + 1);
      var t3 := Program.R(i + 2);
      var t4 := Program.R(i + 3);
      try await t1; catch var e: Exception => Program.Tally(e.Message); end;
      try await t2; catch var e: Exception => Program.Tally(e.Message); end;
      try await t3; catch var e: Exception => Program.Tally(e.Message); end;
      try await t4; catch var e: Exception => Program.Tally(e.Message); end;
    end for;
    Console.WriteLine($"boom={K.boom} fboom={K.fboom} inner={K.inner} other={K.other} mark={K.mark} oda={K.oda} odt={K.odt} cells={Cell.n} xmade={XC.made} xgone={XC.gone}");
  end Main;
end Program;
)BZ",
              "boom=100 fboom=0 inner=0 other=0 mark=0 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}

// Control (clean on base): the instance crosses two chained returns.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_chained_return_through_two_chained_frames) {
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
    return await Program.BoomCellAsync();
  end Q;
  public static async method Q2(): Task<Cell>
    return await Program.Q();
  end Q2;
  public static async method R(i: int): Task
    var t := Program.Q2();
    var x := await t;
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

// Control (clean on base): the same chained return faults and succeeds on
// alternate calls — the success path's chain is undisturbed.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_chained_return_fault_and_success_alternate) {
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
  public static async method OkIntAsync(): Task<Cell>
    await Task.Delay(1);
    return new Cell();
  end OkIntAsync;
  public static async method QI(i: int): Task<Cell>
    if i mod 2 = 0 then
      return await Program.BoomCellAsync();
    end;
    return await Program.OkIntAsync();
  end QI;
  public static async method R(i: int): Task
    var t := Program.QI(i);
    var x := await t;
    K.mark := K.mark + 1;
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
              "boom=50 fboom=0 inner=0 other=0 mark=50 oda=0 odt=0 cells=0 xmade=50 xgone=50\n");
}

// Control (clean since the #7940 fix): the chained frame's fault is
// superseded in a catch arm, then the frame's task is awaited again.
BEZEL_TEST(escaped_borrowed_fault_7943,
           spec_30_9_chained_return_bound_supersede_then_reawait) {
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
    return await Program.BoomCellAsync();
  end Q;
  public static async method R(i: int): Task
    using var t := Program.Q();
    try
      try
        var a := await t;
      catch var e: Exception =>
        do
          K.mark := K.mark + 1;
          throw new ArgumentException("inner");
        end;
      end;
    catch var e3: ArgumentException =>
      Program.Tally(e3.Message);
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
              "boom=100 fboom=0 inner=100 other=0 mark=100 oda=0 odt=0 cells=0 xmade=100 xgone=100\n");
}
