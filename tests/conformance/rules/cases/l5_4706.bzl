//! case: l5_4706
//! doc: stdlib
//! section: 5
//! title: GroupBy with an untyped selector over an IList receiver
//! rule: public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>> where TKey:
//!   hashable;
//! cites: 4.1, 27.4
//! expect: run
//! output: a n=2 sum=4
//!   b n=1 sum=2
module Conf;

public record Item(Name: string, N: int);

public class Program
  public static method Run(l: IList<Item>)
    for var g in l.GroupBy(lambda(i) => i.Name) do
      var sum := 0;
      for var i in g do
        sum := sum + i.N;
      end for;
      Console.WriteLine(g.Key + " n=" + g.Count().ToString() + " sum=" + sum.ToString());
    end for;
  end Run;

  public static method Main()
    using var xs := new Array<Item>();
    xs.Add(new Item("a", 1));
    xs.Add(new Item("b", 2));
    xs.Add(new Item("a", 3));
    Program.Run(xs);
  end Main;
end Program;
