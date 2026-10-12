//! case: s30_3_0006
//! doc: spec
//! section: 30.3
//! title: IsCompleted, IsFaulted and IsCancelled read the task's terminal state
//! rule: `public readonly property IsCompleted: bool;` /
//!   `IsFaulted` / `IsCancelled` on `Task` — a task produced by
//!   `Task.FromResult` is already completed; `FromException` is faulted and
//!   `FromCancelled` cancelled.
//! cites: 30.3, 30.10
//! expect: run
//! output: True
//!   True
//!   True
module Conf;

public class Program
  public static async method Main(): Task
    using var a := Task.FromResult(1);
    Console.WriteLine(a.IsCompleted);
    using var b := Task.FromException<int>(new InvalidOperationException("x"));
    Console.WriteLine(b.IsFaulted);
    using var c := Task.FromCancelled<int>();
    Console.WriteLine(c.IsCancelled);
  end Main;
end Program;
