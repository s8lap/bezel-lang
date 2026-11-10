//! case: s7_5_4203
//! doc: spec
//! section: 7.5
//! title: a null-conditional Contains and Count on a nullable IReadOnlyList perform the access or yield null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable
//! cites: 7.5, 26.5, stdlib 4.1, stdlib 5
//! expect: run
//! output: True
//!   3
//!   False
//!   -1
//! issue: 7557
module Conf;

public class Program
  public static method Probe(ro: IReadOnlyList<int>?)
    Console.WriteLine(ro?.Contains(8) ?? false);
    Console.WriteLine(ro?.Count ?? -1);
  end Probe;

  public static method Main()
    var xs: Array<int> := [7, 8, 9];
    Probe(xs);
    Probe(null);
  end Main;
end Program;
