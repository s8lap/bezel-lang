//! case: l5_4704
//! doc: stdlib
//! section: 5
//! title: GroupBy with an untyped selector over a record key uses record equality for the grouping
//! rule: Groups appear in first-occurrence-of-key order; elements within a group in source order.
//! cites: 27.4, 2.10
//! expect: run
//! output: 1,2 n=2 sum=4
//!   3,4 n=1 sum=2
module Conf;

public record Pos(X: int, Y: int);
public record Item(P: Pos, N: int);

public class Program
  public static method Main()
    using var xs := new Array<Item>();
    xs.Add(new Item(new Pos(1, 2), 1));
    xs.Add(new Item(new Pos(3, 4), 2));
    xs.Add(new Item(new Pos(1, 2), 3));
    for var g in xs.GroupBy(lambda(i) => i.P) do
      var sum := 0;
      for var i in g do
        sum := sum + i.N;
      end for;
      Console.WriteLine(g.Key.X.ToString() + "," + g.Key.Y.ToString() + " n=" + g.Count().ToString() + " sum=" + sum.ToString());
    end for;
  end Main;
end Program;
