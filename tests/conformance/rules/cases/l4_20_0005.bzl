//! case: l4_20_0005
//! doc: stdlib
//! section: 4.20
//! title: Counter<K> iterates its entries in insertion order
//! rule: Counter<K> implements IEnumerable<DictionaryEntry<K, int>>;
//!   iteration is insertion-ordered, exactly as Dictionary.
//! cites: 4.20
//! expect: run
//! stdout: b
module Conf;

public class Program
  public static method Main()
    using var tally := new Counter<string>();
    tally.Increment("b");
    tally.Increment("a");
    for var e in tally do
      Console.WriteLine(e.Key);
    end;
  end Main;
end Program;
