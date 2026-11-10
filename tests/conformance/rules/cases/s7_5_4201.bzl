//! case: s7_5_4201
//! doc: spec
//! section: 7.5
//! title: a null-conditional Count and Contains on a nullable Array perform the access or yield null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable
//! cites: 7.5, stdlib 4.4
//! expect: run
//! output: 3
//!   True
//!   -1
//!   False
module Conf;

public class Program
  public static method Probe(a: Array<int>?)
    Console.WriteLine(a?.Count ?? -1);
    Console.WriteLine(a?.Contains(8) ?? false);
  end Probe;

  public static method Main()
    var xs: Array<int> := [7, 8, 9];
    Probe(xs);
    Probe(null);
  end Main;
end Program;
