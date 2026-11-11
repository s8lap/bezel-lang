//! case: l5_4707
//! doc: stdlib
//! section: 5
//! title: GroupBy composes over a lazy Where and over a Select-ed projection
//! rule: Groups appear in first-occurrence-of-key order; elements within a group in source order.
//! cites: 5.1, 27.4
//! expect: run
//! output: b 1
//!   a 1
//!   len 1 3
module Conf;

public record Item(Name: string, N: int);

public class Program
  public static method Main()
    using var xs := new Array<Item>();
    xs.Add(new Item("a", 1));
    xs.Add(new Item("b", 2));
    xs.Add(new Item("a", 3));
    for var g in xs.Where(lambda(i) => i.N > 1).GroupBy(lambda(i) => i.Name) do
      Console.WriteLine(g.Key + " " + g.Count().ToString());
    end for;
    for var g in xs.OrderBy(lambda(i) => i.N).Select(lambda(i) => i.Name).GroupBy(lambda(s) => s.Length) do
      Console.WriteLine("len " + g.Key.ToString() + " " + g.Count().ToString());
    end for;
  end Main;
end Program;
