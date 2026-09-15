//! case: l9_3_0003
//! doc: stdlib
//! section: 9.3
//! title: decimal.ParseOrNull returns null exactly where Parse raises
//! rule: ParseOrNull(s: string): decimal? — v0.231: null exactly where
//!   Parse(s) raises; otherwise the parsed value.
//! cites: 9.3, 1
//! expect: run
//! stdout: null
module Conf;

public class Program
  public static method Main()
    var n := decimal.ParseOrNull("nope");
    Console.WriteLine($"{n}");
  end Main;
end Program;
