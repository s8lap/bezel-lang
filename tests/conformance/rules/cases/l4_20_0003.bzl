//! case: l4_20_0003
//! doc: stdlib
//! section: 4.20
//! title: Contains distinguishes a present-at-zero key from an absent one
//! rule: method Contains(key: K): bool; // distinguishes present-at-zero
//!   from absent; enables key in tally.
//! cites: 4.20
//! expect: run
//! stdout: True False
module Conf;

public class Program
  public static method Main()
    using var tally := new Counter<string>();
    tally.Increment("a", 0);
    var presentAtZero := tally.Contains("a");
    var absent := tally.Contains("missing");
    Console.WriteLine($"{presentAtZero} {absent}");
  end Main;
end Program;
