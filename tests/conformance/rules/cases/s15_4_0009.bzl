//! case: s15_4_0009
//! doc: spec
//! section: 15.4
//! title: duplicate compile-time-constant dictionary keys are a compile error
//! rule: For `Dictionary<K, V>` and `OwningDictionary<K, V>`, duplicate keys
//!   are a compile error when all duplicate keys are compile-time constants
//!   (§15.4).
//! cites: 15.4
//! expect: reject
//! diagnostic: duplicate
module Conf;

public class Program
  public static method Run()
    using var ages: Dictionary<string, int> := ["alice": 30, "alice": 25];
    Console.WriteLine(ages.Count);
  end Run;
end Program;
