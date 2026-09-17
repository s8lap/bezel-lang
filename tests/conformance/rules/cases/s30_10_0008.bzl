//! case: s30_10_0008
//! doc: spec
//! section: 30.10
//! title: Task.FireAndForget accepts the task and relinquishes ownership
//! rule: `Task.FireAndForget(owned task: Task)` registers a continuation that
//!   logs or handles any exception the task produces, then disposes the task;
//!   the calling code relinquishes ownership.
//! cites: 30.10
//! expect: run
//! output: done
module Conf;

public class Program
  public static async method RunAsync(): Task
    await Task.Delay(0);
  end RunAsync;

  public static async method Main(): Task
    Task.FireAndForget(RunAsync());
    Console.WriteLine("done");
  end Main;
end Program;
