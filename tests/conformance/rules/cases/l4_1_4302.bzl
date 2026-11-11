//! case: l4_1_4302
//! doc: stdlib
//! section: 4.1
//! title: Count reads the element count through an IReadOnlyCollection-typed receiver using the dot spelling
//! rule: pure readonly property Count: int;
//! cites: 4.1, 4.4, 26.5
//! expect: run
//! output: 3
//!   1
//! issue: 8461
module Conf;

public class Program
  public static method Size(r: IReadOnlyCollection<int>): int
    return r.Count;
  end Size;

  public static method Main()
    var xs: Array<int> := [7, 8, 9];
    var ys: Array<int> := [5];
    Console.WriteLine(Size(xs));
    Console.WriteLine(Size(ys));
  end Main;
end Program;
