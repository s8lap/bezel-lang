//! case: l5_4705
//! doc: stdlib
//! section: 5
//! title: a Select over the GroupBy result sees each IGrouping so g.Count() counts group members
//! rule: public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>> where TKey:
//!   hashable;
//! cites: 3.1, 27.4
//! expect: run
//! output: 2
//!   1
module Conf;

public record Item(Name: string, N: int);

public class Program
  public static method Main()
    using var xs := new Array<Item>();
    xs.Add(new Item("a", 1));
    xs.Add(new Item("b", 2));
    xs.Add(new Item("a", 3));
    var counts := xs.GroupBy(lambda(i) => i.Name).Select(lambda(g) => g.Count()).ToArray();
    for var c in counts do
      Console.WriteLine(c.ToString());
    end for;
  end Main;
end Program;
