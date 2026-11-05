//! case: l5_1304
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull inside a lambda and a helper method answers the same as at the top level
//! rule: null when index is out of range
//! cites: 15.1
//! expect: run
//! output: 8
//!   9
//!   -1
//!   8
//!   -5
module Conf;

public class Program
  static method G(xs: Array<int>, i: int): int
    var r := xs.Skip(1).ElementAtOrNull(i);
    if r = null then
      return -5;
    end if;
    return r!;
  end G;

  public static method Main()
    using var xs := new Array<int>();
    xs.Add(7);
    xs.Add(8);
    xs.Add(9);
    var f := lambda(i: int) => xs.Where(lambda(x) => x > 7).ElementAtOrNull(i) ?? -1;
    Console.WriteLine(f(0));
    Console.WriteLine(f(1));
    Console.WriteLine(f(2));
    Console.WriteLine(G(xs, 0));
    Console.WriteLine(G(xs, 4));
  end Main;
end Program;
