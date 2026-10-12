//! case: s30_3_0008
//! doc: spec
//! section: 30.3
//! title: Result on a completed cancelled task raises OperationCanceledException
//! rule: Reading `Result` on a **completed** `Task<T>` ... on a cancelled
//!   task it raises `OperationCanceledException`.
//! cites: 30.3, 30.2
//! expect: run
//! stdout: cancelled
module Conf;

public class Program
  public static method Main()
    using var t := Task.FromCancelled<int>();
    try
      var v := t.Result;
      Console.WriteLine(v);
    catch
      var ex: OperationCanceledException => Console.WriteLine("cancelled")
    end try;
  end Main;
end Program;
