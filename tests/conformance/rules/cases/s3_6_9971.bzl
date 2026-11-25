//! case: s3_6_9971
//! doc: spec
//! section: 3.6
//! title: a Set<string?> holds one null and collapses a repeated string; Count, Contains and Remove agree
//! rule: such a type's `Equals` and `GetHashCode` are `T`'s, through §8.4's
//!   null-safe wrapper — which hashes `null` to one fixed
//!   implementation-defined value
//! cites: 3.6, 8.4, 4.10
//! expect: run
//! issue: 9277
//! output: True
//!   False
//!   True
//!   False
//!   2
//!   True
//!   True
//!   False
//!   1
module Conf;

public class Program
  public static method Main()
    var s := new Set<string?>();
    Console.WriteLine(s.TryAdd("a"));
    Console.WriteLine(s.TryAdd("a"));
    Console.WriteLine(s.TryAdd(null));
    Console.WriteLine(s.TryAdd(null));
    Console.WriteLine(s.Count);
    Console.WriteLine(s.Contains("a"));
    Console.WriteLine(s.Remove("a"));
    Console.WriteLine(s.Contains("a"));
    Console.WriteLine(s.Count);
  end Main;
end Program;
