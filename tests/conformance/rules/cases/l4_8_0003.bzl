//! case: l4_8_0003
//! doc: stdlib
//! section: 4.8
//! title: TryAdd reports false and makes no mutation on a duplicate key
//! rule: TryAdd(key, value): bool; // v0.282: false, no mutation if the
//!   key is present.
//! cites: 4.8, 1
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    d.Add("a", 1);
    var ok := d.TryAdd("a", 2);
    Console.WriteLine(ok);
  end Main;
end Program;
