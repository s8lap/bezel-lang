//! case: s7_5_0023
//! doc: spec
//! section: 7.5
//! title: a null-conditional Array.LastIndexOf call yields its result on a non-null receiver, null otherwise
//! rule: returns null if `x` is null; otherwise it performs the access
//! cites: 7.5, 4.4
//! expect: run
//! output: 2
//!   -9
module Conf;

public class Program
  public static method Probe(a: Array<int>?): int
    return a?.LastIndexOf(1) ?? -9;
  end Probe;

  public static method Main()
    var xs: Array<int> := [1, 2, 1];
    Console.WriteLine(Probe(xs));
    Console.WriteLine(Probe(null));
  end Main;
end Program;
