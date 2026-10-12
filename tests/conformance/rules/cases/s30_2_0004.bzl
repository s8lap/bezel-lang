//! case: s30_2_0004
//! doc: spec
//! section: 30.2
//! title: awaiting a cancelled task raises OperationCanceledException
//! rule: If the task was cancelled, `await` raises `OperationCanceledException`.
//! cites: 30.2, 30.3
//! expect: run
//! stdout: cancelled
module Conf;

public class Program
  public static async method Main(): Task
    using var t := Task.FromCancelled<int>();
    try
      var v := await t;
      Console.WriteLine(v);
    catch
      var ex: OperationCanceledException => Console.WriteLine("cancelled")
    end try;
  end Main;
end Program;
