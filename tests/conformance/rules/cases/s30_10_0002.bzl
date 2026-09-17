//! case: s30_10_0002
//! doc: spec
//! section: 30.10
//! title: Task.WhenAny yields the index of the settled task
//! rule: `Task.WhenAny(tasks: Array<Task>): Task<int>` — completes when the
//!   first task completes, yielding the **index** of the winner in
//!   `tasks`.
//! cites: 30.10
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static async method Main(): Task
    using var t1 := Task.Delay(0);
    var a: Array<Task> := [t1];
    var idx := await Task.WhenAny(a);
    Console.WriteLine(idx);
  end Main;
end Program;
