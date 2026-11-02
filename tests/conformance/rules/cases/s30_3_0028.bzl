//! case: s30_3_0028
//! doc: spec
//! section: 30.3
//! title: Wait on an OwningTask of a completed owned result returns and IsCompleted is true
//! rule: they block the calling thread until the task completes
//! cites: 30.3, 30.4
//! expect: run
//! output: True
module Conf;

public class R
  public constructor()
  end constructor;
end R;

public class Program
  public static method Main(): int
    using var t := Task.FromOwnedResult(new R());
    t.Wait();
    Console.WriteLine(t.IsCompleted.ToString());
    return 0;
  end Main;
end Program;
