//! case: l4_17_9970
//! doc: stdlib
//! section: 4.17
//! title: Single() over a SortedDictionary answers the entry, so .Key and .Value read it
//! rule: public class SortedDictionary<K, V>:
//!   IReadOnlyCollection<DictionaryEntry<K, V>>
//! cites: 4.17, 5
//! expect: run
//! issue: 9290
//! stdout: 5 50
module Conf;

public class Program
  public static method Main()
    using var sd := new SortedDictionary<int, int>();
    sd.Add(5, 50);
    var e := sd.Single();
    Console.WriteLine($"{e.Key} {e.Value}");
  end Main;
end Program;
