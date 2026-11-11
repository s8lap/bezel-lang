//! case: l5_4710
//! doc: stdlib
//! section: 5
//! title: a GroupBy key selector that names a member the element type lacks is rejected
//! rule: public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>> where TKey:
//!   hashable;
//! cites: 27.4, 26.5
//! expect: reject
//! diagnostic: NoSuch
module Conf;

public record Item(Name: string, N: int);

public class Program
  public static method Main()
    using var xs := new Array<Item>();
    xs.Add(new Item("a", 1));
    for var g in xs.GroupBy(lambda(i) => i.NoSuch) do
      Console.WriteLine("x");
    end for;
  end Main;
end Program;
