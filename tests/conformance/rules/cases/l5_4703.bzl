//! case: l5_4703
//! doc: stdlib
//! section: 5
//! title: GroupBy with an untyped selector over a string key groups in first-occurrence order
//! rule: Groups appear in first-occurrence-of-key order; elements within a group in source order.
//! cites: 27.4
//! expect: run
//! output: a n=2 sum=4
//!   b n=1 sum=2
module Conf;

public record Item(Name: string, N: int);

public class Program
  public static method Main()
    using var xs := new Array<Item>();
    xs.Add(new Item("a", 1));
    xs.Add(new Item("b", 2));
    xs.Add(new Item("a", 3));
    for var g in xs.GroupBy(lambda(i) => i.Name) do
      var sum := 0;
      for var i in g do
        sum := sum + i.N;
      end for;
      Console.WriteLine(g.Key + " n=" + g.Count().ToString() + " sum=" + sum.ToString());
    end for;
  end Main;
end Program;
