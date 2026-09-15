//! case: s30_10_0004
//! doc: spec
//! section: 30.10
//! title: WhenAny of an empty array raises ArgumentException synchronously
//! rule: `WhenAny` of an **empty** array raises `ArgumentException`
//!   **synchronously at the call site**, not as a faulted task.
//! cites: 30.10
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static async method Main(): Task
    var a: Array<Task> := [];
    try
      var idx := await Task.WhenAny(a);
    catch
      var ex: ArgumentException => Console.WriteLine("caught")
    end try;
  end Main;
end Program;
