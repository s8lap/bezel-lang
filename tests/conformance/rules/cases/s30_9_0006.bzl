//! case: s30_9_0006
//! doc: spec
//! section: 30.9
//! title: an outer task whose fault is never re-raised disposes the retained fault instance at its disposal
//! rule: An operand the frame **owns** — an unbound temporary (`await
//!   Inner()`), a plain owned local, an `owned` parameter — **moves** into the
//!   faulted task's state as a kind-2 chained source does (§30.5): the
//!   temporary's deferred terminal-act disposal, the local's discharge and the
//!   parameter's dispose-at-terminal-state do not run for it, and the faulted
//!   task's teardown disposes it
//! cites: 30.9, 30.5
//! expect: run
//! output: settled
//!   instance disposed
//!   after
//! issue: 5831
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message);
  end constructor;

  destructor
    Console.WriteLine("instance disposed");
  end destructor;
end BoomException;

public class Demo
  public static async method Inner(): Task
    await Task.Delay(10);
    throw new BoomException("boom");
  end Inner;

  public static async method Outer(): Task
    await Inner();
  end Outer;

  public static async method Main(): Task
    do
      using var o := Outer();
      var arr: Array<Task> := [o];
      _ := await Task.WhenAny(arr);
      Console.WriteLine("settled");
    end;
    Console.WriteLine("after");
  end Main;
end Demo;
