//! case: l4_17_0001
//! doc: stdlib
//! section: 4.17
//! title: SortedDictionary<K, V> iterates entries in ascending key order regardless of insertion order
//! rule: A comparison-ordered associative collection: iteration visits
//!   entries in ascending key order.
//! cites: 4.17
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    using var sd := new SortedDictionary<int, string>();
    sd.Add(3, "c");
    sd.Add(1, "a");
    sd.Add(2, "b");
    for var e in sd do
      Console.WriteLine(e.Key);
    end;
  end Main;
end Program;
