//! case: s3_6_9970
//! doc: spec
//! section: 3.6
//! title: a Set<int?> holds one null and collapses a repeated value; Count, Contains and Remove agree
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
    var s := new Set<int?>();
    Console.WriteLine(s.TryAdd(1));
    Console.WriteLine(s.TryAdd(1));
    Console.WriteLine(s.TryAdd(null));
    Console.WriteLine(s.TryAdd(null));
    Console.WriteLine(s.Count);
    Console.WriteLine(s.Contains(null));
    Console.WriteLine(s.Remove(null));
    Console.WriteLine(s.Contains(null));
    Console.WriteLine(s.Count);
  end Main;
end Program;
