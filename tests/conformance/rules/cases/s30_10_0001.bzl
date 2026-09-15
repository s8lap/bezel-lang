//! case: s30_10_0001
//! doc: spec
//! section: 30.10
//! title: Task.WhenAll completes when all input tasks complete
//! rule: `Task.WhenAll(tasks: Array<Task>): Task` — completes when all
//!   tasks complete.
//! cites: 30.10
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static async method Main(): Task
    using var t1 := Task.Delay(0);
    using var t2 := Task.Delay(0);
    var a: Array<Task> := [t1, t2];
    await Task.WhenAll(a);
    Console.WriteLine("done");
  end Main;
end Program;
