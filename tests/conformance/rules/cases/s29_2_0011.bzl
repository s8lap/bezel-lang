//! case: s29_2_0011
//! doc: spec
//! section: 29.2
//! title: for over a Dictionary yields DictionaryEntry
//! rule: The desugaring binds `var x := __enumerator.Current;`, so the
//!   iteration variable's type is the source's `IEnumerable<T>` element type.
//!   `Dictionary<K, V>` conforms as `ICollection<DictionaryEntry<K, V>>`, so
//!   iterating one yields `DictionaryEntry<K, V>` values, one per live entry,
//!   in insertion order (stdlib §4.8).
//! cites: 29.1, 29.2
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
