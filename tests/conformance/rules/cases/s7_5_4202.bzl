//! case: s7_5_4202
//! doc: spec
//! section: 7.5
//! title: a null-conditional Count on a nullable Dictionary counts its entries or yields null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable
//! cites: 7.5, stdlib 4.8
//! expect: run
//! output: 2
//!   -1
module Conf;

public class Program
  public static method Probe(d: Dictionary<int, int>?)
    Console.WriteLine(d?.Count ?? -1);
  end Probe;

  public static method Main()
    using var m := new Dictionary<int, int>();
    m.Add(1, 10);
    m.Add(2, 20);
    Probe(m);
    Probe(null);
  end Main;
end Program;
