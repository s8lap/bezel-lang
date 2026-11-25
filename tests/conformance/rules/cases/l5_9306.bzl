//! case: l5_9306
//! doc: stdlib
//! section: 5
//! title: LINQ over a Dictionary projects each entry's Key and Value
//! rule: public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>
//! cites: 4.8, 5
//! expect: run
//! output: k a
//!   v 2
//! notes: Harvested from the #8542 review (p5): the control for the IGrouping element-type change. A
//!   `Dictionary<string, int>` is an `ICollection<DictionaryEntry<string, int>>`, so the lambda parameter of
//!   `Select` is a `DictionaryEntry` with `Key` and `Value` (`public record DictionaryEntry<K, V>(Key: K,
//!   Value: V)`). One entry only, so no hash order is asserted. Trace: entry ("a", 1); `Select(e.Key)` yields
//!   "a" and the program prints "k a"; `Select(e.Value * 2)` yields 2 and prints "v 2".
module Conf;
public class Program
  public static method Main(): int
    using var d := new Dictionary<string, int>();
    d.Add("a", 1);
    for var k in d.Select(lambda(e) => e.Key) do Console.WriteLine("k " + k); end for;
    for var v in d.Select(lambda(e) => e.Value * 2) do Console.WriteLine("v " + v.ToString()); end for;
    return 0;
  end Main;
end Program;
