//! case: s30_9_0005
//! doc: spec
//! section: 30.9
//! title: a fault escaping into an outer task stays live for every awaiter of the outer task
//! rule: An instance that an `await` or a `Result` read re-raised from a task
//!   ... and that then leaves the body ... faults the body's task, which does
//!   **not** become its owner: the instance stays owned by the task that owned
//!   it, the faulted task carries it as a **borrow**, and the faulted task
//!   **retains the operand task** whose fault it carries
//! cites: 30.9, 30.4
//! expect: run
//! output: boom
//!   boom
//! issue: 5831
module Conf;

public class Demo
  public static async method Inner(): Task
    await Task.Delay(10);
    throw new InvalidOperationException("boom");
  end Inner;

  public static async method Outer(): Task
    await Inner();
  end Outer;

  public static async method Main(): Task
    using var o := Outer();
    for var i in 1..2 do
      try
        await o;
      catch
        var e: InvalidOperationException => Console.WriteLine(e.Message)
      end try;
    end;
  end Main;
end Demo;
