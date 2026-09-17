//! case: l9_1_0003
//! doc: stdlib
//! section: 9.1
//! title: int.ParseOrNull returns null exactly where Parse raises
//! rule: ParseOrNull(s: string): int? — null exactly where Parse(s)
//!   raises (the FormatException/OverflowException contract); otherwise
//!   the parsed value.
//! cites: 9.1, 1
//! expect: run
//! stdout: null
module Conf;

public class Program
  public static method Main()
    var n := int.ParseOrNull("xyz");
    Console.WriteLine($"{n}");
  end Main;
end Program;
