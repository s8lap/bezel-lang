//! case: l5_4709
//! doc: stdlib
//! section: 5
//! title: ToArray over GroupBy preserves the groups with their keys and members
//! rule: Groups appear in first-occurrence-of-key order; elements within a group in source order.
//! cites: 5.1, 27.4
//! expect: run
//! output: a 4
//!   b 2
module Conf;

public record Item(Name: string, N: int);

public class Program
  public static method Main()
    using var xs := new Array<Item>();
    xs.Add(new Item("a", 1));
    xs.Add(new Item("b", 2));
    xs.Add(new Item("a", 3));
    var gs := xs.GroupBy(lambda(i) => i.Name).ToArray();
    for var g in gs do
      var sum := 0;
      for var i in g do
        sum := sum + i.N;
      end for;
      Console.WriteLine(g.Key + " " + sum.ToString());
    end for;
  end Main;
end Program;
