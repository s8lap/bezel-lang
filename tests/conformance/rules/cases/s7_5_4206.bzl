//! case: s7_5_4206
//! doc: spec
//! section: 7.5
//! title: a null-conditional Count over a null ICollection initializes an int? local to null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable
//! cites: 7.5, stdlib 4.1
//! expect: run
//! output: -1
//!   3
//! issue: 8461
module Conf;

public class Program
  public static method Show(c: ICollection<int>?)
    var n: int? := c?.Count;
    Console.WriteLine(n ?? -1);
  end Show;

  public static method Main()
    Show(null);
    var xs: Array<int> := [7, 8, 9];
    Show(xs);
  end Main;
end Program;
