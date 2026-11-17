//! case: s30_11_0022
//! doc: spec
//! section: 30.11
//! title: a user class named TaskCompletionSource with an int? Task property is not the stdlib source
//! rule: **Task-object ownership.** The **source owns the task object** its `Task` property
//!   exposes.
//! cites: 30.11, 17.8, 28.3
//! expect: run
//! stdout: 5
//! issue: 8671
module Conf;

public class TaskCompletionSource
  public property Task: int? => 5;
end TaskCompletionSource;

public class Program
  public static method Get(): int?
    using var tcs := new TaskCompletionSource();
    return tcs.Task!;
  end Get;

  public static method Main()
    Console.WriteLine(Get()!);
  end Main;
end Program;
