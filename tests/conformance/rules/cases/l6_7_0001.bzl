//! case: l6_7_0001
//! doc: stdlib
//! section: 6.7
//! title: WhenAny yields the index of the first task to settle and never re-raises its fault
//! rule: WhenAny(tasks: Array<Task>): Task<int>; // index of the first
//!   task to SETTLE — success, fault, or cancellation; never re-raises
//!   the winner's fault (inspect tasks[i]).
//! cites: 6.7
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static async method Main(): Task
    using var t0 := Task.FromException<int>(new InvalidOperationException("x"));
    using var t1 := Task.Delay(2000);
    var arr: Array<Task> := [t0, t1];
    var idx := await Task.WhenAny(arr);
    Console.WriteLine(idx);
  end Main;
end Program;
