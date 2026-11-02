//! case: s30_3_0025
//! doc: spec
//! section: 30.3
//! title: Wait through a null-conditional on a nullable task returns
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 30.3
//! expect: run
//! output: ok
module Conf;

public class Program
  public static method Main(): int
    using var t0 := Task.FromResult(2);
    var t: Task<int>? := t0;
    t?.Wait();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
