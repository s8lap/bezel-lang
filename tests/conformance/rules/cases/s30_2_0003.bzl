//! case: s30_2_0003
//! doc: spec
//! section: 30.2
//! title: awaiting a completed task returns synchronously and observably
//! rule: If the task is already completed, `await` returns the result
//!   synchronously.
//! cites: 30.2
//! expect: run
//! stdout: 7
module Conf;

public class Program
  public static async method Main(): Task
    var n := await Task.FromResult(7);
    Console.WriteLine(n);
  end Main;
end Program;
