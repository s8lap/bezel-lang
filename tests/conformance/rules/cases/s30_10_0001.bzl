//! case: s30_10_0001
//! doc: spec
//! section: 30.10
//! title: Task.WhenAll completes only after every input task has settled
//! rule: `Task.WhenAll` (all input forms) completes only after **every**
//!   input has settled — it never abandons laggards. If every input
//!   succeeded it completes successfully.
//! cites: 30.10
//! expect: run
//! output: step
//!   step
//!   done
module Conf;

public class Program
  public static async method StepAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("step");
  end StepAsync;

  public static async method Main(): Task
    using var t1 := StepAsync();
    using var t2 := StepAsync();
    var a: Array<Task> := [t1, t2];
    await Task.WhenAll(a);
    Console.WriteLine("done");
  end Main;
end Program;
