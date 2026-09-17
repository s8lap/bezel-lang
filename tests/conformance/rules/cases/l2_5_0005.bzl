//! case: l2_5_0005
//! doc: stdlib
//! section: 2.5
//! title: ParseOrNull returns null exactly where Parse raises
//! rule: DateTime.ParseOrNull(s) returns null exactly where Parse(s) raises;
//!   otherwise the parsed value.
//! cites: 1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(DateTime.ParseOrNull("15/03/2024") = null);
  end Main;
end Program;
