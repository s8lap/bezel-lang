//! case: l5_4708
//! doc: stdlib
//! section: 5
//! title: an IGrouping<string, Item> parameter accepts a group yielded by GroupBy
//! rule: public interface IGrouping<TKey, T>: IEnumerable<T>
//! cites: 4.1
//! expect: run
//! output: a 4
//!   b 2
module Conf;

public record Item(Name: string, N: int);

public class Program
  public static method Show(g: IGrouping<string, Item>)
    var sum := 0;
    for var i in g do
      sum := sum + i.N;
    end for;
    Console.WriteLine(g.Key + " " + sum.ToString());
  end Show;

  public static method Main()
    using var xs := new Array<Item>();
    xs.Add(new Item("a", 1));
    xs.Add(new Item("b", 2));
    xs.Add(new Item("a", 3));
    for var g in xs.GroupBy(lambda(i) => i.Name) do
      Program.Show(g);
    end for;
  end Main;
end Program;
