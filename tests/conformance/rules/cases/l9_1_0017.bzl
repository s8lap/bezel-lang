//! case: l9_1_0017
//! doc: stdlib
//! section: 9.1
//! title: Parse(s, radix) accepts an optional leading sign
//! rule: stdlib §9.1: the radix forms read "an optional leading `+` or `-`,
//!     then one or more digits of that radix, and nothing else".
//! cites: 9.1
//! expect: run
//! stdout: -255
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("-ff", 16));
  end Main;
end Program;
