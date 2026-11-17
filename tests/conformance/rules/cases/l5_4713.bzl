//! case: l5_4713
//! doc: stdlib
//! section: 5
//! title: Count() on an IGrouping, which declares no Count member, reaches
//!   the LINQ operator
//! rule: `g.Count()` followed by `g.Average(…)` is defined and reads the
//!   buffer twice
//! cites: 5, 1
//! expect: run
//! output: 3
//!   2
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3, 4, 5];
    for var g in xs.GroupBy(lambda(x: int): int => x mod 2) do
      Console.WriteLine(g.Count());
    end;
  end Main;
end Program;
