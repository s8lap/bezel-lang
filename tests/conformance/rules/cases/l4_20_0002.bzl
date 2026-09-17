//! case: l4_20_0002
//! doc: stdlib
//! section: 4.20
//! title: CountOf returns 0 for an absent key
//! rule: method CountOf(key: K): int; // 0 when absent.
//! cites: 4.20
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    using var tally := new Counter<string>();
    Console.WriteLine(tally.CountOf("missing"));
  end Main;
end Program;
