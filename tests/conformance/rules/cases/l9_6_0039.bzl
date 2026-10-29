//! case: l9_6_0039
//! doc: stdlib
//! section: 9.6
//! title: string.Repeat over a non-empty unit with a count of zero is the empty string
//! rule: public static pure method Repeat(unit: string, count: int): string; // same count contract;
//!   the char overload's contract states count = 0 is "", and the unit overload declares the same count contract
//! cites: 9.6
//! expect: run
//! stdout: 0
//! issue: 7129
//! notes: the non-empty unit at count 0. The char overload is pinned by l9_6_0011. The unit
//!   overload's length for count > 0 is not stated in prose, so the case observes only count 0.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(string.Repeat("ab", 0).Length);
  end Main;
end Program;
