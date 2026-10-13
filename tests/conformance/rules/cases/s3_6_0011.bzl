//! case: s3_6_0011
//! doc: spec
//! section: 3.6
//! title: a nullable hashable type is a hash-container element and null hashes to one value
//! rule: (c) a nullable type `T?` ... where `T` is itself not hashable — such
//!   a type's `Equals` and `GetHashCode` are `T`'s, through §8.4's null-safe
//!   wrapper — which hashes `null` to one fixed implementation-defined value
//! cites: 3.6, 8.4
//! expect: run
//! output: True
//!   False
//!   True
module Conf;

public class Program
  public static method Main()
    var seen := new Set<int?>();
    Console.WriteLine(seen.TryAdd(null));
    Console.WriteLine(seen.TryAdd(null));
    Console.WriteLine(seen.TryAdd(7));
  end Main;
end Program;
