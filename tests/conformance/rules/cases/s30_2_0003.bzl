//! case: s30_2_0003
//! doc: spec
//! section: 30.2
//! title: awaiting a faulted task re-raises the exception
//! rule: If the task is faulted, `await` re-raises the exception.
//! cites: 30.2, 30.9
//! expect: run
//! stdout: boom
module Conf;

public class Program
  public static async method Main(): Task
    using var t := Task.FromException<int>(new InvalidOperationException("boom"));
    try
      var v := await t;
      Console.WriteLine(v);
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message)
    end try;
  end Main;
end Program;
