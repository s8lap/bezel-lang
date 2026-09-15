//! case: s30_10_0009
//! doc: spec
//! section: 30.10
//! title: WhenAll re-raises the first non-successful input in input order
//! rule: Awaiting a non-successful `Task.WhenAll` re-raises the exception of
//!   the **first non-successful input in input order**, per the
//!   first-fault-no-aggregation posture; the other inputs' faults remain
//!   observable on those tasks individually.
//! cites: 30.9
//! expect: run
//! output: first
module Conf;

public class Program
  public static async method FailAsync(tag: string): Task
    await Task.Delay(0);
    throw new InvalidOperationException(tag);
  end FailAsync;

  public static async method Main(): Task
    using var t1 := FailAsync("first");
    using var t2 := FailAsync("second");
    var a: Array<Task> := [t1, t2];
    try
      await Task.WhenAll(a);
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message)
    end try;
  end Main;
end Program;
