//! case: s30_3_0026
//! doc: spec
//! section: 30.3
//! title: Wait on an already-completed task may be called again
//! rule: they block the calling thread until the task completes
//! cites: 30.3
//! expect: run
//! output: 2
module Conf;

public class Program
  public static method Main(): int
    using var t := Task.FromResult(2);
    t.Wait();
    t.Wait();
    Console.WriteLine(t.Result.ToString());
    return 0;
  end Main;
end Program;
