//! case: l5_9971
//! doc: stdlib
//! section: 5
//! title: the entry Single() answers type-checks at a member-access, argument and return position
//! rule: public class SortedDictionary<K, V>:
//!   IReadOnlyCollection<DictionaryEntry<K, V>>
//! cites: 4.17, 5
//! expect: run
//! issue: 9290
//! output: 5 50
//!   100
//!   50
module Conf;

public class Program
  public static method Show(e: DictionaryEntry<int, int>): int
    return e.Key * 10 + e.Value;
  end Show;

  public static method Pick(sd: SortedDictionary<int, int>): int
    return sd.Single().Value;
  end Pick;

  public static method Main()
    using var sd := new SortedDictionary<int, int>();
    sd.Add(5, 50);
    Console.WriteLine($"{sd.Single().Key} {sd.Single().Value}");
    Console.WriteLine(Show(sd.Single()));
    Console.WriteLine(Pick(sd));
  end Main;
end Program;
