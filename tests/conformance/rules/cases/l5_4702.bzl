//! case: l5_4702
//! doc: stdlib
//! section: 5
//! title: GroupBy with an untyped selector over an enum key yields groups in first-occurrence order
//! rule: Groups appear in first-occurrence-of-key order; elements within a group in source order.
//! cites: 27.4, 26.5
//! expect: run
//! output: red n=2 sum=4
//!   blue n=1 sum=2
module Conf;

public enum Color
  Red, Green, Blue;
end Color;

public record Item(C: Color, N: int);

public class Program
  public static method Main()
    using var xs := new Array<Item>();
    xs.Add(new Item(Color.Red, 1));
    xs.Add(new Item(Color.Blue, 2));
    xs.Add(new Item(Color.Red, 3));
    for var g in xs.GroupBy(lambda(i) => i.C) do
      var tag := "other";
      if g.Key = Color.Red then
        tag := "red";
      end if;
      if g.Key = Color.Blue then
        tag := "blue";
      end if;
      var sum := 0;
      for var i in g do
        sum := sum + i.N;
      end for;
      Console.WriteLine(tag + " n=" + g.Count().ToString() + " sum=" + sum.ToString());
    end for;
  end Main;
end Program;
