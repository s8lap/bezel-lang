//! case: s7_5_4207
//! doc: spec
//! section: 7.5
//! title: a null-conditional Count on a nullable IReadOnlyCollection performs the access or yields null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable
//! cites: 7.5, stdlib 4.1
//! expect: run
//! output: 3
//!   -1
//! issue: 8461
module Conf;

public class Program
  public static method Probe(c: IReadOnlyCollection<int>?)
    Console.WriteLine(c?.Count ?? -1);
  end Probe;

  public static method Main()
    var xs: Array<int> := [7, 8, 9];
    Probe(xs);
    Probe(null);
  end Main;
end Program;
