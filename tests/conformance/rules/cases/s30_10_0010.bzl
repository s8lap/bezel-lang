//! case: s30_10_0010
//! doc: spec
//! section: 30.10
//! title: a cancelled input counts as OperationCanceledException for WhenAll
//! rule: Awaiting a non-successful `Task.WhenAll` re-raises the exception of
//!   the first non-successful input in input order — a cancelled input
//!   counting as `OperationCanceledException`.
//! cites: 30.3
//! expect: run
//! output: cancelled
module Conf;

public class Program
  public static async method FailAsync(tag: string): Task
    await Task.Delay(0);
    throw new InvalidOperationException(tag);
  end FailAsync;

  public static async method Main(): Task
    using var t1 := Task.FromCancelled();
    using var t2 := FailAsync("second");
    var a: Array<Task> := [t1, t2];
    try
      await Task.WhenAll(a);
    catch
      OperationCanceledException => Console.WriteLine("cancelled"),
      var ex: InvalidOperationException => Console.WriteLine(ex.Message)
    end try;
  end Main;
end Program;
