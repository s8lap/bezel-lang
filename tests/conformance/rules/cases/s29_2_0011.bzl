//! case: s29_2_0011
//! doc: spec
//! section: 29.2
//! title: for over a Dictionary yields DictionaryEntry
//! rule: `Dictionary<K,V>` produces a `DictionaryEntry<K,V>` for the for
//!   variable; `entry.Value` borrows at class V.
//! cites: 29.2
//! expect: run
//! stdout: 55
module Conf;

public class Program
  public static method Main()
    using var ages: Dictionary<string, int> := ["alice": 30, "bob": 25];
    var total := 0;
    for var entry in ages do
      total := total + entry.Value;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
