//! case: s30_3_0004
//! doc: spec
//! section: 30.3
//! title: IsCompleted reads True on an already-completed task
//! rule: `public class Task { public readonly property IsCompleted: bool;
//!   ... }` — a task produced by `Task.FromResult` is already completed.
//! cites: 30.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    using var t := Task.FromResult(9);
    Console.WriteLine(t.IsCompleted);
  end Main;
end Program;
