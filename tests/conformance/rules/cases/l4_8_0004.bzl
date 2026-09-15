//! case: l4_8_0004
//! doc: stdlib
//! section: 4.8
//! title: TryAdd leaves the existing value unchanged on a duplicate key
//! rule: TryAdd(key, value): bool; // false, no mutation if the key is
//!   present — the twin observation to l4_8_0003's boolean result.
//! cites: 4.8, 1
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    d.Add("a", 1);
    _ := d.TryAdd("a", 2);
    Console.WriteLine(d["a"]);
  end Main;
end Program;
