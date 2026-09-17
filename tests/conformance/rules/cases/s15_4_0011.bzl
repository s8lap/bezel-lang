//! case: s15_4_0011
//! doc: spec
//! section: 15.4
//! title: a colon inside a literal element is a compile error at a non-dictionary target
//! rule: The key-value pair syntax is recognized only when the target type is
//!   `Dictionary<K, V>` or `OwningDictionary<K, V>`; in any other target
//!   context a colon inside a literal element is a compile error (§15.4).
//! cites: 15.4
//! expect: reject
//! diagnostic: colon
module Conf;

public class Program
  public static method Run()
    using var xs: Array<int> := [1: 2];
    Console.WriteLine(xs.Count);
  end Run;
end Program;
