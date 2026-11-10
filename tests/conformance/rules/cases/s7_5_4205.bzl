//! case: s7_5_4205
//! doc: spec
//! section: 7.5
//! title: a null-conditional Contains on a nullable ICollection performs the access or yields null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable
//! cites: 7.5, stdlib 4.1
//! expect: run
//! output: True
//!   False
//!   False
//! issue: 8461
module Conf;

public class Program
  public static method Probe(c: ICollection<int>?)
    Console.WriteLine(c?.Contains(8) ?? false);
  end Probe;

  public static method Main()
    var xs: Array<int> := [7, 8, 9];
    Probe(xs);
    Probe(null);
    var ys: Array<int> := [1, 2];
    Probe(ys);
  end Main;
end Program;
