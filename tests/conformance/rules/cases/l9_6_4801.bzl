//! case: l9_6_4801
//! doc: stdlib
//! section: 9.6
//! title: Array<string>.Sort() orders strings by the ordinal CompareTo, upper-case letters before lower-case
//! rule: public pure method CompareTo(other: string): int; // ordinal; satisfies IComparable<string>
//! cites: 4.4, 14.2
//! expect: run
//! output: Banana
//!   apple
//!   fig
//!   pear
module Conf;

public class Program
  public static method Main()
    var a: Array<string> := ["pear", "apple", "fig", "Banana"];
    a.Sort();
    Console.WriteLine(a[0]);
    Console.WriteLine(a[1]);
    Console.WriteLine(a[2]);
    Console.WriteLine(a[3]);
  end Main;
end Program;
