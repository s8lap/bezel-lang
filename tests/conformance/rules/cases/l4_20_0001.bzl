//! case: l4_20_0001
//! doc: stdlib
//! section: 4.20
//! title: Increment on an absent key creates it at delta
//! rule: method Increment(key: K, delta: int := 1); // absent key is created
//!   at delta.
//! cites: 4.20
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    using var tally := new Counter<string>();
    tally.Increment("a");
    Console.WriteLine(tally.CountOf("a"));
  end Main;
end Program;
