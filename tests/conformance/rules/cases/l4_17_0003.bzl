//! case: l4_17_0003
//! doc: stdlib
//! section: 4.17
//! title: TryAdd on a present key returns false and does not mutate
//! rule: method TryAdd(key: K, value: V): bool; // v0.282 (shape (d)): false
//!   and no mutation if present.
//! cites: 4.17
//! expect: run
//! stdout: False a
module Conf;

public class Program
  public static method Main()
    using var sd := new SortedDictionary<int, string>();
    sd.Add(1, "a");
    var added := sd.TryAdd(1, "b");
    Console.WriteLine($"{added} {sd[1]}");
  end Main;
end Program;
