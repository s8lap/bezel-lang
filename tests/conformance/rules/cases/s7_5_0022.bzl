//! case: s7_5_0022
//! doc: spec
//! section: 7.5
//! title: a null-conditional read of Array.Count is the Count property: the count, or null for a null receiver
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable
//! cites: 7.5, 4.4
//! expect: run
//! output: 3
//!   -1
module Conf;

public class Program
  public static method CountOf(a: Array<int>?): int
    var f := a?.Count;
    return f ?? -1;
  end CountOf;

  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    Console.WriteLine(CountOf(xs));
    Console.WriteLine(CountOf(null));
  end Main;
end Program;
