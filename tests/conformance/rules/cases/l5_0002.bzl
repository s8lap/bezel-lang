//! case: l5_0002
//! doc: stdlib
//! section: 5
//! title: a bound lazy query enumerated twice is a compile error
//! rule: A lazy query is single-consumption -- it may be enumerated at most
//!   once; a second enumeration is a compile error where the flow analysis
//!   can see it statically.
//! cites: 5, 29.11
//! expect: reject
//! diagnostic: single-consumption
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    using var q := xs.Where(lambda(x: int): bool => x > 1);
    for var v in q do
      Console.WriteLine(v);
    end;
    for var v2 in q do
      Console.WriteLine(v2);
    end;
  end Main;
end Program;
