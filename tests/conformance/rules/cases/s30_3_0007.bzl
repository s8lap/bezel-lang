//! case: s30_3_0007
//! doc: spec
//! section: 30.3
//! title: Result on a completed faulted task re-raises the exception
//! rule: Reading `Result` on a **completed** `Task<T>` ... on a faulted task
//!   it re-raises the exception.
//! cites: 30.3, 30.9
//! expect: run
//! stdout: oops
module Conf;

public class Program
  public static method Main()
    using var t := Task.FromException<int>(new InvalidOperationException("oops"));
    try
      var v := t.Result;
      Console.WriteLine(v);
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message)
    end try;
  end Main;
end Program;
