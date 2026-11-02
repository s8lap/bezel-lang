//! case: s30_3_0024
//! doc: spec
//! section: 30.3
//! title: Wait on a completed task in a non-async Main returns and Result is readable
//! rule: they block the calling thread until the task completes
//! cites: 30.3, 30.5
//! expect: run
//! output: 2
module Conf;

public class Program
  public static method Main(): int
    using var t := Task.FromResult(2);
    t.Wait();
    Console.WriteLine(t.Result.ToString());
    return 0;
  end Main;
end Program;
