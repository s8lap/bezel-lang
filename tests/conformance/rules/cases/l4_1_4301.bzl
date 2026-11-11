//! case: l4_1_4301
//! doc: stdlib
//! section: 4.1
//! title: Contains reads true and false through an ICollection-typed receiver using the dot spelling
//! rule: method Contains(item: T): bool;
//! cites: 4.1, 4.4, 26.5
//! expect: run
//! output: True
//!   False
//! issue: 8461
module Conf;

public class Program
  public static method Has(c: ICollection<int>, x: int): bool
    return c.Contains(x);
  end Has;

  public static method Main()
    var xs: Array<int> := [7, 8, 9];
    Console.WriteLine(Has(xs, 8));
    Console.WriteLine(Has(xs, 1));
  end Main;
end Program;
